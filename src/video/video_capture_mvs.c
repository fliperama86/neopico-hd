/**
 * MVS Video Capture Module
 * PIO-based line-by-line capture from Neo Geo MVS
 *
 * Streaming architecture: captures lines directly to ring buffer
 */

#include "pico/stdlib.h"
#include "pico/sync.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "video_capture.h"
#if NEOPICO_EXP_GENLOCK_DYNAMIC
#include "hardware/timer.h"
#endif

#include <stdint.h>
#include <stdlib.h>

#include "audio_subsystem.h"
#include "hardware_config.h"
#include "line_ring.h"
#include "mvs_pins.h"
#include "pico.h"
#include "tusb.h"
#include "video_capture_mvs.pio.h"

#if NEOPICO_DIAG_COUNTERS
#include <stdio.h>
line_ring_diag_t g_line_ring_diag;

// Non-blocking 1 Hz dump of capture-health counters over USB-CDC. Called from
// the inter-frame gap on Core 0. Skips entirely if no host is reading (or the
// CDC TX buffer is full) so it can never stall capture timing.
static void video_capture_diag_tick(uint32_t input_frames)
{
    static uint32_t last_ms = 0;
    static uint32_t l_in = 0, l_out = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((now - last_ms) < 1000U) {
        return;
    }
    last_ms = now;
    uint32_t out = g_line_ring_diag.out_frames;
    // NOTWR/OVR/SYNCRST are printed CUMULATIVE (absolute) so any single line read
    // gives the running totals — robust to dropped/stalled CDC lines. Baseline = 0,
    // so any non-zero means events have occurred since boot.
    char buf[120];
    int n = snprintf(buf, sizeof buf, "[%lu] in=%lu(+%lu) out=%lu(+%lu) NOTWR=%lu OVR=%lu SYNCRST=%lu\r\n",
                     (unsigned long)now, (unsigned long)input_frames, (unsigned long)(input_frames - l_in),
                     (unsigned long)out, (unsigned long)(out - l_out), (unsigned long)g_line_ring_diag.not_written,
                     (unsigned long)g_line_ring_diag.overrun, (unsigned long)g_line_ring_diag.sync_resets);
    // Gate ONLY on TX buffer room (non-blocking) — NOT on tud_cdc_connected(), whose
    // DTR state is unreliable with macOS cu.* devices and stalls the stream.
    if (n > 0 && (int)tud_cdc_write_available() >= n) {
        tud_cdc_write(buf, (uint32_t)n);
        tud_cdc_write_flush();
    }
    l_in = input_frames;
    l_out = out;
}
#endif

// =============================================================================
// Feature Flags
// =============================================================================

// DARK/SHADOW processing.
// When enabled, uses a compact four-state split LUT with a compile-time color
// model. The feature remains default-off until its capture timing is verified.
#ifndef ENABLE_DARK_SHADOW
#define ENABLE_DARK_SHADOW 0
#endif

// =============================================================================
// MVS Timing Constants
// =============================================================================

#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE 320
#define H_SKIP_START 28
#define H_SKIP_END 36
#define V_SKIP_LINES 16
#define NEO_V_ACTIVE 224
#define MVS_CAPTURE_PIO_TARGET_HZ 126000000U

// PIO IRQ index: sync SM raises this on every line push for event-driven vsync (no polling)
#define MVS_SYNC_IRQ_INDEX 0
// No-signal timeout: only used to detect cable unplug / loss of signal; normal path is IRQ-driven
#define MVS_NO_SIGNAL_TIMEOUT_MS 100

// =============================================================================
// State
// =============================================================================

static uint g_mvs_height = 0;

static PIO g_pio_mvs = NULL;
static uint g_sm_sync = 0;
static uint g_offset_sync = 0;
static uint g_sm_pixel = 0;
static uint g_offset_pixel = 0;

static int g_dma_chan = -1;
static uint32_t g_line_buffers[2][NEO_H_TOTAL]; // Ping-pong buffers for line capture

static volatile uint32_t g_frame_count = 0;

#if NEOPICO_EXP_GENLOCK_DYNAMIC
volatile uint32_t g_mvs_vsync_timestamp = 0;
#endif

static int g_skip_start_words = 0;
static int g_active_words = 0;
static int g_line_words = 0;
static float g_capture_pio_clkdiv = 1.0F;

// Semaphore released by sync IRQ handler when vsync is detected (one release per frame)
static semaphore_t g_vsync_sem;

// =============================================================================
// Pixel Conversion
// =============================================================================

#include "mvs_color.h"

#if NEOPICO_MVS_COLOR_MODEL_MENU
#include "mvs_color_model.h"
#include "settings.h"

#if ENABLE_DARK_SHADOW
#error "The normal-color model selector must remain separate from DARK/SHADOW processing"
#endif

static uint32_t g_requested_color_model = MVS_COLOR_MODEL_DIGITAL;
static volatile bool g_sync_decoder_reset_requested;

void video_capture_set_color_model(mvs_color_model_t model)
{
    const uint32_t requested = mvs_color_model_is_valid(model) ? (uint32_t)model : MVS_COLOR_MODEL_DIGITAL;
    __atomic_store_n(&g_requested_color_model, requested, __ATOMIC_RELEASE);
}

mvs_color_model_t video_capture_get_color_model(void)
{
    const uint32_t requested = __atomic_load_n(&g_requested_color_model, __ATOMIC_ACQUIRE);
    return mvs_color_model_is_valid(requested) ? (mvs_color_model_t)requested : MVS_COLOR_MODEL_DIGITAL;
}
#endif

#if ENABLE_DARK_SHADOW
#include "mvs_effect_lut.h"

// Four independent states in captured-bit order: normal, SHADOW, DARK, both.
// The split R/G-plus-B layout is exact for the selected model and uses 8,448
// bytes instead of a 256 KiB flat four-state RGB565 LUT.
static mvs_effect_lut_t g_capture_effect_lut __attribute__((aligned(4)));

static void generate_capture_lut(void)
{
    mvs_effect_lut_generate(&g_capture_effect_lut);
}

static inline uint16_t mvs_capture_lut_lookup(uint32_t raw)
{
    return mvs_effect_lut_lookup_raw(&g_capture_effect_lut, raw);
}
#else
// 32K LUT: corrected RGB555 -> RGB565 (DARK/SHADOW disabled build).
#if NEOPICO_MVS_COLOR_MODEL_MENU
// Keep both normal-color tables so Core 0 can switch one base pointer at VSYNC
// without adding work to the per-pixel loop.
static uint16_t g_color_correct_lut[MVS_COLOR_MODEL_COUNT][32768] __attribute__((aligned(4)));
#else
static uint16_t g_color_correct_lut[32768] __attribute__((aligned(4)));
#endif

static void generate_color_correct_lut(void)
{
    for (uint32_t idx = 0; idx < 32768U; idx++) {
        uint32_t r5 = mvs_correct_5bit((idx >> 10) & 0x1F, MVS_INVERT_R, MVS_REVERSE_R);
        uint32_t g5 = mvs_correct_5bit((idx >> 5) & 0x1F, MVS_INVERT_G, MVS_REVERSE_G);
        uint32_t b5 = mvs_correct_5bit(idx & 0x1F, MVS_INVERT_B, MVS_REVERSE_B);
        if (MVS_BLACK_LEVEL_CLAMP > 0 && r5 <= MVS_BLACK_LEVEL_CLAMP && g5 <= MVS_BLACK_LEVEL_CLAMP &&
            b5 <= MVS_BLACK_LEVEL_CLAMP) {
#if NEOPICO_MVS_COLOR_MODEL_MENU
            g_color_correct_lut[MVS_COLOR_MODEL_DIGITAL][idx] = 0;
            g_color_correct_lut[MVS_COLOR_MODEL_ANALOG][idx] = 0;
#else
            g_color_correct_lut[idx] = 0;
#endif
        } else {
#if NEOPICO_MVS_COLOR_MODEL_MENU
            g_color_correct_lut[MVS_COLOR_MODEL_DIGITAL][idx] =
                mvs_color_model_pack_rgb565(MVS_COLOR_MODEL_DIGITAL, r5, g5, b5);
            g_color_correct_lut[MVS_COLOR_MODEL_ANALOG][idx] =
                mvs_color_model_pack_rgb565(MVS_COLOR_MODEL_ANALOG, r5, g5, b5);
#else
            g_color_correct_lut[idx] = mvs_pack_rgb565(r5, g5, b5);
#endif
        }
    }
}
#endif

#if ENABLE_DARK_SHADOW
static inline uint16_t convert_pixel(uint32_t raw)
{
    return mvs_capture_lut_lookup(raw);
}

static inline void convert_active_pixels(uint16_t *dst, const uint32_t *src, int count)
{
    int remaining = count;
    while (remaining >= 4) {
        dst[0] = mvs_capture_lut_lookup(src[0]);
        dst[1] = mvs_capture_lut_lookup(src[1]);
        dst[2] = mvs_capture_lut_lookup(src[2]);
        dst[3] = mvs_capture_lut_lookup(src[3]);
        dst += 4;
        src += 4;
        remaining -= 4;
    }
    while (remaining-- > 0) {
        *dst++ = mvs_capture_lut_lookup(*src++);
    }
}
#elif NEOPICO_MVS_COLOR_MODEL_MENU
static inline uint16_t convert_pixel(const uint16_t *color_lut, uint32_t raw)
{
    uint32_t color15 = ((raw >> 2) & 0x7FFF) ^ (MVS_RAW_COLOR_MASK & 0x7FFF);
#if MVS_REVERSE_15BIT
    color15 = mvs_reverse_15(color15);
#endif
    return color_lut[color15];
}

static inline void convert_active_pixels(uint16_t *dst, const uint32_t *src, int count, const uint16_t *color_lut)
{
    for (int i = 0; i < count; i++) {
        dst[i] = convert_pixel(color_lut, src[i]);
    }
}
#else
static inline uint16_t convert_pixel(uint32_t raw)
{
    uint32_t color15 = ((raw >> 2) & 0x7FFF) ^ (MVS_RAW_COLOR_MASK & 0x7FFF);
#if MVS_REVERSE_15BIT
    color15 = mvs_reverse_15(color15);
#endif
    return g_color_correct_lut[color15];
}

static inline void convert_active_pixels(uint16_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        dst[i] = convert_pixel(src[i]);
    }
}
#endif

// =============================================================================
// MVS Sync Detection
// =============================================================================

static inline void drain_sync_fifo(PIO pio, uint sm)
{
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }
}

// Runs in IRQ context: one word per line from sync PIO; detect vsync (long pulse after 8 short) and release semaphore
static void sync_irq_handler(void)
{
    pio_interrupt_clear(g_pio_mvs, MVS_SYNC_IRQ_INDEX);
    if (pio_sm_is_rx_fifo_empty(g_pio_mvs, g_sm_sync))
        return;

    uint32_t h_ctr = pio_sm_get(g_pio_mvs, g_sm_sync);
    bool is_short_pulse = (h_ctr <= H_THRESHOLD);

    static uint32_t equ_count = 0;
    static bool in_vsync = false;

#if NEOPICO_MVS_COLOR_MODEL_MENU
    if (g_sync_decoder_reset_requested) {
        equ_count = 0;
        in_vsync = false;
        g_sync_decoder_reset_requested = false;
    }
#endif

    if (!in_vsync) {
        if (is_short_pulse) {
            equ_count++;
        } else {
            if (equ_count >= 8)
                in_vsync = true;
            equ_count = 0;
        }
    } else {
        if (is_short_pulse) {
            equ_count++;
        } else {
            equ_count = 0;
            in_vsync = false;
            sem_release(&g_vsync_sem);
        }
    }
}

// =============================================================================
// Hardware Reset
// =============================================================================

static void video_capture_reset_hardware(void)
{
    // 1. Disable both state machines to stop any pending operations
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, false);
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);

    // 2. Clear FIFOs to remove any stale data
    pio_sm_clear_fifos(g_pio_mvs, g_sm_sync);
    pio_sm_clear_fifos(g_pio_mvs, g_sm_pixel);

    // 3. Reset PCs and re-initialize state
    pio_sm_restart(g_pio_mvs, g_sm_sync);
    pio_sm_restart(g_pio_mvs, g_sm_pixel);

    // Jump sync SM to entry point and reset X register (counter)
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_jmp(g_offset_sync));
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_set(pio_x, 0));

    // Jump pixel SM to entry point (the PULL instruction)
    pio_sm_exec(g_pio_mvs, g_sm_pixel, pio_encode_jmp(g_offset_pixel));

    // Re-enable SMs
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, true);
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

    // 4. Re-feed the pixel count to the Pixel SM
    pio_sm_put_blocking(g_pio_mvs, g_sm_pixel, NEO_H_TOTAL - 1);

    // 5. Clear any pending trigger IRQ and sync data-ready IRQ
    pio_interrupt_clear(g_pio_mvs, 4);
    pio_interrupt_clear(g_pio_mvs, MVS_SYNC_IRQ_INDEX);
}

#if NEOPICO_MVS_COLOR_MODEL_MENU
static void video_capture_resync_after_settings_save(void)
{
    // Core 0 was paused while flash XIP was unavailable. Discard any queued
    // sync lines and restart both capture state machines from a clean phase.
    irq_set_enabled(PIO1_IRQ_0, false);
    sem_reset(&g_vsync_sem, 0);
    g_sync_decoder_reset_requested = true;
    __dmb();
    video_capture_reset_hardware();
    irq_set_enabled(PIO1_IRQ_0, true);
}
#endif

// =============================================================================
// Public API
// =============================================================================

void video_capture_init(uint mvs_height)
{
    g_mvs_height = mvs_height;

#if ENABLE_DARK_SHADOW
    generate_capture_lut();
#else
    generate_color_correct_lut();
#endif

    // 19-bit capture: 1 pixel per word
    g_skip_start_words = H_SKIP_START;
    g_active_words = NEO_H_ACTIVE;
    g_line_words = NEO_H_TOTAL;
    g_capture_pio_clkdiv = (float)clock_get_hz(clk_sys) / (float)MVS_CAPTURE_PIO_TARGET_HZ;
    if (g_capture_pio_clkdiv < 1.0F) {
        g_capture_pio_clkdiv = 1.0F;
    }

    // Initialize PIO blocks
    pio_clear_instruction_memory(pio0);
    pio_clear_instruction_memory(pio1);
    pio_set_gpio_base(pio0, 0);
    pio_set_gpio_base(pio1, 0);

    g_pio_mvs = pio1;

    // 1. Force PIO1 GPIOBASE to 16 (pin index N = GP(N+16); GP27 = index 11)
    *(volatile uint32_t *)((uintptr_t)g_pio_mvs + 0x168) = 16;

    // 2. Add programs
    pio_clear_instruction_memory(g_pio_mvs);
    g_offset_sync = pio_add_program(g_pio_mvs, &mvs_sync_4a_program);
    g_offset_pixel = pio_add_program(g_pio_mvs, &mvs_pixel_capture_dark19_program);

    // 3. Claim SMs
    g_sm_sync = (uint)pio_claim_unused_sm(g_pio_mvs, true);
    g_sm_pixel = (uint)pio_claim_unused_sm(g_pio_mvs, true);

    // 4. Setup capture GPIOs GP27-45 (CSYNC, PCLK, B, G, R, SHADOW, DARK)
    for (uint i = PIN_MVS_BASE; i <= MVS_CAPTURE_PIN_LAST; i++) {
        pio_gpio_init(g_pio_mvs, i);
        gpio_disable_pulls(i);
        gpio_set_input_enabled(i, true);
        gpio_set_input_hysteresis_enabled(i, true);
    }

    // 5. Configure Sync SM: CSYNC = GP27 (pin index 11 with GPIOBASE=16)
    pio_sm_config c = mvs_sync_4a_program_get_default_config(g_offset_sync);
    sm_config_set_clkdiv(&c, g_capture_pio_clkdiv);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(g_pio_mvs, g_sm_sync, g_offset_sync, &c);

    uint pin_idx_sync = PIN_MVS_BASE - 16; // 11: GP27 = CSYNC (wait/jmp pin)
    g_pio_mvs->sm[g_sm_sync].pinctrl = (g_pio_mvs->sm[g_sm_sync].pinctrl & ~0x000f8000) | (pin_idx_sync << 15);
    g_pio_mvs->sm[g_sm_sync].execctrl = (g_pio_mvs->sm[g_sm_sync].execctrl & ~0x1f000000) | (pin_idx_sync << 24);

    // 6. Configure Pixel SM: IN_BASE = GP27 (pin index 11), capture GP27-45
    pio_sm_config pc = mvs_pixel_capture_dark19_program_get_default_config(g_offset_pixel);
    sm_config_set_clkdiv(&pc, g_capture_pio_clkdiv);
    sm_config_set_in_shift(&pc, false, true, MVS_CAPTURE_BITS);
    pio_sm_init(g_pio_mvs, g_sm_pixel, g_offset_pixel, &pc);

    uint pin_idx_pixel = PIN_MVS_BASE - 16; // 11: first pin of capture window
    g_pio_mvs->sm[g_sm_pixel].pinctrl = (g_pio_mvs->sm[g_sm_pixel].pinctrl & ~0x000f8000) | (pin_idx_pixel << 15);

    // 7. Start
    pio_interrupt_clear(g_pio_mvs, 4);
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_set(pio_x, 0));
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, true);
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

    // 7a. Initialize Pixel SM with pixel count
    pio_sm_put_blocking(g_pio_mvs, g_sm_pixel, NEO_H_TOTAL - 1);

    // 8. Configure DMA for Async Pixel Capture
    g_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(g_dma_chan);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(g_pio_mvs, g_sm_pixel, false));
    dma_channel_configure(g_dma_chan, &dc, g_line_buffers[0], &g_pio_mvs->rxf[g_sm_pixel], 0, false);

    // 9. Sync IRQ: event-driven vsync (no polling). Sync SM raises IRQ 0 on every line push.
    sem_init(&g_vsync_sem, 0, 2);
    pio_interrupt_clear(g_pio_mvs, MVS_SYNC_IRQ_INDEX);
    g_pio_mvs->inte0 |= (1U << MVS_SYNC_IRQ_INDEX);
    irq_set_exclusive_handler(PIO1_IRQ_0, sync_irq_handler);
    irq_set_enabled(PIO1_IRQ_0, true);
}

// Genlock: 60 Hz = 126 MHz sysclk, ~59.18 Hz = 124.1 MHz (nominal MVS)
#define SYS_CLK_60HZ_KHZ 126000
#define SYS_CLK_GENLOCK_KHZ 124100

void video_capture_run(void)
{
    bool audio_rearm_on_next_signal = false;
#if NEOPICO_MVS_COLOR_MODEL_MENU
    const uint16_t *frame_color_lut = g_color_correct_lut[MVS_COLOR_MODEL_DIGITAL];
#endif

    // One-time: wait for first vsync (IRQ-driven) then drain FIFO for clean phase
    if (sem_acquire_timeout_ms(&g_vsync_sem, 500)) {
        drain_sync_fifo(g_pio_mvs, g_sm_sync);
    }

    while (1) {
        g_frame_count++;

        if (!sem_acquire_timeout_ms(&g_vsync_sem, MVS_NO_SIGNAL_TIMEOUT_MS)) {
#if NEOPICO_DIAG_COUNTERS
            g_line_ring_diag.sync_resets++;
#endif
            audio_rearm_on_next_signal = true;
#if NEOPICO_MVS_COLOR_MODEL_MENU
            if (settings_service_pending_save()) {
                video_capture_resync_after_settings_save();
            } else {
                video_capture_reset_hardware();
            }
#else
            video_capture_reset_hardware();
#endif
            tud_task();
            continue;
        }

        if (audio_rearm_on_next_signal) {
            audio_subsystem_request_rearm();
            audio_rearm_on_next_signal = false;
        }

#if NEOPICO_MVS_COLOR_MODEL_MENU
        // Read the cross-core request exactly once per frame. All active lines
        // in this frame therefore use the same LUT base pointer.
        uint32_t frame_color_model = __atomic_load_n(&g_requested_color_model, __ATOMIC_ACQUIRE);
        if (!mvs_color_model_is_valid(frame_color_model)) {
            frame_color_model = MVS_COLOR_MODEL_DIGITAL;
        }
        frame_color_lut = g_color_correct_lut[frame_color_model];
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC
        g_mvs_vsync_timestamp = timer_hw->timerawl;
#endif
        // Signal VSYNC to Core 1
        line_ring_vsync();

        drain_sync_fifo(g_pio_mvs, g_sm_sync);

        // Reset Pixel Capture SM for frame alignment
        pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);
        pio_sm_clear_fifos(g_pio_mvs, g_sm_pixel);
        pio_sm_exec(g_pio_mvs, g_sm_pixel, pio_encode_jmp(g_offset_pixel + 2));
        pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

        // Prepare first DMA
        dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
        dma_channel_set_write_addr(g_dma_chan, g_line_buffers[0], true);

        // Trigger capture
        pio_interrupt_clear(g_pio_mvs, 4);
        pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_irq_set(false, 4));

        // Skip V border lines
        for (int skip = 0; skip < V_SKIP_LINES; skip++) {
            dma_channel_wait_for_finish_blocking(g_dma_chan);
            dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
            dma_channel_set_write_addr(g_dma_chan, g_line_buffers[0], true);
        }

        // Capture active lines into ring buffer
        uint8_t buf_idx = 0;
        for (uint16_t line = 0; line + 1 < g_mvs_height; line++) {
            uint16_t *dst = line_ring_write_ptr(line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            // Start next DMA (ping-pong)
            uint32_t *buf = g_line_buffers[buf_idx];
            buf_idx ^= 1U;
            dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
            dma_channel_set_write_addr(g_dma_chan, g_line_buffers[buf_idx], true);

            // Convert pixels directly to ring buffer
            uint32_t *src = buf + g_skip_start_words;
#if NEOPICO_MVS_COLOR_MODEL_MENU
            convert_active_pixels(dst, src, g_active_words, frame_color_lut);
#else
            convert_active_pixels(dst, src, g_active_words);
#endif

            // Signal line ready
            line_ring_commit(line + 1);
        }

        // Last active line (no next DMA to schedule)
        if (g_mvs_height > 0) {
            uint16_t last_line = (uint16_t)(g_mvs_height - 1);
            uint16_t *dst = line_ring_write_ptr(last_line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            uint32_t *src = g_line_buffers[buf_idx] + g_skip_start_words;
#if NEOPICO_MVS_COLOR_MODEL_MENU
            convert_active_pixels(dst, src, g_active_words, frame_color_lut);
#else
            convert_active_pixels(dst, src, g_active_words);
#endif

            line_ring_commit(g_mvs_height);
        }

#if NEOPICO_DIAG_COUNTERS
        video_capture_diag_tick(g_frame_count);
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
        // Persist only after a complete input frame. This pauses capture for a
        // rare flash operation while Core 1 continues outputting the last frame.
        if (settings_service_pending_save()) {
            video_capture_resync_after_settings_save();
        }
#endif
    }
}

uint32_t video_capture_get_frame_count(void)
{
    return g_frame_count;
}

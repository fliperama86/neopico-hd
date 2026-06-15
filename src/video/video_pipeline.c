#include "video_pipeline.h"

#include "hardware/irq.h"
#include "hardware/timer.h"

#if NEOPICO_USE_NONRT_HDMI
#include "pico_hdmi/video_output.h"
#else
#include "pico_hdmi/video_output_rt.h"
#endif

#include <string.h>

#include "line_ring.h"
#if NEOPICO_SETTINGS_FLASH
#include "settings.h"
#endif
#if NEOPICO_ENABLE_OSD
#include "osd/fast_osd.h"
#endif
#if NEOPICO_EXP_REBOOT_MODE_SWITCH
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#endif
#include "pico.h"
#include "video_config.h"
#if NEOPICO_EXP_GENLOCK_DYNAMIC && !NEOPICO_USE_NONRT_HDMI
#include "pico_hdmi/video_output_rt.h"

#include "hardware/timer.h"

#include "video_capture.h"
#endif

#ifndef NEOPICO_VIDEO_TEST_PATTERN
#define NEOPICO_VIDEO_TEST_PATTERN 0
#endif

#ifndef NEOPICO_VIDEO_240P
#define NEOPICO_VIDEO_240P 0
#endif

#ifndef NEOPICO_VIDEO_720P
#define NEOPICO_VIDEO_720P 0
#endif

#ifndef NEOPICO_EXP_REBOOT_MODE_SWITCH
#define NEOPICO_EXP_REBOOT_MODE_SWITCH 0
#endif

#ifndef NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
#define NEOPICO_EXP_REBOOT_MODE_SWITCH_720P 0
#endif

#ifndef NEOPICO_EXP_RAM_OSD_APPLY_PATH
#define NEOPICO_EXP_RAM_OSD_APPLY_PATH 0
#endif

#if NEOPICO_EXP_REBOOT_MODE_SWITCH && NEOPICO_USE_NONRT_HDMI
#error "NEOPICO_EXP_REBOOT_MODE_SWITCH requires the rt PicoHDMI path"
#endif

#if NEOPICO_EXP_REBOOT_MODE_SWITCH && NEOPICO_VIDEO_720P
#error "Use NEOPICO_EXP_REBOOT_MODE_SWITCH_720P for 720p reboot switching; do not combine with NEOPICO_VIDEO_720P"
#endif

// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;
#if NEOPICO_ENABLE_OSD
static bool osd_visible_latched = false;
#endif
typedef void (*pixel_scale_fn_t)(uint32_t *dst, const uint16_t *src, int count);
// Overscan/background outside active 224-line image area (RGB565): black.
#define OVERSCAN_COLOR_RGB565 0x0000
// No-signal fallback color (RGB565): mid gray.
#define NO_SIGNAL_COLOR_RGB565 0x7BEF

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
static void __scratch_x("000_video_pipeline_480p")
    video_pipeline_scanline_callback_480p(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
static void video_pipeline_scanline_callback_reboot_modes(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);
#endif
#endif

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
static video_pipeline_reboot_mode_t reboot_requested_mode =
    (NEOPICO_VIDEO_240P != 0) ? VIDEO_PIPELINE_REBOOT_MODE_240P : VIDEO_PIPELINE_REBOOT_MODE_480P;
#define REBOOT_MODE_BOOT_MAGIC 0x4e505253U
#define REBOOT_MODE_BOOT_CHECK_XOR 0xa5a55a5aU
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
#define REBOOT_MODE_MAX VIDEO_PIPELINE_REBOOT_MODE_720P
#else
#define REBOOT_MODE_MAX VIDEO_PIPELINE_REBOOT_MODE_240P
#endif

static inline uint32_t reboot_mode_boot_check(uint32_t mode)
{
    return REBOOT_MODE_BOOT_MAGIC ^ mode ^ REBOOT_MODE_BOOT_CHECK_XOR;
}
#elif NEOPICO_VIDEO_720P
#define VIDEO_PIPELINE_H_WORDS (1280U / 2U)
#define VIDEO_PIPELINE_H_SCALE 3U
#define VIDEO_PIPELINE_SCALE_PIXELS(dst, src, count) video_pipeline_triple_pixels_fast((dst), (src), (count))
static inline bool video_pipeline_map_active_line(uint32_t active_line, uint32_t *fb_line)
{
    if ((active_line % 3U) != 0U) {
        return false;
    }
    *fb_line = active_line / 3U;
    return true;
}
#elif NEOPICO_VIDEO_240P
#define VIDEO_PIPELINE_H_WORDS (1280U / 2U)
#define VIDEO_PIPELINE_H_SCALE 4U
#define VIDEO_PIPELINE_SCALE_PIXELS(dst, src, count) video_pipeline_quadruple_pixels_fast((dst), (src), (count))
static inline bool video_pipeline_map_active_line(uint32_t active_line, uint32_t *fb_line)
{
    *fb_line = active_line;
    return true;
}
#else
#define VIDEO_PIPELINE_H_WORDS (640U / 2U)
#define VIDEO_PIPELINE_H_SCALE 2U
#define VIDEO_PIPELINE_SCALE_PIXELS(dst, src, count) video_pipeline_double_pixels_fast((dst), (src), (count))
static inline bool video_pipeline_map_active_line(uint32_t active_line, uint32_t *fb_line)
{
    *fb_line = active_line >> 1;
    return true;
}
#endif

#define VIDEO_PIPELINE_IMAGE_WORDS ((LINE_WIDTH * VIDEO_PIPELINE_H_SCALE) / 2U)
#define VIDEO_PIPELINE_X_MARGIN_WORDS                                                                                  \
    ((VIDEO_PIPELINE_H_WORDS > VIDEO_PIPELINE_IMAGE_WORDS)                                                             \
         ? ((VIDEO_PIPELINE_H_WORDS - VIDEO_PIPELINE_IMAGE_WORDS) / 2U)                                                \
         : 0U)
#if NEOPICO_ENABLE_OSD
#define VIDEO_PIPELINE_OSD_X_WORDS                                                                                     \
    (VIDEO_PIPELINE_X_MARGIN_WORDS + (((uint32_t)OSD_BOX_X * VIDEO_PIPELINE_H_SCALE) / 2U))
#define VIDEO_PIPELINE_OSD_W_WORDS (((uint32_t)OSD_BOX_W * VIDEO_PIPELINE_H_SCALE) / 2U)
#endif

static void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
    __attribute__((noinline, noclone));

static void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
{
    const uint32_t packed = ((uint32_t)color << 16) | color;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = packed;
    }
}

#if NEOPICO_VIDEO_TEST_PATTERN
static uint16_t test_pattern_line[LINE_WIDTH] __attribute__((aligned(4)));
static bool test_pattern_line_ready = false;

static void video_pipeline_init_test_pattern_line(void)
{
    static const uint16_t colors[] = {
        0x0000, // black
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFE0, // yellow
        0xF81F, // magenta
        0x07FF, // cyan
        0xFFFF, // white
    };
    const uint32_t color_count = (uint32_t)(sizeof(colors) / sizeof(colors[0]));
    for (uint32_t x = 0; x < LINE_WIDTH; x++) {
        test_pattern_line[x] = colors[(x * color_count) / LINE_WIDTH];
    }
    test_pattern_line_ready = true;
}
#endif

#if NEOPICO_EXP_PRECOMPOSED_HDMI
// ===========================================================================
// pico_hdmi 2.0 precomposed + native-pixel path (EXPERIMENTAL, 480p non-RT).
// The scanline ISR shrinks to a pointer lookup: video rows return the
// captured native 320px line DIRECTLY (zero copy -- the bus write replicates
// each halfword and the HSTX expander doubles it to 640), constant rows
// return static color lines, and OSD rows return a native-width compose
// scratch refreshed once per source line. Audio data islands are patched
// into precomposed line headers by the library's scanline ISR itself, so
// audio cannot be starved by Core 1 background work.
// ===========================================================================

static video_output_precomposed_line_t precomposed_ring[8];

#if !NEOPICO_VIDEO_720P
static uint32_t overscan_native_line[LINE_WIDTH / 2];
static uint32_t nosignal_native_line[LINE_WIDTH / 2];

#if NEOPICO_ENABLE_OSD
// Ping/pong native-width scratch for rows where the OSD box overlays video.
// Composed once per source line (the callback fires twice per line-doubled
// pair); invalidated each frame from the vsync callback.
static uint16_t osd_native_scratch[2][LINE_WIDTH];
static volatile uint16_t osd_native_scratch_line = 0xFFFF;
#endif

static const uint32_t *__scratch_x("000_video_pipeline_precomp")
    video_pipeline_scanline_ptr_callback(uint32_t v_scanline, uint32_t active_line)
{
    (void)v_scanline;
    const uint32_t fb_line = active_line >> 1;

#if NEOPICO_ENABLE_OSD
    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    if (osd_visible_latched && osd_line_u32 < OSD_BOX_H) {
        uint16_t *scratch = osd_native_scratch[fb_line & 1];
        if (osd_native_scratch_line != (uint16_t)fb_line) {
            const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
            const uint16_t *src = NULL;
            if (mvs_line_u32 < MVS_HEIGHT && line_ring_ready((uint16_t)mvs_line_u32)) {
                src = line_ring_read_ptr((uint16_t)mvs_line_u32);
            }
            // Explicit word loops: this runs in DMA-ISR context from
            // __scratch_x, so no library memcpy (flash) calls.
            uint32_t *d32 = (uint32_t *)scratch;
            if (src) {
                const uint32_t *s32 = (const uint32_t *)src;
                for (uint32_t i = 0; i < LINE_WIDTH / 2; i++) {
                    d32[i] = s32[i];
                }
            } else {
                const uint32_t fill = NO_SIGNAL_COLOR_RGB565 | ((uint32_t)NO_SIGNAL_COLOR_RGB565 << 16);
                for (uint32_t i = 0; i < LINE_WIDTH / 2; i++) {
                    d32[i] = fill;
                }
            }
            // OSD box is halfword-aligned but OSD_BOX_X may be odd-width
            // positioned; copy as halfwords for safety.
            const uint16_t *osd_src = osd_framebuffer[osd_line_u32];
            for (uint32_t i = 0; i < OSD_BOX_W; i++) {
                scratch[OSD_BOX_X + i] = osd_src[i];
            }
            osd_native_scratch_line = (uint16_t)fb_line;
        }
        return (const uint32_t *)scratch;
    }
#endif

    const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
    if (mvs_line_u32 >= MVS_HEIGHT) {
        return overscan_native_line;
    }
    if (!line_ring_ready((uint16_t)mvs_line_u32)) {
        return nosignal_native_line;
    }
    return (const uint32_t *)line_ring_read_ptr((uint16_t)mvs_line_u32);
}
#endif // !NEOPICO_VIDEO_720P

#if NEOPICO_VIDEO_720P
// ===========================================================================
// 720p precomposed: 1280-wide lines are 3x the native 320, which the 2x
// bus-replication hardware cannot express, so lines are PRE-EXPANDED into a
// ring OUTSIDE the ISR (background prep). The pillarbox is baked in at
// init; prep writes only the 960 px center. The OSD composes at native
// resolution BEFORE tripling, so its cost is resolution-independent. A
// stale ring entry shows as a transient tear, never a desync.
// ===========================================================================

#define P720_RING_LINES 16
#define P720_LINE_WORDS (1280 / 2)
#define P720_CENTER_WORDS ((LINE_WIDTH * 3) / 2)                      // 480
#define P720_PILLAR_WORDS ((P720_LINE_WORDS - P720_CENTER_WORDS) / 2) // 80

static uint32_t p720_ring[P720_RING_LINES][P720_LINE_WORDS];
static uint16_t p720_native_scratch[LINE_WIDTH];
static volatile uint16_t p720_scan_source_line;
static volatile uint16_t p720_frame_seq;       // bumped at vsync; alarm context owns the cursors
static uint16_t p720_prep_line = FRAME_HEIGHT; // nothing prepped yet

static const uint32_t *__scratch_x("000_video_pipeline_p720")
    video_pipeline_p720_ptr_callback(uint32_t v_scanline, uint32_t active_line)
{
    (void)v_scanline;
    const uint32_t src_line = active_line / 3U;
    p720_scan_source_line = (uint16_t)src_line;
    return p720_ring[src_line % P720_RING_LINES];
}

static void video_pipeline_p720_fill_center(uint32_t *line, uint16_t color)
{
    const uint32_t fill = color | ((uint32_t)color << 16);
    for (uint32_t i = 0; i < P720_CENTER_WORDS; i++) {
        line[P720_PILLAR_WORDS + i] = fill;
    }
}

static uint32_t p720_gray_mask[(FRAME_HEIGHT + 31) / 32];
static uint16_t p720_retry_line;

// Returns false when the capture line was not yet committed (gray written);
// such lines are retried before the beam reaches them.
static bool video_pipeline_p720_prep_one(uint16_t fb_line)
{
    uint32_t *line = p720_ring[fb_line % P720_RING_LINES];
    const uint32_t mvs_line_u32 = (uint32_t)fb_line - V_OFFSET;

    if (mvs_line_u32 >= MVS_HEIGHT) {
        video_pipeline_p720_fill_center(line, OVERSCAN_COLOR_RGB565);
        return true;
    }
    const uint16_t *src = NULL;
    if (line_ring_ready((uint16_t)mvs_line_u32)) {
        src = line_ring_read_ptr((uint16_t)mvs_line_u32);
    }
    if (!src) {
        video_pipeline_p720_fill_center(line, NO_SIGNAL_COLOR_RGB565);
        return false;
    }

#if NEOPICO_ENABLE_OSD
    const uint32_t osd_line_u32 = (uint32_t)fb_line - OSD_BOX_Y;
    if (osd_visible_latched && osd_line_u32 < OSD_BOX_H) {
        // Compose at NATIVE resolution, then triple: OSD cost does not
        // scale with the output mode.
        memcpy(p720_native_scratch, src, LINE_WIDTH * sizeof(uint16_t));
        memcpy(&p720_native_scratch[OSD_BOX_X], osd_framebuffer[osd_line_u32], OSD_BOX_W * sizeof(uint16_t));
        video_pipeline_triple_pixels_fast(&line[P720_PILLAR_WORDS], p720_native_scratch, LINE_WIDTH);
        return true;
    }
#endif
    video_pipeline_triple_pixels_fast(&line[P720_PILLAR_WORDS], src, LINE_WIDTH);
    return true;
}

// Ring prep work: keep the ring ahead of the beam, bounded per call.
static void video_pipeline_p720_prep_work(void)
{
    // Frame restart is handled HERE, not in the vsync ISR: the vsync handler
    // can preempt this IRQ mid-line, and a cursor reset racing the post-line
    // increment skipped line 0 (stale slots at the top of the image).
    static uint16_t last_seq;
    const uint16_t seq = p720_frame_seq;
    if (last_seq != seq) {
        last_seq = seq;
        p720_prep_line = 0;
        p720_retry_line = 0;
    }
    uint32_t budget = 6;
    while (budget--) {
        if (p720_prep_line >= FRAME_HEIGHT) {
            break; // frame fully prepped; retries below until vsync restarts
        }
        const uint16_t scan = p720_scan_source_line;
        const int16_t ahead = (int16_t)(p720_prep_line - scan);
        if (ahead >= (int16_t)(P720_RING_LINES - 4)) {
            break; // far enough ahead; don't lap the ring
        }
        const uint16_t l = p720_prep_line;
        if (video_pipeline_p720_prep_one(l)) {
            p720_gray_mask[l / 32] &= ~(1U << (l % 32));
        } else {
            p720_gray_mask[l / 32] |= 1U << (l % 32);
        }
        p720_prep_line++;
    }

    // Retry pass: capture commits lines slightly faster than the beam
    // consumes them, so a line that prepped gray usually becomes ready
    // before display. Bounded work; only lines the beam hasn't reached.
    uint32_t retries = 2;
    while (retries--) {
        const uint16_t scan = p720_scan_source_line;
        if (p720_retry_line <= scan + 1) {
            p720_retry_line = scan + 2;
        }
        while (p720_retry_line < p720_prep_line &&
               !(p720_gray_mask[p720_retry_line / 32] & (1U << (p720_retry_line % 32)))) {
            p720_retry_line++;
        }
        if (p720_retry_line >= p720_prep_line) {
            return;
        }
        if (video_pipeline_p720_prep_one(p720_retry_line)) {
            p720_gray_mask[p720_retry_line / 32] &= ~(1U << (p720_retry_line % 32));
        }
        p720_retry_line++;
    }
}

// The Core 1 background task legitimately stalls for 100s of ms (audio SRC
// bursts) — far longer than the ring's ~1 ms of beam time — so prep cannot
// be cooperatively scheduled there. A hardware alarm IRQ on Core 1 paces it
// preemptively: it interrupts audio bursts, and the scanline DMA ISR (higher
// priority) still preempts IT. ~10 us of work per 250 us tick.
#define P720_PREP_INTERVAL_US 250
static int8_t p720_prep_alarm = -1;

static void video_pipeline_p720_alarm_cb(uint alarm_num)
{
    hardware_alarm_set_target(alarm_num, from_us_since_boot(time_us_64() + P720_PREP_INTERVAL_US));
    video_pipeline_p720_prep_work();
}

// Called from the Core 1 background task: first call installs the alarm
// (must run on Core 1 so the IRQ binds to this core), then becomes a no-op
// (prep runs only in alarm context — single writer, no reentrancy).
void video_pipeline_precomp_background(void)
{
    if (p720_prep_alarm >= 0) {
        return;
    }
    int alarm = hardware_alarm_claim_unused(true);
    p720_prep_alarm = (int8_t)alarm;
    irq_set_priority(TIMER0_IRQ_0 + alarm, 0xC0); // below the scanout DMA ISR
    hardware_alarm_set_callback((uint)alarm, video_pipeline_p720_alarm_cb);
    hardware_alarm_set_target((uint)alarm, from_us_since_boot(time_us_64() + P720_PREP_INTERVAL_US));
}
#endif // NEOPICO_VIDEO_720P

#endif // NEOPICO_EXP_PRECOMPOSED_HDMI

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height)
{
#if NEOPICO_TRIPLE_ASM
    // Prove the M33 scale kernels match the C references before they prep any
    // line (verdict shown on the Genlock OSD as ASM:OK / ASM:BAD).
    video_pipeline_scale_selftest();
#endif
    video_output_init(frame_width, frame_height);
    video_output_set_vsync_callback(video_pipeline_vsync_callback);
#if NEOPICO_EXP_REBOOT_MODE_SWITCH
    if (video_output_active_mode->v_active_lines == 720U) {
        reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_720P;
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
        video_output_set_scanline_callback(video_pipeline_scanline_callback_reboot_modes);
#else
        video_output_set_scanline_callback(video_pipeline_scanline_callback_480p);
#endif
    } else if (video_output_active_mode->v_active_lines == 240U) {
        reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_240P;
        video_output_set_scanline_callback(video_pipeline_scanline_callback_480p);
    } else {
        reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
        video_output_set_scanline_callback(video_pipeline_scanline_callback_480p);
    }
#elif NEOPICO_EXP_PRECOMPOSED_HDMI
#if NEOPICO_VIDEO_720P
    // Pillarbox baked into every ring line once; prep only writes centers.
    for (uint32_t l = 0; l < P720_RING_LINES; l++) {
        for (uint32_t i = 0; i < P720_LINE_WORDS; i++) {
            p720_ring[l][i] = 0;
        }
    }
    video_output_set_scanline_pointer_callback(video_pipeline_p720_ptr_callback);
    video_output_set_native_pixel_mode(false); // 32-bit pre-expanded lines
#else
    for (uint32_t i = 0; i < LINE_WIDTH / 2; i++) {
        overscan_native_line[i] = OVERSCAN_COLOR_RGB565 | ((uint32_t)OVERSCAN_COLOR_RGB565 << 16);
        nosignal_native_line[i] = NO_SIGNAL_COLOR_RGB565 | ((uint32_t)NO_SIGNAL_COLOR_RGB565 << 16);
    }
    video_output_set_scanline_pointer_callback(video_pipeline_scanline_ptr_callback);
    video_output_set_native_pixel_mode(true);
#endif
    video_output_set_compose_ring(precomposed_ring, sizeof(precomposed_ring) / sizeof(precomposed_ring[0]));
#else
    video_output_set_scanline_callback(video_pipeline_scanline_callback);
#endif

#if NEOPICO_ENABLE_OSD
    osd_visible_latched = osd_visible;
#endif
}

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
#if NEOPICO_EXP_RAM_OSD_APPLY_PATH
#define VIDEO_PIPELINE_REBOOT_REQUEST_RAM(name) __no_inline_not_in_flash_func(name)
#else
#define VIDEO_PIPELINE_REBOOT_REQUEST_RAM(name) name
#endif

void VIDEO_PIPELINE_REBOOT_REQUEST_RAM(video_pipeline_request_reboot_mode)(video_pipeline_reboot_mode_t mode)
{
    if (mode > REBOOT_MODE_MAX) {
        mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
    reboot_requested_mode = mode;
#if NEOPICO_SETTINGS_FLASH
    // Persist the selected resolution so it survives power-off. Blocking write,
    // completes before the watchdog fires. We're about to reboot, so the brief
    // ISR stall during the flash erase is invisible (screen is going away).
    {
        neopico_settings_t persisted;
        settings_load(&persisted); // keep other fields intact
        persisted.resolution = (uint8_t)mode;
        settings_save(&persisted);
    }
#endif
    watchdog_hw->scratch[0] = REBOOT_MODE_BOOT_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)mode;
    watchdog_hw->scratch[2] = reboot_mode_boot_check((uint32_t)mode);
    __dmb();
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

video_pipeline_reboot_mode_t video_pipeline_reboot_requested_mode(void)
{
    return reboot_requested_mode;
}

bool video_pipeline_take_reboot_mode_boot_request(video_pipeline_reboot_mode_t *requested_mode)
{
    const uint32_t magic = watchdog_hw->scratch[0];
    const uint32_t mode = watchdog_hw->scratch[1];
    const uint32_t check = watchdog_hw->scratch[2];
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;

    if ((magic != REBOOT_MODE_BOOT_MAGIC) || (mode > (uint32_t)REBOOT_MODE_MAX) ||
        (check != reboot_mode_boot_check(mode))) {
        return false;
    }

    if (requested_mode) {
        *requested_mode = (video_pipeline_reboot_mode_t)mode;
    }
    return true;
}

void video_pipeline_request_reboot_240p(bool enabled)
{
    video_pipeline_request_reboot_mode(enabled ? VIDEO_PIPELINE_REBOOT_MODE_240P : VIDEO_PIPELINE_REBOOT_MODE_480P);
}

bool video_pipeline_reboot_requested_240p(void)
{
    return reboot_requested_mode == VIDEO_PIPELINE_REBOOT_MODE_240P;
}

bool video_pipeline_take_reboot_240p_boot_request(bool *enabled)
{
    video_pipeline_reboot_mode_t mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    if (!video_pipeline_take_reboot_mode_boot_request(&mode) || mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        return false;
    }

    if (enabled) {
        *enabled = (mode == VIDEO_PIPELINE_REBOOT_MODE_240P);
    }
    return true;
}
#endif

/**
 * Fast 2x pixel doubling: reads 2 pixels, writes 2 doubled words.
 * Processes 32-bits at a time for efficiency.
 */
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *restrict dst, const uint16_t *restrict src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    uint32_t *d = dst;
    int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        uint32_t pair = src32[i];
        uint32_t p0 = pair & 0xFFFF;
        uint32_t p1 = pair >> 16;
        d[0] = p0 | (p0 << 16);
        d[1] = p1 | (p1 << 16);
        d += 2;
    }
}

/**
 * Fast 3x pixel scaling: reads 2 pixels, writes 3 doubled words (6 output pixels).
 * For 720p 4:3 mode (960 output pixels from 320 source pixels, centered).
 */
// When NEOPICO_TRIPLE_ASM=1 these two symbols come from scale_pixels.S (M33
// kernels); the C bodies below compile only as the default path. The asm path
// keeps C reference copies (static, below) and a boot self-test that proves
// them equivalent before any frame is prepped.
#if !NEOPICO_TRIPLE_ASM
void __scratch_y("") video_pipeline_triple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count >> 1;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        dst[(i * 3) + 0] = p0 | (p0 << 16);
        dst[(i * 3) + 1] = p0 | (p1 << 16);
        dst[(i * 3) + 2] = p1 | (p1 << 16);
    }
}

/**
 * Fast 4x pixel quadrupling: reads 2 pixels, writes 4 doubled words (8 output pixels).
 * For 240p direct mode (1280 output pixels from 320 source pixels).
 */
void __scratch_y("") video_pipeline_quadruple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        uint32_t d0 = p0 | (p0 << 16);
        uint32_t d1 = p1 | (p1 << 16);
        dst[i * 4] = d0;
        dst[(i * 4) + 1] = d0;
        dst[(i * 4) + 2] = d1;
        dst[(i * 4) + 3] = d1;
    }
}
#endif // !NEOPICO_TRIPLE_ASM

#if NEOPICO_TRIPLE_ASM
// Boot equivalence gate for the scale_pixels.S kernels. References mirror the
// C bodies above; the self-test runs both over several pixel counts (incl. odd
// and zero) and publishes the verdict. Never trust the asm — verify it.
volatile bool g_scale_asm_selftest_ok = true;

static void scale_triple_ref(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i], p0 = two & 0xFFFF, p1 = two >> 16;
        dst[(i * 3) + 0] = p0 | (p0 << 16);
        dst[(i * 3) + 1] = p0 | (p1 << 16);
        dst[(i * 3) + 2] = p1 | (p1 << 16);
    }
}

static void scale_quad_ref(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;
    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i], p0 = two & 0xFFFF, p1 = two >> 16;
        uint32_t d0 = p0 | (p0 << 16), d1 = p1 | (p1 << 16);
        dst[(i * 4) + 0] = d0;
        dst[(i * 4) + 1] = d0;
        dst[(i * 4) + 2] = d1;
        dst[(i * 4) + 3] = d1;
    }
}

void video_pipeline_scale_selftest(void)
{
    static uint16_t in[324];
    static uint32_t ref[1400];
    static uint32_t got[1400];
    static const int counts[] = {320, 318, 319, 64, 10, 2, 1, 0};

    uint32_t s = 0x12345678u;
    for (int i = 0; i < 324; i++) {
        s = s * 1664525u + 1013904223u;
        in[i] = (uint16_t)(s >> 16);
    }

    bool ok = true;
    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        int n = counts[c];

        for (int i = 0; i < 1400; i++) {
            ref[i] = 0xAAAAAAAAu;
            got[i] = 0x55555555u;
        }
        scale_triple_ref(ref, in, n);
        video_pipeline_triple_pixels_fast(got, in, n);
        for (int i = 0; i < (n >> 1) * 3; i++) {
            if (ref[i] != got[i]) {
                ok = false;
            }
        }

        for (int i = 0; i < 1400; i++) {
            ref[i] = 0xAAAAAAAAu;
            got[i] = 0x55555555u;
        }
        scale_quad_ref(ref, in, n);
        video_pipeline_quadruple_pixels_fast(got, in, n);
        for (int i = 0; i < (n >> 1) * 4; i++) {
            if (ref[i] != got[i]) {
                ok = false;
            }
        }
    }
    g_scale_asm_selftest_ok = ok;
}
#endif // NEOPICO_TRIPLE_ASM

#if NEOPICO_EXP_GENLOCK_DYNAMIC && !NEOPICO_USE_NONRT_HDMI
// Nominals that approximate MVS ~59.18 Hz at 25.2 MHz pixel clock:
//   480p: 25.2M / (800 * 532) = 59.21 Hz   (±1 → 59.10–59.32 Hz)
//   240p: 25.2M / (1600 * 266) = 59.21 Hz  (±1 → 58.99–59.43 Hz)
#define GENLOCK_NOMINAL_VTOTAL_480 532
#define GENLOCK_NOMINAL_VTOTAL_240 266
//   720p: pixel clock is sys/5 = 372/5 = 74.4 MHz (NOT the CEA 74.25!):
//   74.4M / (1650 * 762) = 59.187 Hz (+2 us/frame vs MVS 59.1856) —
//   nominal creeps the phase UP very slowly; 761 (-20 us/frame) pulls back.
#define GENLOCK_NOMINAL_VTOTAL_720 762
#define GENLOCK_PHASE_THRESHOLD_US 200
#define GENLOCK_PHASE_MAX_US 5000
// Output vsyncs landing shortly after the MVS vsync sample a frame base no
// capture line has been committed to yet (lines prep gray): phase must stay
// inside [~3ms, ~15ms]. A tight setpoint needs a fractional line rate and
// dithers vtotal every few frames, which makes sinks' vertical lock hunt
// (top-of-image wobble). Instead: WIDE HYSTERESIS — let the phase ramp the
// healthy zone on a constant vtotal, pull it back on a constant vtotal-1.
// Two one-line timing changes per ~42 s instead of ~18 per second.
#define GENLOCK_PHASE_PULLBACK_AT_US 14000
#define GENLOCK_PHASE_RESUME_AT_US 4000
#define GENLOCK_PHASE_SETPOINT_US 11000

// Once per frame from the vsync callback; does not need scratch residency
// (and scratch_x is at its hard boundary).
volatile uint32_t g_genlock_phase_us;      // published for the genlock OSD
volatile uint32_t g_genlock_outzone_count; // raw out-of-zone frames (incl. measurement spikes)

static void genlock_dynamic_update(void)
{
    uint32_t hdmi_ts = timer_hw->timerawl;
    uint32_t mvs_ts = g_mvs_vsync_timestamp;
    uint32_t phase = hdmi_ts - mvs_ts; // us since last MVS vsync, [0, ~16.9ms)
    g_genlock_phase_us = phase;

    const uint16_t mode_total = video_output_active_mode->v_total_lines;
    uint16_t nominal = (mode_total <= 266)   ? GENLOCK_NOMINAL_VTOTAL_240
                       : (mode_total >= 700) ? GENLOCK_NOMINAL_VTOTAL_720
                                             : GENLOCK_NOMINAL_VTOTAL_480;

    // Steady state: vtotal stays at nominal FOREVER and a proportional servo
    // on the blanking h-trim nulls the residual drift (sub-line steps are
    // invisible; vtotal steps of a whole line visibly disturb some sinks).
    // vtotal only steps during acquire (boot / signal reappearing), when the
    // phase is outside the content-safe zone and speed matters more than
    // cosmetics.
    static int applied_trim;
    static uint16_t step_cooldown;
    static uint8_t out_zone_streak;

    // A single late mvs-timestamp IRQ (Core 0 shares with USB/capture) makes
    // the phase READ as a huge excursion for one frame; acting on it puts a
    // one-frame vtotal step on the wire -- the exact whole-frame jolt this
    // sink shows. Count raw excursions (OSD "A") but only ACT on a streak a
    // measurement spike cannot produce.
    const bool out_low = phase < GENLOCK_PHASE_RESUME_AT_US;
    const bool out_high = phase > GENLOCK_PHASE_PULLBACK_AT_US;
    if (out_low || out_high) {
        g_genlock_outzone_count++;
        if (out_zone_streak < 255) {
            out_zone_streak++;
        }
    } else {
        out_zone_streak = 0;
    }

    if (out_zone_streak >= 8 && out_low) {
        rt_v_total_lines = (uint16_t)(nominal + 1); // fast acquire upward
    } else if (out_zone_streak >= 8 && out_high) {
        rt_v_total_lines = (uint16_t)(nominal - 1); // fast pull back down
    } else {
        rt_v_total_lines = nominal;
        // SLOW INTEGRATOR. Bench finding: this sink tolerates any CONSTANT
        // trim (even the clamp) but visibly glitches on trim ACTIVITY --
        // the proportional servo hunting +-1 px at frame rate was itself
        // the artifact. So: hold trim absolutely constant; only when the
        // phase wanders past the deadband, make a single 1-px adjustment,
        // then hold again (rate-limited: 10 frames while far out for
        // convergence, 60 frames near lock). At equilibrium the +-0.5 px
        // quantization means one lone pixel-step every few MINUTES.
        // Derivative gating: trim->drift->phase is a double integration, so
        // stepping on position error alone limit-cycles (observed: trim
        // sweeping -6..-30 forever, glitching at every step). Only step
        // while the phase is NOT already heading back toward the setpoint;
        // this both damps the cycle and acts as anti-windup. Settles on the
        // quantization-optimal constant trim with a lone +-1 px touch every
        // ~30-60 s.
        static uint32_t drift_prev_phase;
        static int32_t drift_per64; // us per 64 frames; 1 px ~= 12
        static uint8_t drift_ctr;
        static bool drift_valid;
        if (!drift_valid) {
            drift_prev_phase = phase;
            drift_valid = true;
        }
        if (++drift_ctr >= 64) {
            drift_per64 = (int32_t)(phase - drift_prev_phase);
            drift_prev_phase = phase;
            drift_ctr = 0;
        }

        int32_t e_us = (int32_t)phase - (int32_t)GENLOCK_PHASE_SETPOINT_US;
        if (step_cooldown) {
            step_cooldown--;
        } else if (e_us > 400 && drift_per64 >= -6) {
            if (applied_trim > -30) {
                applied_trim--;
                video_output_set_vblank_htrim_px(applied_trim);
            }
            step_cooldown = 30;
        } else if (e_us < -400 && drift_per64 <= 6) {
            if (applied_trim < 30) {
                applied_trim++;
                video_output_set_vblank_htrim_px(applied_trim);
            }
            step_cooldown = 30;
        }
    }
}
#endif

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 */
void __scratch_x("") video_pipeline_vsync_callback(void)
{
    line_ring_output_vsync();
#if NEOPICO_EXP_PRECOMPOSED_HDMI && NEOPICO_ENABLE_OSD && !NEOPICO_VIDEO_720P
    osd_native_scratch_line = 0xFFFF;
#endif
#if NEOPICO_EXP_PRECOMPOSED_HDMI && NEOPICO_VIDEO_720P
    p720_scan_source_line = 0;
    p720_frame_seq++;
#endif
#if NEOPICO_EXP_GENLOCK_DYNAMIC && !NEOPICO_USE_NONRT_HDMI
    genlock_dynamic_update();
#endif
#if NEOPICO_ENABLE_OSD
    osd_visible_latched = osd_visible;
#endif
}

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
static void __scratch_x("000_video_pipeline_480p")
    video_pipeline_scanline_callback_480p(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    const bool mode_is_240p = (video_output_active_mode->v_active_lines == 240U);
    const uint32_t h_words = mode_is_240p ? (1280U / 2U) : (640U / 2U);
    const uint32_t h_scale = mode_is_240p ? 4U : 2U;
    const uint32_t image_words = (LINE_WIDTH * h_scale) / 2U;
    const uint32_t x_margin_words = 0U;
#if NEOPICO_ENABLE_OSD
    const uint32_t osd_x_words = (((uint32_t)OSD_BOX_X * h_scale) / 2U);
    const uint32_t osd_w_words = (((uint32_t)OSD_BOX_W * h_scale) / 2U);
#endif
    const pixel_scale_fn_t scale_pixels =
        mode_is_240p ? video_pipeline_quadruple_pixels_fast : video_pipeline_double_pixels_fast;
    const uint32_t fb_line = mode_is_240p ? active_line : (active_line >> 1);
#define VIDEO_PIPELINE_SCALE_SELECTED(dst_arg, src_arg, count_arg) scale_pixels((dst_arg), (src_arg), (count_arg))

#if NEOPICO_ENABLE_OSD
    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    const bool osd_line_active = osd_visible_latched && (osd_line_u32 < OSD_BOX_H);
#else
    const bool osd_line_active = false;
#endif

    if (!osd_line_active) {
        const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
        // Single unsigned range check for active 224-line window.
        if (mvs_line_u32 >= MVS_HEIGHT) {
            video_pipeline_fill_rgb565(dst, h_words, OVERSCAN_COLOR_RGB565);
            return;
        }

        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        const uint16_t *src = NULL;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
        }
        if (!src) {
            video_pipeline_fill_rgb565(dst, h_words, NO_SIGNAL_COLOR_RGB565);
            return;
        }
        video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, LINE_WIDTH);
        video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                                   OVERSCAN_COLOR_RGB565);
        return;
    }

#if NEOPICO_ENABLE_OSD
    // OSD-active path: draw OSD even if capture source is unavailable.
    const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
    const uint16_t *src = NULL;
    if (mvs_line_u32 < MVS_HEIGHT) {
        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
        }
    }

    const uint16_t *osd_src = osd_framebuffer[osd_line_u32];
    if (!src) {
        video_pipeline_fill_rgb565(dst, osd_x_words, NO_SIGNAL_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
        video_pipeline_fill_rgb565(dst + osd_x_words + osd_w_words, h_words - osd_x_words - osd_w_words,
                                   NO_SIGNAL_COLOR_RGB565);
        return;
    }

    // Before OSD
    video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
    VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, OSD_BOX_X);
    // OSD region (blit from OSD framebuffer)
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
    // After OSD
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words + osd_w_words, src + OSD_BOX_X + OSD_BOX_W,
                                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                               OVERSCAN_COLOR_RGB565);
#endif
#undef VIDEO_PIPELINE_SCALE_SELECTED
}
#endif

#if (!NEOPICO_EXP_REBOOT_MODE_SWITCH || NEOPICO_EXP_REBOOT_MODE_SWITCH_720P) && !NEOPICO_EXP_PRECOMPOSED_HDMI
#if NEOPICO_EXP_REBOOT_MODE_SWITCH && NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
static void video_pipeline_scanline_callback_reboot_modes(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
#else
void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
#endif
{
    (void)v_scanline;

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
    const bool mode_is_240p = (video_output_active_mode->v_active_lines == 240U);
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
    const bool mode_is_720p = (video_output_active_mode->v_active_lines == 720U);
    const uint32_t h_words = (mode_is_240p || mode_is_720p) ? (1280U / 2U) : (640U / 2U);
    const uint32_t h_scale = mode_is_720p ? 3U : mode_is_240p ? 4U : 2U;
    const uint32_t image_words = (LINE_WIDTH * h_scale) / 2U;
    const uint32_t x_margin_words = (h_words > image_words) ? ((h_words - image_words) / 2U) : 0U;
    const pixel_scale_fn_t scale_pixels = mode_is_720p   ? video_pipeline_triple_pixels_fast
                                          : mode_is_240p ? video_pipeline_quadruple_pixels_fast
                                                         : video_pipeline_double_pixels_fast;
    if (mode_is_720p && ((active_line % 3U) != 0U)) {
        return;
    }
    const uint32_t fb_line = mode_is_720p ? (active_line / 3U) : mode_is_240p ? active_line : (active_line >> 1);
#else
    const uint32_t h_words = mode_is_240p ? (1280U / 2U) : (640U / 2U);
    const uint32_t h_scale = mode_is_240p ? 4U : 2U;
    const uint32_t image_words = (LINE_WIDTH * h_scale) / 2U;
    const uint32_t x_margin_words = 0U;
    const pixel_scale_fn_t scale_pixels =
        mode_is_240p ? video_pipeline_quadruple_pixels_fast : video_pipeline_double_pixels_fast;
    const uint32_t fb_line = mode_is_240p ? active_line : (active_line >> 1);
#endif
#if NEOPICO_ENABLE_OSD
    const uint32_t osd_x_words = x_margin_words + (((uint32_t)OSD_BOX_X * h_scale) / 2U);
    const uint32_t osd_w_words = (((uint32_t)OSD_BOX_W * h_scale) / 2U);
#endif
#define VIDEO_PIPELINE_SCALE_SELECTED(dst_arg, src_arg, count_arg) scale_pixels((dst_arg), (src_arg), (count_arg))
#else
    const uint32_t h_words = VIDEO_PIPELINE_H_WORDS;
    const uint32_t image_words = VIDEO_PIPELINE_IMAGE_WORDS;
    const uint32_t x_margin_words = VIDEO_PIPELINE_X_MARGIN_WORDS;
#if NEOPICO_ENABLE_OSD
    const uint32_t osd_x_words = VIDEO_PIPELINE_OSD_X_WORDS;
    const uint32_t osd_w_words = VIDEO_PIPELINE_OSD_W_WORDS;
#endif
#define VIDEO_PIPELINE_SCALE_SELECTED(dst_arg, src_arg, count_arg)                                                     \
    VIDEO_PIPELINE_SCALE_PIXELS((dst_arg), (src_arg), (count_arg))

#if NEOPICO_VIDEO_TEST_PATTERN && NEOPICO_VIDEO_720P
    if (!test_pattern_line_ready) {
        video_pipeline_init_test_pattern_line();
    }
    if ((active_line % 3U) != 0U) {
        return;
    }
    video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
    VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, test_pattern_line, LINE_WIDTH);
    video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                               OVERSCAN_COLOR_RGB565);
    return;
#endif

    uint32_t fb_line;
    if (!video_pipeline_map_active_line(active_line, &fb_line)) {
        return;
    }
#endif

#if NEOPICO_ENABLE_OSD
    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    const bool osd_line_active = osd_visible_latched && (osd_line_u32 < OSD_BOX_H);
#else
    const bool osd_line_active = false;
#endif

    if (!osd_line_active) {
        const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
        // Single unsigned range check for active 224-line window.
        if (mvs_line_u32 >= MVS_HEIGHT) {
            video_pipeline_fill_rgb565(dst, h_words, OVERSCAN_COLOR_RGB565);
            return;
        }

        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        const uint16_t *src = NULL;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
        }
        if (!src) {
            video_pipeline_fill_rgb565(dst, h_words, NO_SIGNAL_COLOR_RGB565);
            return;
        }
        video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, LINE_WIDTH);
        video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                                   OVERSCAN_COLOR_RGB565);
        return;
    }

#if NEOPICO_ENABLE_OSD
    // OSD-active path: draw OSD even if capture source is unavailable.
    const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
    const uint16_t *src = NULL;
    if (mvs_line_u32 < MVS_HEIGHT) {
        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
        }
    }

    const uint16_t *osd_src = osd_framebuffer[osd_line_u32];
    if (!src) {
        // No capture source: render OSD over fallback color without double-writing the OSD span.
        video_pipeline_fill_rgb565(dst, osd_x_words, NO_SIGNAL_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
        video_pipeline_fill_rgb565(dst + osd_x_words + osd_w_words, h_words - osd_x_words - osd_w_words,
                                   NO_SIGNAL_COLOR_RGB565);
        return;
    }

    // Before OSD
    video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
    VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, OSD_BOX_X);
    // OSD region (blit from OSD framebuffer)
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
    // After OSD
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words + osd_w_words, src + OSD_BOX_X + OSD_BOX_W,
                                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                               OVERSCAN_COLOR_RGB565);
#endif
#undef VIDEO_PIPELINE_SCALE_SELECTED
}
#endif

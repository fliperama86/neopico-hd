/**
 * DVI Audio Test - Simple Tone
 *
 * Tests HDMI audio output with a 440 Hz sine tone.
 * Based on PicoDVI sprite_bounce_audio example pattern.
 *
 * Use MODE_240P or MODE_480P to switch timing modes.
 */

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"

#include "dvi.h"
#include "audio_ring.h"
#include "neopico_config.h"

// =============================================================================
// Mode Selection
// =============================================================================

// #define MODE_240P  // Doesn't work - flickering video
#define MODE_480P     // Working configuration

// =============================================================================
// Timing Configuration
// =============================================================================

#ifdef MODE_240P
    // 240p timing with extra horizontal blanking for audio data islands
    static const struct dvi_timing dvi_timing_640x240p_60hz = {
        .h_sync_polarity   = false,
        .h_front_porch     = 16,
        .h_sync_width      = 96,
        .h_back_porch      = 88,   // Increased from 48 to give more room for data islands
        .h_active_pixels   = 640,
        .v_sync_polarity   = false,
        .v_front_porch     = 3,
        .v_sync_width      = 3,
        .v_back_porch      = 16,
        .v_active_lines    = 240,
        .bit_clk_khz       = 126000  // Keep same clock - will be ~55Hz instead of 60Hz
    };
    #define DVI_TIMING dvi_timing_640x240p_60hz
    #define FRAME_HEIGHT 240  // Full 240 lines (no blank_settings)
    #define AUDIO_CTS 12600   // CTS for 48kHz @ 126MHz bit clock
    #define MODE_NAME "240p"
#else  // MODE_480P
    #define DVI_TIMING dvi_timing_640x480p_60hz
    #define FRAME_HEIGHT 232  // 480p with blank_settings 8+8 = 464 DVI lines / 2 = 232
    #define AUDIO_CTS 25200
    #define MODE_NAME "480p"
#endif

#define FRAME_WIDTH 320
#define VREG_VSEL VREG_VOLTAGE_1_20
#define AUDIO_BUFFER_SIZE 256
#define N_SCANLINE_BUFFERS 4

// =============================================================================
// Global State
// =============================================================================

struct dvi_inst dvi0;
static uint16_t static_scanbuf[N_SCANLINE_BUFFERS][FRAME_WIDTH];
static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static struct repeating_timer audio_timer;

// =============================================================================
// Sine Wave Table (from PicoDVI example)
// =============================================================================

static const int16_t sine[128] = {
    0x3fff, 0x4322, 0x4644, 0x4962, 0x4c7b, 0x4f8b, 0x5292, 0x558e,
    0x587c, 0x5b5b, 0x5e29, 0x60e5, 0x638c, 0x661e, 0x6898, 0x6af9,
    0x6d3f, 0x6f6a, 0x7177, 0x7365, 0x7534, 0x76e3, 0x786f, 0x79d9,
    0x7b1e, 0x7c40, 0x7d3c, 0x7e13, 0x7ec3, 0x7f4c, 0x7faf, 0x7fea,
    0x7ffe, 0x7fea, 0x7faf, 0x7f4c, 0x7ec3, 0x7e13, 0x7d3c, 0x7c40,
    0x7b1e, 0x79d9, 0x786f, 0x76e3, 0x7534, 0x7365, 0x7177, 0x6f6a,
    0x6d3f, 0x6af9, 0x6898, 0x661e, 0x638c, 0x60e5, 0x5e29, 0x5b5b,
    0x587c, 0x558e, 0x5292, 0x4f8b, 0x4c7b, 0x4962, 0x4644, 0x4322,
    0x3fff, 0x3cdb, 0x39b9, 0x369b, 0x3382, 0x3072, 0x2d6b, 0x2a6f,
    0x2781, 0x24a2, 0x21d4, 0x1f18, 0x1c71, 0x19df, 0x1765, 0x1504,
    0x12be, 0x1093, 0x0e86, 0x0c98, 0x0ac9, 0x091a, 0x078e, 0x0624,
    0x04df, 0x03bd, 0x02c1, 0x01ea, 0x013a, 0x00b1, 0x004e, 0x0013,
    0x0000, 0x0013, 0x004e, 0x00b1, 0x013a, 0x01ea, 0x02c1, 0x03bd,
    0x04df, 0x0624, 0x078e, 0x091a, 0x0ac9, 0x0c98, 0x0e86, 0x1093,
    0x12be, 0x1504, 0x1765, 0x19df, 0x1c71, 0x1f18, 0x21d4, 0x24a2,
    0x2781, 0x2a6f, 0x2d6b, 0x3072, 0x3382, 0x369b, 0x39b9, 0x3cdb,
};

#define SINE_SIZE (sizeof(sine) / sizeof(sine[0]))

// =============================================================================
// Audio Timer Callback (exactly like PicoDVI example)
// =============================================================================

static bool audio_timer_callback(struct repeating_timer *t) {
    (void)t;
    int size = get_write_size(&dvi0.audio_ring, false);
    audio_sample_t *audio_ptr = get_write_pointer(&dvi0.audio_ring);
    audio_sample_t sample;
    static uint sample_count = 0;

    for (int cnt = 0; cnt < size; cnt++) {
        sample.channels[0] = sine[sample_count % SINE_SIZE];
        sample.channels[1] = sine[sample_count % SINE_SIZE];
        *audio_ptr++ = sample;
        sample_count++;
    }
    increase_write_pointer(&dvi0.audio_ring, size);
    return true;
}

// =============================================================================
// Core 1: DVI Output
// =============================================================================

static void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
    __builtin_unreachable();
}

// =============================================================================
// Render Loop (exactly like PicoDVI example)
// =============================================================================

static void __not_in_flash("render") render_loop() {
    uint heartbeat = 0;

    while (1) {
        if (++heartbeat >= 30) {
            heartbeat = 0;
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }

        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            uint16_t *pixbuf;
            queue_remove_blocking(&dvi0.q_colour_free, &pixbuf);

            // Simple gradient pattern
            for (uint x = 0; x < FRAME_WIDTH; x++) {
                uint16_t blue = (x * 31) / FRAME_WIDTH;
                uint16_t green = (y * 63) / FRAME_HEIGHT;
                pixbuf[x] = (green << 5) | blue;
            }

            queue_add_blocking(&dvi0.q_colour_valid, &pixbuf);
        }
    }
}

// =============================================================================
// Main
// =============================================================================

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    sleep_ms(1000);

    printf("\n===========================================\n");
    printf("  DVI Audio Test - %s\n", MODE_NAME);
    printf("===========================================\n");
    printf("Bit clock: %d kHz\n", DVI_TIMING.bit_clk_khz);
    printf("Audio: 48000 Hz, CTS=25200, N=6144\n");
    printf("Tone: ~375 Hz (128 samples at 48kHz)\n\n");

    // Initialize DVI
    printf("Configuring DVI\n");
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // HDMI Audio setup
#ifdef MODE_240P
    // 240p - doesn't work reliably (flickering)
    dvi_get_blank_settings(&dvi0)->top    = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, 48000, 12600, 6144);
#else
    // 480p - working configuration
    dvi_get_blank_settings(&dvi0)->top    = 4 * 2;
    dvi_get_blank_settings(&dvi0)->bottom = 4 * 2;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, 48000, 25200, 6144);
#endif
    add_repeating_timer_ms(2, audio_timer_callback, NULL, &audio_timer);

    printf("Core 1 start\n");
    multicore_launch_core1(core1_main);

    // Pre-fill scanline buffer queue (critical!)
    printf("Allocating scanline buffers\n");
    for (int i = 0; i < N_SCANLINE_BUFFERS; ++i) {
        void *bufptr = &static_scanbuf[i];
        queue_add_blocking(&dvi0.q_colour_free, &bufptr);
    }

    printf("Start rendering\n");
    render_loop();
    __builtin_unreachable();
}

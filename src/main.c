/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Main coordinator - orchestrates video and audio subsystems
 *
 * Pin Configuration:
 *   MVS PCLK:      GPIO 0 (C2)
 *   MVS Green:     GPIO 1-5 (G4-G0, reversed bit order)
 *   MVS Blue:      GPIO 6-10 (B0-B4)
 *   MVS Red:       GPIO 11-15 (R0-R4)
 *   MVS CSYNC:     GPIO 22
 *   DVI Clock:     GPIO 25-26
 *   DVI Data:      GPIO 27-32
 *   I2S DAT:       GPIO 36
 *   I2S WS:        GPIO 37
 *   I2S BCK:       GPIO 38
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "dvi.h"
#include "pins.h"

// Video modules
#include "video/video_capture.h"
#include "video/video_output.h"

// Audio modules
#include "audio/audio_pipeline.h"
#include "audio/audio_output.h"

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240  // 480p: 240 * 2 = 480 DVI lines
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20

// Use 480p timing (252 MHz) - required for HDMI audio
#define DVI_TIMING dvi_timing_640x480p_60hz

// =============================================================================
// Framebuffer
// =============================================================================

static uint16_t g_framebuf[FRAME_WIDTH * FRAME_HEIGHT];

// =============================================================================
// Audio
// =============================================================================

#define AUDIO_BUFFER_SIZE 1024  // HDMI audio buffer

static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static audio_pipeline_t audio_pipeline;

// Audio output callback - writes processed samples to HDMI
static void audio_output_callback(const ap_sample_t *samples, uint32_t count, void *ctx) {
    (void)ctx;  // Unused
    audio_output_write(samples, count);
}

// =============================================================================
// Main - Coordinator
// =============================================================================

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("NeoPico-HD: MVS Capture + DVI Output (Line-by-line 60fps)\n");

    // Initialize framebuffer to black
    memset(g_framebuf, 0, sizeof(g_framebuf));

    // Initialize video output (DVI)
    video_output_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT);

    // Initialize video capture (MVS)
    video_capture_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, MVS_HEIGHT);

    // Initialize HDMI audio output
    struct dvi_inst *dvi = video_output_get_dvi();
    dvi_audio_sample_buffer_set(dvi, audio_buffer, AUDIO_BUFFER_SIZE);
    audio_output_init(dvi, 48000);
    printf("HDMI audio initialized\n");

    // Initialize audio pipeline (I2S capture → filters → SRC)
    audio_pipeline_config_t audio_cfg = {
        .pin_dat = PIN_I2S_DAT,
        .pin_ws = PIN_I2S_WS,
        .pin_bck = PIN_I2S_BCK,
        .pio = pio2,  // Use PIO2 (RP2350 has 3 PIOs!)
        .sm = 0,
        .pin_btn1 = 0,  // No buttons for now
        .pin_btn2 = 0,
    };
    if (!audio_pipeline_init(&audio_pipeline, &audio_cfg)) {
        printf("ERROR: Failed to initialize audio pipeline\n");
        return 1;
    }
    audio_pipeline_start(&audio_pipeline);
    printf("Audio pipeline started\n");

    // Start DVI output on Core 1
    printf("Starting Core 1 (DVI)\n");
    video_output_start();

    printf("Starting line-by-line capture loop\n");

    // Main capture loop
    while (true) {
        uint32_t frame_count = video_capture_get_frame_count();
        gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 30) & 1);

        // Capture one frame (blocks until complete)
        if (!video_capture_frame()) {
            continue;  // Timeout, try again
        }

        // Process audio: I2S → filters → SRC → HDMI
        audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
    }

    return 0;
}

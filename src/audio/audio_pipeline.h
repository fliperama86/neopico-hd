/**
 * Audio Pipeline - Main Orchestrator
 *
 * Connects all audio modules together and handles:
 * - Button input for toggling stages
 * - Status reporting for display
 * - Data flow between stages
 */

#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include "audio_common.h"
#include "audio_buffer.h"
#include "i2s_capture.h"
#include "dc_filter.h"
#include "lowpass.h"
#include "src.h"

// Button pins (using freed Bank 0 pins - old audio pins)
#define AUDIO_BTN1_PIN 21  // DC filter toggle (was BCK)
#define AUDIO_BTN2_PIN 23  // SRC mode cycle (was DAT)

// Debounce time in milliseconds
#define BUTTON_DEBOUNCE_MS 50

// Pipeline status (for display)
typedef struct {
    // Capture stats
    uint32_t capture_sample_rate;   // Measured input rate
    uint32_t samples_captured;
    uint32_t capture_overflows;

    // Processing state
    bool dc_filter_enabled;
    bool lowpass_enabled;
    src_mode_t src_mode;

    // Output stats
    uint32_t output_sample_rate;
    uint32_t samples_output;
    uint32_t output_underruns;
} audio_pipeline_status_t;

// Pipeline configuration
typedef struct {
    // I2S capture pins
    uint pin_bck;
    uint pin_dat;
    uint pin_ws;

    // Button pins
    uint pin_btn1;
    uint pin_btn2;

    // PIO resources
    PIO pio;
    uint sm;
} audio_pipeline_config_t;

// Pipeline instance
typedef struct {
    audio_pipeline_config_t config;

    // Modules
    ap_ring_t capture_ring;
    i2s_capture_t capture;
    dc_filter_t dc_filter;
    lowpass_t lowpass;
    src_t src;

    // Button state (for debouncing)
    uint32_t btn1_last_press;
    uint32_t btn2_last_press;
    bool btn1_last_state;
    bool btn2_last_state;

    // Output stats
    uint32_t samples_output;
    uint32_t output_underruns;

    bool initialized;
} audio_pipeline_t;

// Initialize pipeline with config
bool audio_pipeline_init(audio_pipeline_t *p, const audio_pipeline_config_t *config);

// Start pipeline (begins capture)
void audio_pipeline_start(audio_pipeline_t *p);

// Stop pipeline
void audio_pipeline_stop(audio_pipeline_t *p);

// Process audio: call this regularly from main loop
// Reads from capture buffer, processes through stages, writes to output
// output_fn: callback to write samples to HDMI audio ring
typedef void (*audio_output_fn)(const ap_sample_t *samples, uint32_t count, void *ctx);
void audio_pipeline_process(audio_pipeline_t *p, audio_output_fn output_fn, void *ctx);

// Poll buttons and handle toggles (call from main loop)
void audio_pipeline_poll_buttons(audio_pipeline_t *p);

// Get current status for display
void audio_pipeline_get_status(audio_pipeline_t *p, audio_pipeline_status_t *status);

// Direct control (alternative to buttons)
void audio_pipeline_set_dc_filter(audio_pipeline_t *p, bool enabled);
void audio_pipeline_set_src_mode(audio_pipeline_t *p, src_mode_t mode);

#endif // AUDIO_PIPELINE_H

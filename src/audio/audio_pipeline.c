/**
 * Audio Pipeline Orchestrator
 *
 * Connects all audio modules and handles:
 * - Button input with debouncing
 * - Data flow between stages
 * - Status reporting
 */

#include "audio_pipeline.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <string.h>

// Processing buffer size (intermediate between stages)
#define PROCESS_BUFFER_SIZE 64

// Static buffers for processing
static ap_sample_t process_in[PROCESS_BUFFER_SIZE];
static ap_sample_t process_out[PROCESS_BUFFER_SIZE];

bool audio_pipeline_init(audio_pipeline_t *p, const audio_pipeline_config_t *config)
{
    memset(p, 0, sizeof(*p));
    p->config = *config;

    // Initialize ring buffer
    ap_ring_init(&p->capture_ring);

    // Initialize capture
    i2s_capture_config_t cap_config = {
        .pin_bck = config->pin_bck,
        .pin_dat = config->pin_dat,
        .pin_ws = config->pin_ws,
        .pio = config->pio,
        .sm = config->sm};

    if (!i2s_capture_init(&p->capture, &cap_config, &p->capture_ring))
    {
        return false;
    }

    // Initialize DC filter (disabled by default for troubleshooting)
    dc_filter_init(&p->dc_filter);
    p->dc_filter.enabled = false;

    // Initialize lowpass filter (disabled by default for troubleshooting)
    lowpass_init(&p->lowpass);
    p->lowpass.enabled = false;

    // Initialize SRC (DROP mode by default - proper decimation)
    src_init(&p->src, SRC_INPUT_RATE_DEFAULT, SRC_OUTPUT_RATE_DEFAULT);
    p->src.mode = SRC_MODE_DROP;

    // Configure button pins as inputs with pull-ups
    // Note: Requires correct RP2350B board definition for GPIO 30+
    gpio_init(config->pin_btn1);
    gpio_set_dir(config->pin_btn1, GPIO_IN);
    gpio_pull_up(config->pin_btn1);

    gpio_init(config->pin_btn2);
    gpio_set_dir(config->pin_btn2, GPIO_IN);
    gpio_pull_up(config->pin_btn2);

    // Initialize button state
    p->btn1_last_state = true; // Pull-up means idle high
    p->btn2_last_state = true;
    p->btn1_last_press = 0;
    p->btn2_last_press = 0;

    p->initialized = true;
    return true;
}

void audio_pipeline_start(audio_pipeline_t *p)
{
    if (!p->initialized)
        return;

    // Start capture (polling mode - no DMA/IRQ)
    i2s_capture_start(&p->capture);
}

void audio_pipeline_stop(audio_pipeline_t *p)
{
    if (!p->initialized)
        return;

    i2s_capture_stop(&p->capture);
}

void audio_pipeline_process(audio_pipeline_t *p, audio_output_fn output_fn, void *ctx)
{
    if (!p->initialized || !output_fn)
        return;

    // Poll for new samples from PIO
    i2s_capture_poll(&p->capture);

    // Read available samples from capture ring
    uint32_t available = ap_ring_available(&p->capture_ring);
    if (available == 0)
        return;

    // Limit to buffer size
    if (available > PROCESS_BUFFER_SIZE)
    {
        available = PROCESS_BUFFER_SIZE;
    }

    // Read samples into processing buffer
    for (uint32_t i = 0; i < available; i++)
    {
        process_in[i] = ap_ring_read(&p->capture_ring);
    }

    // Apply DC filter (in-place)
    dc_filter_process_buffer(&p->dc_filter, process_in, available);

    // Apply lowpass filter (anti-aliasing before SRC)
    lowpass_process_buffer(&p->lowpass, process_in, available);

    // Apply sample rate conversion
    uint32_t in_consumed = 0;
    uint32_t out_count = src_process(&p->src,
                                     process_in, available,
                                     process_out, PROCESS_BUFFER_SIZE,
                                     &in_consumed);

    // Output processed samples
    if (out_count > 0)
    {
        output_fn(process_out, out_count, ctx);
        p->samples_output += out_count;
    }
}

void audio_pipeline_poll_buttons(audio_pipeline_t *p)
{
    if (!p->initialized)
        return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Read button states (active low with pull-up)
    bool btn1_pressed = !gpio_get(p->config.pin_btn1);
    bool btn2_pressed = !gpio_get(p->config.pin_btn2);

    // Button 1: Toggle DC filter (on press with debounce)
    if (btn1_pressed && !p->btn1_last_state)
    {
        if (now - p->btn1_last_press > BUTTON_DEBOUNCE_MS)
        {
            dc_filter_toggle(&p->dc_filter);
            p->btn1_last_press = now;
        }
    }
    p->btn1_last_state = btn1_pressed;

    // Button 2: Cycle SRC mode (on press with debounce)
    if (btn2_pressed && !p->btn2_last_state)
    {
        if (now - p->btn2_last_press > BUTTON_DEBOUNCE_MS)
        {
            src_cycle_mode(&p->src);
            p->btn2_last_press = now;
        }
    }
    p->btn2_last_state = btn2_pressed;
}

void audio_pipeline_get_status(audio_pipeline_t *p, audio_pipeline_status_t *status)
{
    if (!p->initialized)
    {
        memset(status, 0, sizeof(*status));
        return;
    }

    status->capture_sample_rate = i2s_capture_get_sample_rate(&p->capture);
    status->samples_captured = p->capture.samples_captured;
    status->capture_overflows = p->capture.overflows;

    status->dc_filter_enabled = p->dc_filter.enabled;
    status->lowpass_enabled = p->lowpass.enabled;
    status->src_mode = p->src.mode;

    status->output_sample_rate = p->src.output_rate;
    status->samples_output = p->samples_output;
    status->output_underruns = p->output_underruns;
}

void audio_pipeline_set_dc_filter(audio_pipeline_t *p, bool enabled)
{
    if (!p->initialized)
        return;
    dc_filter_set_enabled(&p->dc_filter, enabled);
}

void audio_pipeline_set_src_mode(audio_pipeline_t *p, src_mode_t mode)
{
    if (!p->initialized)
        return;
    src_set_mode(&p->src, mode);
}

#include "audio_subsystem.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output_rt.h"

#include "pico/time.h"

#include "hardware/pio.h"

#include <stdio.h>

#include "audio_pipeline.h"
#include "mvs_pins.h"

// Audio pipeline instance
static audio_pipeline_t audio_pipeline;

// Audio state for HSTX encoding
static int audio_frame_counter = 0;
#define AUDIO_COLLECT_SIZE 128
static audio_sample_t audio_collect_buffer[AUDIO_COLLECT_SIZE];
static uint32_t audio_collect_count = 0;

// When true, push silence to HDMI instead of captured samples (CPS2_DIGAV-style: no garbage on power-on/timeout)
static volatile bool audio_output_muted = true;

static const audio_sample_t audio_silence[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};

void audio_subsystem_set_muted(bool muted)
{
    audio_output_muted = muted;
}

static void audio_output_callback(const audio_sample_t *samples, uint32_t count, void *ctx)
{
    (void)ctx;

    for (uint32_t i = 0; i < count; i++) {
        if (audio_collect_count < AUDIO_COLLECT_SIZE) {
            audio_collect_buffer[audio_collect_count++] = samples[i];
        }

        if (audio_collect_count >= 4) {
            hstx_packet_t packet;
            const audio_sample_t *src = audio_output_muted ? audio_silence : audio_collect_buffer;
            int new_frame_counter = hstx_packet_set_audio_samples(&packet, src, 4, audio_frame_counter);

            hstx_data_island_t island;
            hstx_encode_data_island(&island, &packet, false, true);

            if (hstx_di_queue_push(&island)) {
                audio_frame_counter = new_frame_counter;
                audio_collect_count -= 4;
                for (uint32_t j = 0; j < audio_collect_count; j++) {
                    audio_collect_buffer[j] = audio_collect_buffer[j + 4];
                }
            } else {
                break;
            }
        }
    }
}

#ifdef AUDIO_TEST_TONE
// --- Test tone: precomputed 1kHz sine at 48kHz, 48 samples/cycle, 1/8 amplitude ---
// clang-format off
static const int16_t sine_1khz[48] = {
       0,   535,  1060,  1568,  2048,  2494,  2896,  3250,
    3547,  3784,  3957,  4061,  4096,  4061,  3957,  3784,
    3547,  3250,  2896,  2494,  2048,  1568,  1060,   535,
       0,  -535, -1060, -1568, -2048, -2494, -2896, -3250,
   -3547, -3784, -3957, -4061, -4096, -4061, -3957, -3784,
   -3547, -3250, -2896, -2494, -2048, -1568, -1060,  -535,
};
// clang-format on
static uint32_t sine_phase = 0;

static void audio_background_task(void)
{
    // Only produce when DI queue needs filling
    uint32_t level = hstx_di_queue_get_level();
    if (level >= 128)
        return;

    // Feed 16 precomputed samples (4 packets worth) into the output callback
    audio_sample_t samples[16];
    for (int i = 0; i < 16; i++) {
        int16_t v = sine_1khz[sine_phase];
        samples[i].left = v;
        samples[i].right = v;
        sine_phase = (sine_phase + 1) % 48;
    }
    audio_output_callback(samples, 16, NULL);
}
#else
// Global frame count from video_output.c
extern volatile uint32_t video_frame_count;
static uint32_t last_rate_update_frame = 0;

static void audio_background_task_control(void)
{
    if (video_frame_count - last_rate_update_frame >= 30) { // Every ~0.5s (at 60fps)
        last_rate_update_frame = video_frame_count;

        uint32_t level = hstx_di_queue_get_level();
        uint32_t current_rate = audio_pipeline.src.input_rate;
        uint32_t new_rate = current_rate;

        // Target: 128 (half of 256)
        // Deadband: +/- 32 (96 to 160)
        if (level > 160) {
            new_rate += 10;
        } else if (level < 96) {
            new_rate -= 10;
        }

        // Clamp to sane MVS limits (55.5kHz +/- 5%)
        if (new_rate < 53000)
            new_rate = 53000;
        if (new_rate > 58000)
            new_rate = 58000;

        if (new_rate != current_rate) {
            audio_pipeline.src.input_rate = new_rate;
        }
    }
}

static void audio_background_task(void)
{
    audio_background_task_control();

    audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
    while (ap_ring_available(&audio_pipeline.capture_ring) > 0) {
        audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
    }
}
#endif

// Audio startup state machine — runs from Core 1 background task
// Minimal: wait for HSTX → init + start → brief warmup (muted) → unmute → run
// No restart cycle (avoids APB writes while HSTX DMA active).
enum {
    AUDIO_STATE_WAIT_HSTX, // Wait for HSTX to stabilize
    AUDIO_STATE_INIT,      // Initialize GPIO + PIO + DMA + start capture
    AUDIO_STATE_WARM,      // Capture running muted, discard MVS garbage
    AUDIO_STATE_RUNNING,   // Normal operation
};

#define AUDIO_HSTX_SETTLE_FRAMES 120 // ~2s at 60fps
#define AUDIO_WARM_FRAMES 30         // ~0.5s at 60fps

static int audio_state = AUDIO_STATE_WAIT_HSTX;
static uint32_t audio_state_enter_frame = 0;

void audio_subsystem_background_task(void)
{
    switch (audio_state) {
        case AUDIO_STATE_WAIT_HSTX:
            if (video_frame_count >= AUDIO_HSTX_SETTLE_FRAMES) {
                audio_state = AUDIO_STATE_INIT;
            }
            break;

        case AUDIO_STATE_INIT:
            audio_subsystem_init();
#ifdef AUDIO_TEST_TONE
            audio_subsystem_set_muted(false);
            audio_state = AUDIO_STATE_RUNNING;
#else
            audio_subsystem_start();
            audio_state_enter_frame = video_frame_count;
            audio_state = AUDIO_STATE_WARM;
#endif
            break;

        case AUDIO_STATE_WARM:
            // Run muted — capture + SRC run but output is silence.
            // Discards MVS power-on garbage without corrupting filter state
            // (filters are disabled in minimal path).
            audio_background_task();
            if (video_frame_count - audio_state_enter_frame >= AUDIO_WARM_FRAMES) {
                // Flush ring buffer to discard stale warmup samples
                ap_ring_init(&audio_pipeline.capture_ring);
                audio_collect_count = 0;
                audio_subsystem_set_muted(false);
                audio_state = AUDIO_STATE_RUNNING;
            }
            break;

        case AUDIO_STATE_RUNNING:
            audio_background_task();
            break;
    }
}

void audio_subsystem_init(void)
{
    // Initialize PIO for I2S capture
    pio_clear_instruction_memory(pio2);
    pio_set_gpio_base(pio2, 0);

    audio_pipeline_config_t audio_config = {
        .pin_bck = PIN_I2S_BCK, .pin_dat = PIN_I2S_DAT, .pin_ws = PIN_I2S_WS, .pio = pio2, .sm = 0};

    audio_pipeline_init(&audio_pipeline, &audio_config);
}

void audio_subsystem_start(void)
{
    audio_pipeline_start(&audio_pipeline);
}

void audio_subsystem_stop(void)
{
    audio_pipeline_stop(&audio_pipeline);

    // Flush all buffers and reset processing state
    ap_ring_init(&audio_pipeline.capture_ring);
    audio_pipeline.src.accumulator = 0;
    audio_collect_count = 0;
    audio_frame_counter = 0;
}

// ============================================================================
// Pre-fill DI queue with silence (call before Core 1 launch)
// ============================================================================

void audio_subsystem_prefill_di_queue(void)
{
    while (hstx_di_queue_get_level() < 200) {
        hstx_packet_t packet;
        int fc = hstx_packet_set_audio_samples(&packet, audio_silence, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        if (!hstx_di_queue_push(&island))
            break;
        audio_frame_counter = fc;
    }
}

// ============================================================================
// Core 0 audio poll — simple warmup then run
// ============================================================================

#define CORE0_WARMUP_US 2000000ULL       // 2.0s
#define CORE0_RELOCK_SETTLE_US 300000ULL // 0.3s

static bool core0_audio_unmuted = false;
static uint64_t core0_start_time = 0;
static bool core0_relock_done = false;
static uint64_t core0_relock_time = 0;

void audio_subsystem_core0_poll(void)
{
    if (core0_start_time == 0)
        core0_start_time = time_us_64();

    uint64_t now = time_us_64();

    // Process audio every call (muted output during warmup)
    audio_background_task();

    // One-shot hard relock: when MVS clocks are stable, re-arm capture state.
    // This mirrors the "press Pico reset after power-on" recovery, but only for
    // the audio block (no HSTX/ISR impact).
    if (!core0_relock_done && (now - core0_start_time >= CORE0_WARMUP_US)) {
        audio_pipeline_stop(&audio_pipeline);
        ap_ring_init(&audio_pipeline.capture_ring);
        audio_collect_count = 0;
        audio_pipeline.src.accumulator = 0;
        audio_pipeline.src.phase = 0;
        audio_pipeline.src.have_prev = false;
        audio_pipeline_start(&audio_pipeline);
        core0_relock_done = true;
        core0_relock_time = now;
        return;
    }

    // Keep muted briefly after relock, then unmute with clean state.
    if (core0_relock_done && !core0_audio_unmuted && (now - core0_relock_time >= CORE0_RELOCK_SETTLE_US)) {
        ap_ring_init(&audio_pipeline.capture_ring);
        audio_collect_count = 0;
        audio_pipeline.src.accumulator = 0;
        audio_pipeline.src.phase = 0;
        audio_pipeline.src.have_prev = false;
        audio_subsystem_set_muted(false);
        core0_audio_unmuted = true;
    }
}

#include "audio_subsystem.h"
#include "audio_pipeline.h"
#include "hardware/pio.h"
#include "mvs_pins.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico_dvi2/hstx_packet.h"
#include "pico_dvi2/video_output.h"
#include <stdio.h>

// Audio pipeline instance
static audio_pipeline_t audio_pipeline;

// Audio state for HSTX encoding
static int audio_frame_counter = 0;
#define AUDIO_COLLECT_SIZE 128
static audio_sample_t audio_collect_buffer[AUDIO_COLLECT_SIZE];
static uint32_t audio_collect_count = 0;

static void audio_output_callback(const audio_sample_t *samples, uint32_t count,
                                  void *ctx) {
  (void)ctx;

  for (uint32_t i = 0; i < count; i++) {
    if (audio_collect_count < AUDIO_COLLECT_SIZE) {
      audio_collect_buffer[audio_collect_count++] = samples[i];
    }

    if (audio_collect_count >= 4) {
      hstx_packet_t packet;
      // Get the new frame counter but don't commit it yet
      int new_frame_counter = hstx_packet_set_audio_samples(
          &packet, audio_collect_buffer, 4, audio_frame_counter);

      hstx_data_island_t island;
      hstx_encode_data_island(&island, &packet, false, true);

      if (hstx_di_queue_push(&island)) {
        // Only advance frame counter after successful push to maintain
        // IEC 60958 block synchronization (B_FLAG every 192 samples)
        audio_frame_counter = new_frame_counter;
        audio_collect_count -= 4;
        for (uint32_t j = 0; j < audio_collect_count; j++) {
          audio_collect_buffer[j] = audio_collect_buffer[j + 4];
        }
      } else {
        break; // Queue full - frame counter NOT advanced, packet will be retried
      }
    }
  }
}

static void audio_background_task(void) {
  audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
  while (ap_ring_available(&audio_pipeline.capture_ring) > 0) {
    audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
  }
}

void audio_subsystem_init(void) {
  // Initialize PIO for I2S capture
  pio_clear_instruction_memory(pio2);
  pio_set_gpio_base(pio2, 0);

  audio_pipeline_config_t audio_config = {.pin_bck = 2, // I2S_BCK_PIN
                                          .pin_dat = 0, // I2S_DAT_PIN
                                          .pin_ws = 1,  // I2S_WS_PIN
                                          .pin_btn1 = PIN_OSD_BTN_MENU,
                                          .pin_btn2 = PIN_OSD_BTN_BACK,
                                          .pio = pio2,
                                          .sm = 0};

  audio_pipeline_init(&audio_pipeline, &audio_config);

  // Register with video output Core 1 loop
  video_output_set_background_task(audio_background_task);
}

void audio_subsystem_start(void) { audio_pipeline_start(&audio_pipeline); }

void audio_subsystem_stop(void) { audio_pipeline_stop(&audio_pipeline); }

/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 */

#include "audio_subsystem.h"
#include "hardware/clocks.h"
#include "mvs_pins.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico_dvi2/video_output.h"
#include "video_capture.h"
#include "osd/osd.h"
#include <stdio.h>
#include <string.h>

// Main video framebuffer
uint16_t framebuf[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH] __attribute__((aligned(4)));

// Initialize framebuffer to black (called once at init)
static void init_background(void) { memset(framebuf, 0, sizeof(framebuf)); }

volatile bool video_signal_ok = false;

// ============================================================================
// DVI Scanline Callback (Core 1)
// ============================================================================

/**
 * This function is called by the DVI library on Core 1 for every active video line.
 * It performs 2x vertical scaling (line doubling) from 240p to 480p.
 */
void __scratch_x("") mvs_scanline_doubler(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer) {
  // 2x Vertical Scaling: Every 240p line is shown twice to reach 480p
  uint32_t fb_line = active_line / 2;
  
  if (fb_line >= FRAMEBUF_HEIGHT) {
    memset(line_buffer, 0, MODE_H_ACTIVE_PIXELS * 2);
    return;
  }

  const uint16_t *src = &framebuf[fb_line * FRAMEBUF_WIDTH];
  uint32_t *dst = line_buffer;

  // Box bounds (Same as before for visual consistency)
  const uint32_t box_x = 10;
  const uint32_t box_y = 10;
  const uint32_t box_w = 16;
  const uint32_t box_h = 16;
  bool in_box_y = (fb_line >= box_y && fb_line < (box_y + box_h));

  // Determine OSD color
  uint16_t osd_color = 0x07E0; // Green default
  if (!video_signal_ok) {
      // Blink red every ~30 frames (0.5s)
      if ((video_frame_count / 30) % 2) {
          osd_color = 0xF800; // Red
      } else {
          osd_color = 0x0000; // Transparent/Black (Wait, transparent implies video copy?)
          // If no signal, video is static/noise. 
          // Let's use Black (0x0000) but mark span as SOLID to obscure the noise?
          // Or just standard blink.
          // Let's stick to simple color swap.
      }
  }

  // Construct primitive Span List on the stack (Simulating pre-calculated data)
  osd_span_t spans[3];
  int span_count = 0;

  if (in_box_y) {
      // Span 1: Left gap (Transparent)
      spans[0].length = box_x;
      spans[0].is_solid = false;
      
      // Span 2: The Box (Solid Color)
      spans[1].length = box_w;
      spans[1].is_solid = true; // Always draw box pixels
      spans[1].color = osd_color;
      
      // Span 3: Right side (Transparent)
      spans[2].length = FRAMEBUF_WIDTH - (box_x + box_w);
      spans[2].is_solid = false;
      span_count = 3;
  } else {
      // Full line transparent
      spans[0].length = FRAMEBUF_WIDTH;
      spans[0].is_solid = false;
      span_count = 1;
  }

  // RENDERER: Iterate the spans
  // This is the generic logic we want to use permanently.
  for (int s = 0; s < span_count; s++) {
      uint16_t len = spans[s].length;
      
      if (spans[s].is_solid) {
          // Solid Color Fill
          uint32_t color32 = spans[s].color | (spans[s].color << 16);
          for (int i = 0; i < len; i++) {
              *dst++ = color32;
          }
          // Advance src pointer even though we didn't use it, to keep sync
          src += len;
      } else {
          // Transparent: Copy Video
          for (int i = 0; i < len; i++) {
              uint32_t p = *src++;
              *dst++ = p | (p << 16);
          }
      }
  }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void) {
  // Set system clock to 126 MHz for exact 25.2 MHz pixel clock
  set_sys_clock_khz(126000, true);

  stdio_init_all();

  // Initialize LED for heartbeat
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

  sleep_ms(1000);

  // Flush any pending output
  stdio_flush();

  // Initialize shared resources before launching Core 1
  init_background();
  hstx_di_queue_init();
  video_output_init();
  
  // Register the scanline doubler callback (with overlay)
  video_output_set_scanline_callback(mvs_scanline_doubler);

  // 1. Initialize video capture
  video_capture_init(framebuf, FRAMEBUF_WIDTH, FRAMEBUF_HEIGHT, 224);
  sleep_ms(200);

  // 2. Initialize audio subsystem
  audio_subsystem_init();
  audio_subsystem_start();
  stdio_flush();

  // 3. Launch Core 1 for HSTX output
  multicore_launch_core1(video_output_core1_run);
  sleep_ms(100);

  uint32_t led_toggle_frame = 0;
  bool led_state = false;

  while (1) {
    if (video_capture_frame()) {
        video_signal_ok = true;
    } else {
        video_signal_ok = false;
        audio_subsystem_stop();
        audio_subsystem_start();
    }

    if (video_frame_count >= led_toggle_frame + 30) {
      led_state = !led_state;
      gpio_put(PICO_DEFAULT_LED_PIN, led_state);
      led_toggle_frame = video_frame_count;
    }
  }

  return 0;
}

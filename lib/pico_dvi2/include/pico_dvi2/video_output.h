#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include "pico_dvi2/video_config.h"
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Video Output Configuration
// ============================================================================

#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

#define MODE_H_TOTAL_PIXELS                                                    \
  (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH +                \
   MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES                                                     \
  (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH +                \
   MODE_V_ACTIVE_LINES)

// Framebuffer resolution (MVS native, 2x scaled to 640x480)
#define FRAMEBUF_WIDTH FRAME_WIDTH
#define FRAMEBUF_HEIGHT FRAME_HEIGHT

// ============================================================================
// Global State
// ============================================================================

extern uint16_t framebuf[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH];
extern volatile uint32_t video_frame_count;

// ============================================================================
// Public Interface
// ============================================================================

typedef void (*video_output_task_fn)(void);

/**
 * Initialize HSTX and DMA for video output.
 */
void video_output_init(void);

/**
 * Register a background task to run in the Core 1 loop.
 * This is typically used for audio processing.
 */
void video_output_set_background_task(video_output_task_fn task);

/**
 * Core 1 entry point for video output.
 * This function does not return.
 */
void video_output_core1_run(void);

#endif // VIDEO_OUTPUT_H

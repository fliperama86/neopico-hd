#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include "video_config.h"
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

/**
 * Initialize HSTX and DMA for video output.
 */
void video_output_init(void);

/**
 * Push audio samples to the HDMI audio ring buffer.
 * Encodes samples into Data Islands when enough are collected.
 */
#include "data_packet.h"
void video_output_push_audio_samples(const audio_sample_t *samples,
                                     uint32_t count);

/**
 * Core 1 entry point for video output and audio processing.
 * This function does not return.
 */
void video_output_core1_run(void);

#endif // VIDEO_OUTPUT_H

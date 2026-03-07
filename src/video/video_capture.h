#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include "pico/types.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize MVS video capture
 *
 * @param mvs_height Active video lines from MVS (e.g., 224)
 */
void video_capture_init(uint mvs_height);

/**
 * Run the video capture loop (never returns)
 * Captures lines into the global line ring buffer
 * Signals VSYNC to Core 1 at frame boundaries
 */
void video_capture_run(void);

/**
 * Get current frame count
 */
uint32_t video_capture_get_frame_count(void);

#if NEOPICO_EXP_GENLOCK_DYNAMIC
/**
 * Timestamp (timer_hw->timerawl) of the most recent MVS VSYNC, written by Core 0.
 */
extern volatile uint32_t g_mvs_vsync_timestamp;
#endif

#endif // VIDEO_CAPTURE_H

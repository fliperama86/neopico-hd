#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/types.h"

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

#endif // VIDEO_CAPTURE_H

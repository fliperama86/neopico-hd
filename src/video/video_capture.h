#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/types.h"

/**
 * Initialize MVS video capture
 *
 * @param framebuffer Pointer to RGB565 framebuffer to write captured frames
 * @param frame_width Width of framebuffer (e.g., 320)
 * @param frame_height Height of framebuffer (e.g., 240)
 * @param mvs_height Active video lines from MVS (e.g., 224)
 */
void video_capture_init(uint16_t *framebuffer, uint frame_width, uint frame_height, uint mvs_height);

/**
 * Capture one frame from MVS
 * Blocks until a complete frame is captured
 *
 * @return true on success, false on timeout
 */
bool video_capture_frame(void);

/**
 * Get current frame count
 */
uint32_t video_capture_get_frame_count(void);

#endif // VIDEO_CAPTURE_H

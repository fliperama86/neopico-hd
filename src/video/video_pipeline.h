#ifndef VIDEO_PIPELINE_H
#define VIDEO_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "pico.h"

// Video effect toggles
extern bool fx_scanlines_enabled;

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 *
 * @param frame_width Output frame width (e.g., 640)
 * @param frame_height Output frame height (e.g., 480)
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height);

/**
 * Fast 2x pixel doubling without any effect.
 * Processes 32-bits (2 pixels) at a time for efficiency.
 *
 * @param dst Destination buffer (must be 32-bit aligned)
 * @param src Source RGB565 buffer
 * @param count Number of source pixels to process
 */
void __scratch_x("") video_pipeline_double_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

/**
 * Fast 2x pixel doubling with 50% darkening for scanlines.
 * Processes 32-bits (2 pixels) at a time for efficiency.
 *
 * @param dst Destination buffer (must be 32-bit aligned)
 * @param src Source RGB565 buffer
 * @param count Number of source pixels to process
 */
void __scratch_x("") video_pipeline_double_pixels_scanline(uint32_t *dst, const uint16_t *src, int count);

/**
 * Scanline doubler callback for HDMI output.
 * Called by Core 1 DMA ISR for every active video line.
 * Performs 2x vertical scaling (line doubling) from 240p to 480p.
 * Handles OSD overlay with loop splitting for no per-pixel branching.
 *
 * @param v_scanline Current vertical scanline (unused)
 * @param active_line Current active line in 480p output space
 * @param dst Destination buffer for doubled pixels
 */
void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 */
void __scratch_x("") video_pipeline_vsync_callback(void);

#endif // VIDEO_PIPELINE_H

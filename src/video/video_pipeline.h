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
 */
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

/**
 * Fast 3x pixel scaling for 720p 4:3 pillarboxed mode.
 * Two source pixels produce six output pixels (three uint32_t words).
 */
void __scratch_y("") video_pipeline_triple_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

/**
 * Fast 4x pixel quadrupling for 240p direct mode.
 * Each source pixel produces 4 output pixels (2 uint32_t words).
 */
void __scratch_y("") video_pipeline_quadruple_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

/**
 * Scanline callback for HDMI output.
 * Called by Core 1 DMA ISR for every active video line.
 * Mode-aware: 480p uses 2x, 240p uses 4x, 720p uses centered 3x.
 */
void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 */
void __scratch_x("") video_pipeline_vsync_callback(void);

#endif // VIDEO_PIPELINE_H

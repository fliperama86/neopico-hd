#ifndef VIDEO_PIPELINE_H
#define VIDEO_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "pico.h"

// Video effect toggles
extern bool fx_scanlines_enabled;

/**
 * Fast 2x pixel doubling with darkening effect for scanlines.
 * Processes 32-bits (2 pixels) at a time for efficiency.
 * 
 * @param dst Destination buffer (must be 32-bit aligned)
 * @param src Source RGB565 buffer
 * @param count Number of source pixels to process
 */
void __scratch_x("") video_pipeline_double_pixels_scanline(uint32_t *dst, const uint16_t *src, int count);

#endif // VIDEO_PIPELINE_H

#include "video_pipeline.h"
#include "pico.h"
#include <string.h>

// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;

/**
 * Fast 2x pixel doubling with 50% darkening for scanlines.
 * Uses bit-masking to prevent color bleed across channels.
 * RGB565: RRRRR GGGGGG BBBBB
 * 50% Darken: (pixel & 0xF7DE) >> 1
 */
void __scratch_x("")
    video_pipeline_double_pixels_scanline(uint32_t *dst, const uint16_t *src,
                                          int count) {
  const uint32_t *src32 = (const uint32_t *)src;
  int pairs = count / 2;

  for (int i = 0; i < pairs; i++) {
    uint32_t two = src32[i];

    // Extract and darken first pixel
    uint32_t p0 = (two & 0xFFFF);
    p0 = (p0 & 0xF7DE) >> 1;

    // Extract and darken second pixel
    uint32_t p1 = (two >> 16);
    p1 = (p1 & 0xF7DE) >> 1;

    // Double each pixel and store
    dst[i * 2] = p0 | (p0 << 16);
    dst[i * 2 + 1] = p1 | (p1 << 16);
  }
}

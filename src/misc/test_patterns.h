/**
 * Test Pattern Generator
 *
 * Generates standard video test patterns (color bars, gradients, etc.)
 * for verifying HDMI/DVI output independent of capture hardware.
 */

#ifndef TEST_PATTERNS_H
#define TEST_PATTERNS_H

#include <stdint.h>
#include "pico/types.h"

/**
 * Fill framebuffer with SMPTE color bars (RGB565 format)
 *
 * @param framebuf Pointer to RGB565 framebuffer
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 */
void fill_color_bars(uint16_t *framebuf, uint width, uint height);

#endif // TEST_PATTERNS_H

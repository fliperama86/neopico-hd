/**
 * Test Pattern Generator
 */

#include "test_patterns.h"
#include "pico/types.h"

void fill_color_bars(uint16_t *framebuf, uint width, uint height) {
    // 8 vertical bars: Red, Green, Blue, Yellow, Magenta, Cyan, White, Black
    const uint16_t colors[8] = {
        0xF800,  // Red
        0x07E0,  // Green
        0x001F,  // Blue
        0xFFE0,  // Yellow (R+G)
        0xF81F,  // Magenta (R+B)
        0x07FF,  // Cyan (G+B)
        0xFFFF,  // White
        0x0000   // Black
    };

    uint bar_width = width / 8;

    for (uint y = 0; y < height; y++) {
        for (uint x = 0; x < width; x++) {
            uint bar = x / bar_width;
            if (bar >= 8) bar = 7;  // Clamp to black for remainder
            framebuf[y * width + x] = colors[bar];
        }
    }
}

#ifndef DEBUG_RENDER_H
#define DEBUG_RENDER_H

#include "pico/types.h"
#include <stdint.h>

// Color constants (RGB565)
#define DEBUG_COLOR_WHITE 0xFFFF
#define DEBUG_COLOR_BLACK 0x0000
#define DEBUG_COLOR_GREEN 0x07E0
#define DEBUG_COLOR_RED 0xF800
#define DEBUG_COLOR_YELLOW 0xFFE0
#define DEBUG_COLOR_BLUE 0x001F

/**
 * Draw a single character to the framebuffer
 */
void debug_draw_char(uint16_t *fb, uint fb_w, uint fb_h, int x, int y, char c,
                     uint16_t color, uint16_t bg);

/**
 * Draw a string to the framebuffer
 */
void debug_draw_string(uint16_t *fb, uint fb_w, uint fb_h, int x, int y,
                       const char *str, uint16_t color, uint16_t bg);

#endif // DEBUG_RENDER_H

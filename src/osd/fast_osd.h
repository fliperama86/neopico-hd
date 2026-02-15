#ifndef FAST_OSD_H
#define FAST_OSD_H

#include <stdbool.h>
#include <stdint.h>

// OSD box dimensions (in 320x240 space, doubled at output)
#define OSD_BOX_X 48  // Start X position
#define OSD_BOX_Y 56  // Start Y position
#define OSD_BOX_W 224 // Width in pixels (must be multiple of 8)
#define OSD_BOX_H 128 // Height in pixels

// Colors (RGB565)
#define OSD_COLOR_BG 0x0000 // Black background
#define OSD_COLOR_FG 0xFFFF // White text
#define OSD_COLOR_GREEN 0x07E0
#define OSD_COLOR_RED 0xF800
#define OSD_COLOR_YELLOW 0xFFE0
#define OSD_COLOR_GRAY 0x7BEF // Gray text

#define FAST_OSD_COLS 28
#define FAST_OSD_ROWS 16
#define FAST_OSD_GLYPH_CHECK ((char)0x01)
#define FAST_OSD_GLYPH_CROSS ((char)0x02)

extern volatile bool osd_visible;
extern uint16_t osd_framebuffer[OSD_BOX_H][OSD_BOX_W];

static inline void osd_show(void)
{
    osd_visible = true;
}

static inline void osd_hide(void)
{
    osd_visible = false;
}

static inline void osd_toggle(void)
{
    osd_visible = !osd_visible;
}

void fast_osd_init(void);
void fast_osd_clear(void);
void fast_osd_putc(uint8_t row, uint8_t col, char c);
void fast_osd_putc_color(uint8_t row, uint8_t col, char c, uint16_t color);
void fast_osd_puts(uint8_t row, uint8_t col, const char *text);
void fast_osd_puts_color(uint8_t row, uint8_t col, const char *text, uint16_t color);
const char *fast_osd_get_row(uint8_t row);

#endif // FAST_OSD_H

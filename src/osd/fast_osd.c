#include "fast_osd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "font_8x8.h"
#include "pico.h"

volatile bool osd_visible = false;
uint16_t __attribute__((aligned(4))) osd_framebuffer[OSD_BOX_H][OSD_BOX_W];

// Text grid: one NUL-terminated string per row.
static char fast_osd_text[FAST_OSD_ROWS][FAST_OSD_COLS + 1];
// Foreground color per cell.
static uint16_t fast_osd_color[FAST_OSD_ROWS][FAST_OSD_COLS];

static inline bool fast_osd_in_bounds(uint8_t row, uint8_t col)
{
    return row < FAST_OSD_ROWS && col < FAST_OSD_COLS;
}

static inline uint8_t fast_osd_normalize_char(char c)
{
    const uint8_t ch = (uint8_t)c;
    if (ch == (uint8_t)FAST_OSD_GLYPH_CHECK || ch == (uint8_t)FAST_OSD_GLYPH_CROSS) {
        return ch;
    }
    if (ch < 32U || ch > 126U) {
        return (uint8_t)' ';
    }
    return ch;
}

static inline void fast_osd_render_cell(uint8_t row, uint8_t col, char c, uint16_t color)
{
    if (!fast_osd_in_bounds(row, col)) {
        return;
    }

    const uint8_t ch = fast_osd_normalize_char(c);
    const int x = col * 8;
    const int y = row * 8;
    const uint8_t *glyph = font8x8[ch];

    for (int glyph_row = 0; glyph_row < 8; glyph_row++) {
        const uint8_t bits = glyph[glyph_row];
        uint16_t *dst_row = &osd_framebuffer[y + glyph_row][x];
        dst_row[0] = (bits & 0x80) ? color : OSD_COLOR_BG;
        dst_row[1] = (bits & 0x40) ? color : OSD_COLOR_BG;
        dst_row[2] = (bits & 0x20) ? color : OSD_COLOR_BG;
        dst_row[3] = (bits & 0x10) ? color : OSD_COLOR_BG;
        dst_row[4] = (bits & 0x08) ? color : OSD_COLOR_BG;
        dst_row[5] = (bits & 0x04) ? color : OSD_COLOR_BG;
        dst_row[6] = (bits & 0x02) ? color : OSD_COLOR_BG;
        dst_row[7] = (bits & 0x01) ? color : OSD_COLOR_BG;
    }
}

void fast_osd_clear(void)
{
    uint32_t *dst32 = (uint32_t *)osd_framebuffer;
    const uint32_t bg32 = OSD_COLOR_BG | ((uint32_t)OSD_COLOR_BG << 16);
    const uint32_t words = (OSD_BOX_W * OSD_BOX_H) / 2;
    for (uint32_t i = 0; i < words; i++) {
        dst32[i] = bg32;
    }

    for (uint8_t r = 0; r < FAST_OSD_ROWS; r++) {
        memset(fast_osd_text[r], ' ', FAST_OSD_COLS);
        for (uint8_t c = 0; c < FAST_OSD_COLS; c++) {
            fast_osd_color[r][c] = OSD_COLOR_FG;
        }
        fast_osd_text[r][FAST_OSD_COLS] = '\0';
    }
}

void fast_osd_init(void)
{
    fast_osd_clear();
}

void fast_osd_putc(uint8_t row, uint8_t col, char c)
{
    fast_osd_putc_color(row, col, c, OSD_COLOR_FG);
}

void fast_osd_putc_color(uint8_t row, uint8_t col, char c, uint16_t color)
{
    if (!fast_osd_in_bounds(row, col)) {
        return;
    }

    const char norm = (char)fast_osd_normalize_char(c);
    if (fast_osd_text[row][col] == norm && fast_osd_color[row][col] == color) {
        return; // Nothing changed; skip render.
    }

    fast_osd_text[row][col] = norm;
    fast_osd_color[row][col] = color;
    fast_osd_render_cell(row, col, norm, color);
}

void fast_osd_puts(uint8_t row, uint8_t col, const char *text)
{
    fast_osd_puts_color(row, col, text, OSD_COLOR_FG);
}

void fast_osd_puts_color(uint8_t row, uint8_t col, const char *text, uint16_t color)
{
    if (!text || row >= FAST_OSD_ROWS || col >= FAST_OSD_COLS) {
        return;
    }
    while (*text && col < FAST_OSD_COLS) {
        fast_osd_putc_color(row, col, *text++, color);
        col++;
    }
}

const char *fast_osd_get_row(uint8_t row)
{
    if (row >= FAST_OSD_ROWS) {
        return "";
    }
    return fast_osd_text[row];
}

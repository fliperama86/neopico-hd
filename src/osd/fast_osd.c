#include "fast_osd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "font_8x8.h"
#include "pico.h"

#define FAST_OSD_CHAR_MIN 32
#define FAST_OSD_CHAR_MAX 126
#define FAST_OSD_CHAR_COUNT (FAST_OSD_CHAR_MAX - FAST_OSD_CHAR_MIN + 1)

volatile bool osd_visible = false;
uint16_t __attribute__((aligned(4))) osd_framebuffer[OSD_BOX_H][OSD_BOX_W];

// Text grid: one NUL-terminated string per row.
static char fast_osd_text[FAST_OSD_ROWS][FAST_OSD_COLS + 1];
// Pre-expanded RGB565 glyph tiles for printable ASCII (32..126).
static uint16_t __attribute__((aligned(4))) fast_osd_glyph_tiles[FAST_OSD_CHAR_COUNT][8][8];

static inline bool fast_osd_in_bounds(uint8_t row, uint8_t col)
{
    return row < FAST_OSD_ROWS && col < FAST_OSD_COLS;
}

static inline uint8_t fast_osd_normalize_char(char c)
{
    if ((unsigned char)c < FAST_OSD_CHAR_MIN || (unsigned char)c > FAST_OSD_CHAR_MAX) {
        return (uint8_t)' ';
    }
    return (uint8_t)c;
}

static void fast_osd_build_glyph_tiles(void)
{
    for (uint8_t code = FAST_OSD_CHAR_MIN; code <= FAST_OSD_CHAR_MAX; code++) {
        const uint8_t tile_idx = (uint8_t)(code - FAST_OSD_CHAR_MIN);
        const uint8_t *glyph = font8x8[code];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            uint16_t *dst_row = fast_osd_glyph_tiles[tile_idx][row];
            dst_row[0] = (bits & 0x80) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[1] = (bits & 0x40) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[2] = (bits & 0x20) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[3] = (bits & 0x10) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[4] = (bits & 0x08) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[5] = (bits & 0x04) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[6] = (bits & 0x02) ? OSD_COLOR_FG : OSD_COLOR_BG;
            dst_row[7] = (bits & 0x01) ? OSD_COLOR_FG : OSD_COLOR_BG;
        }
    }
}

static inline void fast_osd_render_cell(uint8_t row, uint8_t col, char c)
{
    if (!fast_osd_in_bounds(row, col)) {
        return;
    }

    const uint8_t ch = fast_osd_normalize_char(c);
    const uint8_t tile_idx = (uint8_t)(ch - FAST_OSD_CHAR_MIN);
    const int x = col * 8;
    const int y = row * 8;
    const uint32_t *src32 = (const uint32_t *)fast_osd_glyph_tiles[tile_idx];

    for (int glyph_row = 0; glyph_row < 8; glyph_row++) {
        uint32_t *dst32 = (uint32_t *)&osd_framebuffer[y + glyph_row][x];
        const uint32_t *row_src32 = src32 + (glyph_row * 4);
        memcpy(dst32, row_src32, 16);
    }
}

void fast_osd_init(void)
{
    fast_osd_build_glyph_tiles();

    // Clear full framebuffer to background.
    uint32_t *dst32 = (uint32_t *)osd_framebuffer;
    const uint32_t bg32 = OSD_COLOR_BG | ((uint32_t)OSD_COLOR_BG << 16);
    const uint32_t words = (OSD_BOX_W * OSD_BOX_H) / 2;
    for (uint32_t i = 0; i < words; i++) {
        dst32[i] = bg32;
    }

    for (uint8_t r = 0; r < FAST_OSD_ROWS; r++) {
        memset(fast_osd_text[r], ' ', FAST_OSD_COLS);
        fast_osd_text[r][FAST_OSD_COLS] = '\0';
    }
}

void fast_osd_putc(uint8_t row, uint8_t col, char c)
{
    if (!fast_osd_in_bounds(row, col)) {
        return;
    }
    c = (char)fast_osd_normalize_char(c);
    if (fast_osd_text[row][col] == c) {
        return; // Nothing changed; skip render.
    }
    fast_osd_text[row][col] = c;
    fast_osd_render_cell(row, col, c);
}

void fast_osd_puts(uint8_t row, uint8_t col, const char *text)
{
    if (!text || row >= FAST_OSD_ROWS || col >= FAST_OSD_COLS) {
        return;
    }
    while (*text && col < FAST_OSD_COLS) {
        fast_osd_putc(row, col, *text++);
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

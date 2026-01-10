#include "osd.h"
#include "font_8x8.h"
#include <string.h>

// OSD state
volatile bool osd_visible = false;

// Pre-rendered RGB565 framebuffer for OSD box
// Aligned for efficient 32-bit access
uint16_t __attribute__((aligned(4)))
    osd_framebuffer[OSD_BOX_H][OSD_BOX_W];

void osd_init(void) {
    osd_clear();
    osd_visible = false;
}

void osd_clear(void) {
    // Fill with background color using 32-bit writes
    uint32_t bg32 = OSD_COLOR_BG | (OSD_COLOR_BG << 16);
    uint32_t *dst = (uint32_t *)osd_framebuffer;
    uint32_t count = (OSD_BOX_W * OSD_BOX_H) / 2;

    for (uint32_t i = 0; i < count; i++) {
        dst[i] = bg32;
    }
}

void osd_putchar(int x, int y, char c) {
    // Bounds check
    if (x < 0 || x + 8 > OSD_BOX_W || y < 0 || y + 8 > OSD_BOX_H)
        return;

    // Clamp to valid ASCII range (use space for invalid chars)
    if (c < 32 || c > 126)
        c = ' ';

    // New font format: font8x8[ascii][row] - direct indexing
    const uint8_t *glyph = font8x8[(uint8_t)c];

    // Render 8 rows
    for (int row = 0; row < 8; row++) {
        uint8_t line = glyph[row];
        uint16_t *dst_row = &osd_framebuffer[y + row][x];

        // Unrolled 8-pixel write (bit 7 = leftmost pixel, bit 0 = rightmost)
        dst_row[0] = (line & 0x80) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[1] = (line & 0x40) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[2] = (line & 0x20) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[3] = (line & 0x10) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[4] = (line & 0x08) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[5] = (line & 0x04) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[6] = (line & 0x02) ? OSD_COLOR_FG : OSD_COLOR_BG;
        dst_row[7] = (line & 0x01) ? OSD_COLOR_FG : OSD_COLOR_BG;
    }
}

void osd_puts(int x, int y, const char *str) {
    while (*str) {
        osd_putchar(x, y, *str++);
        x += 8;
        if (x + 8 > OSD_BOX_W)
            break;
    }
}

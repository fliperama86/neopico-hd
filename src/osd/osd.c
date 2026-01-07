#include "osd.h"
#include "font_8x8.h"
#include <string.h>

// 1bpp buffer (320x240 bits = 9600 bytes)
uint32_t osd_buffer[OSD_WIDTH * OSD_HEIGHT / 32] __attribute__((aligned(4)));
bool osd_visible = true;
uint16_t osd_text_color = 0xFFFF; // White (RGB565)

void osd_init(void) { osd_clear(); }

void osd_clear(void) { memset(osd_buffer, 0, sizeof(osd_buffer)); }

void osd_draw_pixel(int x, int y, bool val) {
  if (x < 0 || x >= OSD_WIDTH || y < 0 || y >= OSD_HEIGHT)
    return;

  uint32_t idx = (y * OSD_WIDTH + x);
  if (val) {
    osd_buffer[idx >> 5] |= (1u << (idx & 31));
  } else {
    osd_buffer[idx >> 5] &= ~(1u << (idx & 31));
  }
}

void osd_draw_char(int x, int y, char c, bool val) {
  if (c < 32 || c > 126)
    return;

  // font_8x8 from lib/PicoDVI assets is interleaved:
  // row 0 of all chars, then row 1 of all chars, etc.
  // There are 95 characters (ASCII 32 to 126).
  for (int row = 0; row < 8; row++) {
    uint8_t line = font_8x8[(c - 32) + (row * 95)];
    for (int col = 0; col < 8; col++) {
      if ((line >> col) & 1) {
        osd_draw_pixel(x + col, y + row, val);
      }
    }
  }
}

void osd_draw_string(int x, int y, const char *str, bool val) {
  while (*str) {
    osd_draw_char(x, y, *str++, val);
    x += 8;
    if (x + 8 > OSD_WIDTH)
      break;
  }
}


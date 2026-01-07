#ifndef OSD_H
#define OSD_H

#include <stdbool.h>
#include <stdint.h>

#define OSD_WIDTH 320
#define OSD_HEIGHT 240
#define OSD_BUFFER_SIZE (OSD_WIDTH * OSD_HEIGHT / 8)

// Global OSD buffer and state
extern uint32_t osd_buffer[OSD_WIDTH * OSD_HEIGHT / 32];
extern bool osd_visible;
extern uint16_t osd_text_color;

// Initialize OSD system
void osd_init(void);

// Clear the OSD buffer
void osd_clear(void);

// Draw a single pixel on the OSD buffer
void osd_draw_pixel(int x, int y, bool val);

// Draw a character using the internal 8x8 font
void osd_draw_char(int x, int y, char c, bool val);

// Draw a string
void osd_draw_string(int x, int y, const char *str, bool val);

// Check if a pixel is active at given coordinates
static inline bool osd_get_pixel(int x, int y) {
  uint32_t idx = (y * OSD_WIDTH + x);
  return (osd_buffer[idx >> 5] >> (idx & 31)) & 1;
}

#endif // OSD_H

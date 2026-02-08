#ifndef OSD_H
#define OSD_H

#include <stdbool.h>
#include <stdint.h>

// OSD box dimensions (in 320x240 space, will be doubled to 640x480)
#define OSD_BOX_X 80  // Start X position
#define OSD_BOX_Y 88  // Start Y position
#define OSD_BOX_W 160 // Width in pixels (must be multiple of 8)
#define OSD_BOX_H 64  // Height in pixels

// Colors (RGB565)
#define OSD_COLOR_BG 0x0000 // Black background
#define OSD_COLOR_FG 0xFFFF // White text

// OSD state
extern volatile bool osd_visible;

// Pre-rendered RGB565 buffer for the OSD box
// Placed in scratch_y to avoid bus contention with Core 0
extern uint16_t osd_framebuffer[OSD_BOX_H][OSD_BOX_W];

// Initialize OSD system
void osd_init(void);

// Clear OSD to background color
void osd_clear(void);

// Draw a character at (x,y) relative to OSD box origin
void osd_putchar(int x, int y, char c);

// Draw a string at (x,y) relative to OSD box origin
void osd_puts(int x, int y, const char *str);

// Show/hide OSD
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

void osd_background_task(void);
#endif // OSD_H

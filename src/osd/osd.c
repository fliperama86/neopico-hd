#include "osd.h"

#include "pico/time.h"

#include "hardware/gpio.h"

#include <string.h>

#include "font_8x8.h"
#include "mvs_pins.h"
#include "selftest.h"
#include "video/video_config.h"

// OSD state
volatile bool osd_visible = false;

// Button IRQ → background task flag
static volatile bool osd_toggle_pending = false;

// Pre-rendered RGB565 framebuffer for OSD box
// Aligned for efficient 32-bit access
uint16_t __attribute__((aligned(4))) osd_framebuffer[OSD_BOX_H][OSD_BOX_W];

void osd_init(void)
{
    osd_setup_button_irq();
}

void osd_clear(void)
{
    // Fill with background color using 32-bit writes
    uint32_t bg32 = OSD_COLOR_BG | (OSD_COLOR_BG << 16);
    uint32_t *dst = (uint32_t *)osd_framebuffer;
    uint32_t count = (OSD_BOX_W * OSD_BOX_H) / 2;

    for (uint32_t i = 0; i < count; i++) {
        dst[i] = bg32;
    }
}

void osd_putchar_color(int x, int y, char c, uint16_t color)
{
    // Bounds check
    if (x < 0 || x + 8 > OSD_BOX_W || y < 0 || y + 8 > OSD_BOX_H)
        return;

    // Allow special glyphs at positions 1-2, clamp other control chars
    uint8_t idx = (uint8_t)c;
    if (idx != 1 && idx != 2 && (idx < 32 || idx > 126))
        idx = ' ';

    const uint8_t *glyph = font8x8[idx];

    // Render 8 rows
    for (int row = 0; row < 8; row++) {
        uint8_t line = glyph[row];
        uint16_t *dst_row = &osd_framebuffer[y + row][x];

        dst_row[0] = (line & 0x80) ? color : OSD_COLOR_BG;
        dst_row[1] = (line & 0x40) ? color : OSD_COLOR_BG;
        dst_row[2] = (line & 0x20) ? color : OSD_COLOR_BG;
        dst_row[3] = (line & 0x10) ? color : OSD_COLOR_BG;
        dst_row[4] = (line & 0x08) ? color : OSD_COLOR_BG;
        dst_row[5] = (line & 0x04) ? color : OSD_COLOR_BG;
        dst_row[6] = (line & 0x02) ? color : OSD_COLOR_BG;
        dst_row[7] = (line & 0x01) ? color : OSD_COLOR_BG;
    }
}

void osd_putchar(int x, int y, char c)
{
    osd_putchar_color(x, y, c, OSD_COLOR_FG);
}

void osd_puts_color(int x, int y, const char *str, uint16_t color)
{
    while (*str) {
        osd_putchar_color(x, y, *str++, color);
        x += 8;
        if (x + 8 > OSD_BOX_W)
            break;
    }
}

void osd_puts(int x, int y, const char *str)
{
    osd_puts_color(x, y, str, OSD_COLOR_FG);
}

// ============================================================================
// Button GPIO IRQ — fires on Core 0, just sets a flag
// ============================================================================

#define OSD_BTN_DEBOUNCE_MS 200

static void osd_gpio_irq_callback(uint gpio, uint32_t events)
{
    (void)events;
    if (gpio != PIN_OSD_BTN_MENU)
        return;

    static uint32_t last_press = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_press < OSD_BTN_DEBOUNCE_MS)
        return;
    last_press = now;

    osd_visible = !osd_visible;

    if (!osd_visible) {
        return;
    }

    // selftest_reset();
}

void osd_setup_button_irq(void)
{
    gpio_set_irq_enabled_with_callback(PIN_OSD_BTN_BACK, GPIO_IRQ_EDGE_FALL, true, osd_gpio_irq_callback);
}

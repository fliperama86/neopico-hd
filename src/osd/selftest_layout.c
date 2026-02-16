#include "selftest_layout.h"

#include <stdbool.h>

#include "fast_osd.h"

#define ST_TITLE_ROW 2
#define ST_TITLE_COL 2
#define ST_SPINNER_COL 25

#define ST_VIDEO_ROW 4
#define ST_VIDEO_LABEL_ROW 5
#define ST_BITS_HEADER_ROW 6
#define ST_RED_ROW 7
#define ST_GREEN_ROW 8
#define ST_BLUE_ROW 9

#define ST_AUDIO_ROW 11
#define ST_AUDIO_LABEL_ROW 12

static inline void selftest_render_icon(uint8_t row, uint8_t col, bool ok)
{
    fast_osd_putc_color(row, col, ok ? FAST_OSD_GLYPH_CHECK : FAST_OSD_GLYPH_CROSS,
                        ok ? OSD_COLOR_GREEN : OSD_COLOR_RED);
}

static inline void selftest_render_icon_neutral(uint8_t row, uint8_t col)
{
    fast_osd_putc_color(row, col, '-', OSD_COLOR_GRAY);
}

void selftest_layout_reset(void)
{
    fast_osd_clear();

    fast_osd_puts(ST_TITLE_ROW, ST_TITLE_COL, "NeoPico-HD Self Test");

    fast_osd_puts(ST_VIDEO_ROW, 1, "Video");
    fast_osd_puts_color(ST_VIDEO_LABEL_ROW, 1, "CSYNC", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_VIDEO_LABEL_ROW, 10, "PCLK", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_VIDEO_LABEL_ROW, 19, "Shadow", OSD_COLOR_GRAY);

    fast_osd_puts_color(ST_BITS_HEADER_ROW, 8, "0", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 10, "1", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 12, "2", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 14, "3", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 16, "4", OSD_COLOR_GRAY);

    fast_osd_puts_color(ST_RED_ROW, 1, "Red", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_GREEN_ROW, 1, "Green", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BLUE_ROW, 1, "Blue", OSD_COLOR_GRAY);

    fast_osd_puts(ST_AUDIO_ROW, 1, "Audio");
    fast_osd_puts_color(ST_AUDIO_LABEL_ROW, 1, "BCK", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_AUDIO_LABEL_ROW, 10, "WS", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_AUDIO_LABEL_ROW, 18, "DAT", OSD_COLOR_GRAY);

    selftest_render_icon_neutral(ST_VIDEO_LABEL_ROW, 7);
    selftest_render_icon_neutral(ST_VIDEO_LABEL_ROW, 15);
    selftest_render_icon_neutral(ST_VIDEO_LABEL_ROW, 26);

    for (uint8_t col = 8; col <= 16; col += 2) {
        selftest_render_icon_neutral(ST_RED_ROW, col);
        selftest_render_icon_neutral(ST_GREEN_ROW, col);
        selftest_render_icon_neutral(ST_BLUE_ROW, col);
    }

    selftest_render_icon_neutral(ST_AUDIO_LABEL_ROW, 5);
    selftest_render_icon_neutral(ST_AUDIO_LABEL_ROW, 14);
    selftest_render_icon_neutral(ST_AUDIO_LABEL_ROW, 22);
}

void selftest_layout_update(uint32_t frame_count, bool has_snapshot, uint32_t toggled_bits)
{
    static uint8_t dat_hold = 0;
    static const uint32_t red_bits[5] = {SELFTEST_BIT_R0, SELFTEST_BIT_R1, SELFTEST_BIT_R2, SELFTEST_BIT_R3,
                                         SELFTEST_BIT_R4};
    static const uint32_t green_bits[5] = {SELFTEST_BIT_G0, SELFTEST_BIT_G1, SELFTEST_BIT_G2, SELFTEST_BIT_G3,
                                           SELFTEST_BIT_G4};
    static const uint32_t blue_bits[5] = {SELFTEST_BIT_B0, SELFTEST_BIT_B1, SELFTEST_BIT_B2, SELFTEST_BIT_B3,
                                          SELFTEST_BIT_B4};
    static const char spinner[4] = {'|', '/', '-', '\\'};
    const char spin = spinner[(frame_count / 60U) & 3U];
    fast_osd_putc_color(ST_TITLE_ROW, ST_SPINNER_COL, spin, OSD_COLOR_YELLOW);

    if (!has_snapshot) {
        return;
    }

    selftest_render_icon(ST_VIDEO_LABEL_ROW, 7, (toggled_bits & SELFTEST_BIT_CSYNC) != 0U);
    selftest_render_icon(ST_VIDEO_LABEL_ROW, 15, (toggled_bits & SELFTEST_BIT_PCLK) != 0U);
    selftest_render_icon(ST_VIDEO_LABEL_ROW, 26, (toggled_bits & SELFTEST_BIT_SHADOW) != 0U);

    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t col = (uint8_t)(8U + (i * 2U));
        selftest_render_icon(ST_RED_ROW, col, (toggled_bits & red_bits[i]) != 0U);
        selftest_render_icon(ST_GREEN_ROW, col, (toggled_bits & green_bits[i]) != 0U);
        selftest_render_icon(ST_BLUE_ROW, col, (toggled_bits & blue_bits[i]) != 0U);
    }

    selftest_render_icon(ST_AUDIO_LABEL_ROW, 5, (toggled_bits & SELFTEST_BIT_BCK) != 0U);
    selftest_render_icon(ST_AUDIO_LABEL_ROW, 14, (toggled_bits & SELFTEST_BIT_WS) != 0U);

    if ((toggled_bits & SELFTEST_BIT_DAT) != 0U) {
        dat_hold = 5;
    } else if (dat_hold > 0U) {
        dat_hold--;
    }
    selftest_render_icon(ST_AUDIO_LABEL_ROW, 22, dat_hold > 0U);
}

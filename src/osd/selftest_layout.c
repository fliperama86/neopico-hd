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

void selftest_layout_reset(void)
{
    fast_osd_clear();

    fast_osd_puts(ST_TITLE_ROW, ST_TITLE_COL, "NeoPico-HD Self Test");

    fast_osd_puts(ST_VIDEO_ROW, 1, "Video");
    fast_osd_puts_color(ST_VIDEO_LABEL_ROW, 1, "CSYNC", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_VIDEO_LABEL_ROW, 10, "PCLK", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_VIDEO_LABEL_ROW, 19, "Shadow", OSD_COLOR_GRAY);

    fast_osd_puts_color(ST_BITS_HEADER_ROW, 8, "4", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 10, "3", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 12, "2", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 14, "1", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BITS_HEADER_ROW, 16, "0", OSD_COLOR_GRAY);

    fast_osd_puts_color(ST_RED_ROW, 1, "Red", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_GREEN_ROW, 1, "Green", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_BLUE_ROW, 1, "Blue", OSD_COLOR_GRAY);

    fast_osd_puts(ST_AUDIO_ROW, 1, "Audio");
    fast_osd_puts_color(ST_AUDIO_LABEL_ROW, 1, "BCK", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_AUDIO_LABEL_ROW, 10, "WS", OSD_COLOR_GRAY);
    fast_osd_puts_color(ST_AUDIO_LABEL_ROW, 18, "DAT", OSD_COLOR_GRAY);

    selftest_render_icon(ST_VIDEO_LABEL_ROW, 7, false);
    selftest_render_icon(ST_VIDEO_LABEL_ROW, 15, false);
    selftest_render_icon(ST_VIDEO_LABEL_ROW, 26, false);

    for (uint8_t col = 8; col <= 16; col += 2) {
        selftest_render_icon(ST_RED_ROW, col, false);
        selftest_render_icon(ST_GREEN_ROW, col, false);
        selftest_render_icon(ST_BLUE_ROW, col, false);
    }

    selftest_render_icon(ST_AUDIO_LABEL_ROW, 5, false);
    selftest_render_icon(ST_AUDIO_LABEL_ROW, 14, false);
    selftest_render_icon(ST_AUDIO_LABEL_ROW, 22, false);
}

void selftest_layout_update(uint32_t frame_count)
{
    static const char spinner[4] = {'|', '/', '-', '\\'};
    const char spin = spinner[(frame_count / 60U) & 3U];
    fast_osd_putc_color(ST_TITLE_ROW, ST_SPINNER_COL, spin, OSD_COLOR_YELLOW);
}

#include "video_pipeline.h"

#include "pico_hdmi/video_output.h"

#include <string.h>

#include "line_ring.h"
#include "osd/fast_osd.h"
#include "pico.h"
#include "video_config.h"

// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;
static bool osd_visible_latched = false;

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height)
{
    video_output_init(frame_width, frame_height);
    video_output_set_scanline_callback(video_pipeline_scanline_callback);
    video_output_set_vsync_callback(video_pipeline_vsync_callback);

    osd_visible_latched = osd_visible;
}

/**
 * Fast 2x pixel doubling: reads 2 pixels, writes 2 doubled words.
 * Processes 32-bits at a time for efficiency.
 */
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *restrict dst, const uint16_t *restrict src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    uint32_t *d = dst;
    int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        uint32_t pair = src32[i];
        uint32_t p0 = pair & 0xFFFF;
        uint32_t p1 = pair >> 16;
        d[0] = p0 | (p0 << 16);
        d[1] = p1 | (p1 << 16);
        d += 2;
    }
}

/**
 * Fast 4x pixel quadrupling: reads 2 pixels, writes 4 doubled words (8 output pixels).
 * For 240p direct mode (1280 output pixels from 320 source pixels).
 */
void __scratch_y("") video_pipeline_quadruple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        uint32_t d0 = p0 | (p0 << 16);
        uint32_t d1 = p1 | (p1 << 16);
        dst[i * 4] = d0;
        dst[(i * 4) + 1] = d0;
        dst[(i * 4) + 2] = d1;
        dst[(i * 4) + 3] = d1;
    }
}

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 */
void __scratch_x("") video_pipeline_vsync_callback(void)
{
    line_ring_output_vsync();
    osd_visible_latched = osd_visible;
}

void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    const uint32_t h_words = MODE_H_ACTIVE_PIXELS / 2;
    const uint32_t fb_line = active_line >> 1;
    const uint32_t mvs_line_u32 = fb_line - V_OFFSET;

    // Single unsigned range check for active 224-line window.
    if (mvs_line_u32 >= MVS_HEIGHT) {
        memset(dst, 0, h_words * sizeof(uint32_t));
        return;
    }

    const uint16_t mvs_line = (uint16_t)mvs_line_u32;

    // Get source line (NULL if not ready)
    const uint16_t *src = NULL;
    if (line_ring_ready(mvs_line)) {
        src = line_ring_read_ptr(mvs_line);
    }

    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    const bool osd_line_active = osd_visible_latched && (osd_line_u32 < OSD_BOX_H);

    if (!osd_line_active && src) {
        video_pipeline_double_pixels_fast(dst, src, LINE_WIDTH);
        return;
    }

    const uint16_t *osd_src = osd_framebuffer[osd_line_u32];

    if (src) {
        // Before OSD
        video_pipeline_double_pixels_fast(dst, src, OSD_BOX_X);
        // OSD region (blit from OSD framebuffer)
        video_pipeline_double_pixels_fast(dst + OSD_BOX_X, osd_src, OSD_BOX_W);
        // After OSD
        video_pipeline_double_pixels_fast(dst + OSD_BOX_X + OSD_BOX_W, src + OSD_BOX_X + OSD_BOX_W,
                                          LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
        return;
    }
}

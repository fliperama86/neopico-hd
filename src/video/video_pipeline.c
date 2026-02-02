#include "video_pipeline.h"

#include <string.h>

#include "pico.h"

#include "pico_hdmi/video_output.h"

#include "line_ring.h"
#include "osd/osd.h"
#include "video_config.h"


// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height)
{
    video_output_init(frame_width, frame_height);
    video_output_set_scanline_callback(video_pipeline_scanline_callback);
    video_output_set_vsync_callback(video_pipeline_vsync_callback);
}

/**
 * Fast 2x pixel doubling: reads 2 pixels, writes 2 doubled words.
 * Processes 32-bits at a time for efficiency.
 */
void __scratch_x("") video_pipeline_double_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        dst[i * 2] = p0 | (p0 << 16);
        dst[(i * 2) + 1] = p1 | (p1 << 16);
    }
}

/**
 * Fast 2x pixel doubling with 50% darkening for scanlines.
 * Uses bit-masking to prevent color bleed across channels.
 * RGB565: RRRRR GGGGGG BBBBB
 * 50% Darken: (pixel & 0xF7DE) >> 1
 */
void __scratch_x("") video_pipeline_double_pixels_scanline(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];

        // Extract and darken first pixel
        uint32_t p0 = (two & 0xFFFF);
        p0 = (p0 & 0xF7DE) >> 1;

        // Extract and darken second pixel
        uint32_t p1 = (two >> 16);
        p1 = (p1 & 0xF7DE) >> 1;

        // Double each pixel and store
        dst[i * 2] = p0 | (p0 << 16);
        dst[(i * 2) + 1] = p1 | (p1 << 16);
    }
}

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 */
void __scratch_x("") video_pipeline_vsync_callback(void)
{
    line_ring_output_vsync();
}

/**
 * Scanline callback - reads from line ring buffer.
 * Called by Core 1 DMA ISR for every active video line.
 * Performs 2x vertical scaling (line doubling) from 240p to 480p.
 */
void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    uint32_t fb_line = active_line / 2;

    // Bounds check
    if (fb_line >= FRAME_HEIGHT) {
        uint32_t blue = 0x00000000;
        for (int i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) dst[i] = blue;
        return;
    }

    // Vertical centering
    if (fb_line < V_OFFSET || fb_line >= V_OFFSET + MVS_HEIGHT) {
        uint32_t blue = 0x00000000;
        for (int i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) dst[i] = blue;
        return;
    }

    uint16_t mvs_line = fb_line - V_OFFSET;

    // Get source line (NULL if not ready)
    const uint16_t *src = line_ring_ready(mvs_line) ? line_ring_read_ptr(mvs_line) : NULL;

    // Check if OSD visible on this line
    bool osd_on_line = osd_visible && fb_line >= OSD_BOX_Y && fb_line < OSD_BOX_Y + OSD_BOX_H;

    if (osd_on_line) {
        uint32_t osd_y = fb_line - OSD_BOX_Y;
        const uint16_t *osd_src = osd_framebuffer[osd_y];

        // Region 1: Before OSD
        if (src) {
            video_pipeline_double_pixels_fast(dst, src, OSD_BOX_X);
        } else {
            uint32_t blue = 0x00000000;
            for (int i = 0; i < OSD_BOX_X; i++) dst[i] = blue;
        }

        // Region 2: OSD box
        video_pipeline_double_pixels_fast(dst + OSD_BOX_X, osd_src, OSD_BOX_W);

        // Region 3: After OSD
        if (src) {
            video_pipeline_double_pixels_fast(dst + OSD_BOX_X + OSD_BOX_W,
                src + OSD_BOX_X + OSD_BOX_W, LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
        } else {
            uint32_t blue = 0x00000000;
            for (int i = OSD_BOX_X + OSD_BOX_W; i < LINE_WIDTH; i++) dst[i] = blue;
        }
    } else {
        if (src) {
            video_pipeline_double_pixels_fast(dst, src, LINE_WIDTH);
        } else {
            uint32_t blue = 0x00000000;
            for (int i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) dst[i] = blue;
        }
    }
}

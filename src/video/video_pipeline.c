#include "video_pipeline.h"

#include "pico_hdmi/video_output_rt.h"

#include <string.h>

#include "line_ring.h"
#include "osd/osd.h"
#include "pico.h"
#include "video_config.h"

// H_WORDS computed at runtime from active mode

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
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
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
void video_pipeline_double_pixels_scanline(uint32_t *dst, const uint16_t *src, int count)
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
}

/**
 * Scanline callback - reads from line ring buffer.
 * Called by Core 1 DMA ISR for every active video line.
 * Mode-aware: 480p uses 2x doubling, 240p uses 4x quadrupling.
 */
void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    bool is_240p = (video_output_active_mode == &video_mode_240_p);
    uint32_t h_words = video_output_active_mode->h_active_pixels / 2;

    // 240p: 1:1 line mapping, 480p: 2 output lines per source line
    uint32_t fb_line = is_240p ? active_line : active_line / 2;

    // Bounds check
    if (fb_line >= FRAME_HEIGHT) {
        for (uint32_t i = 0; i < h_words; i++)
            dst[i] = 0;
        return;
    }

    // Vertical centering
    if (fb_line < V_OFFSET || fb_line >= V_OFFSET + MVS_HEIGHT) {
        for (uint32_t i = 0; i < h_words; i++)
            dst[i] = 0;
        return;
    }

    uint16_t mvs_line = fb_line - V_OFFSET;

    // Get source line (NULL if not ready)
    const uint16_t *src = line_ring_ready(mvs_line) ? line_ring_read_ptr(mvs_line) : NULL;

    // Check if OSD visible on this line
    bool osd_on_line = osd_visible && fb_line >= OSD_BOX_Y && fb_line < OSD_BOX_Y + OSD_BOX_H;

    if (is_240p) {
        // 240p: 4x expansion (320 src -> 1280 output = 640 uint32_t words)
        // Each source pixel produces 2 dst words (4 output pixels)
        if (osd_on_line) {
            uint32_t osd_y = fb_line - OSD_BOX_Y;
            const uint16_t *osd_src = osd_framebuffer[osd_y];

            // Region 1: Before OSD (OSD_BOX_X source pixels -> OSD_BOX_X*2 dst words)
            if (src) {
                video_pipeline_quadruple_pixels_fast(dst, src, OSD_BOX_X);
            } else {
                for (uint32_t i = 0; i < OSD_BOX_X * 2; i++)
                    dst[i] = 0;
            }

            // Region 2: OSD box (OSD_BOX_W source pixels -> OSD_BOX_W*2 dst words)
            video_pipeline_quadruple_pixels_fast(dst + (OSD_BOX_X * 2), osd_src, OSD_BOX_W);

            // Region 3: After OSD
            uint32_t after_src_start = OSD_BOX_X + OSD_BOX_W;
            uint32_t after_dst_offset = after_src_start * 2;
            uint32_t after_count = LINE_WIDTH - after_src_start;
            if (src) {
                video_pipeline_quadruple_pixels_fast(dst + after_dst_offset, src + after_src_start, after_count);
            } else {
                for (uint32_t i = after_dst_offset; i < h_words; i++)
                    dst[i] = 0;
            }
        } else {
            if (src) {
                video_pipeline_quadruple_pixels_fast(dst, src, LINE_WIDTH);
            } else {
                for (uint32_t i = 0; i < h_words; i++)
                    dst[i] = 0;
            }
        }
    } else {
        // 480p: 2x expansion (320 src -> 640 output = 320 uint32_t words)
        // Each source pixel produces 1 dst word (2 output pixels)
        if (osd_on_line) {
            uint32_t osd_y = fb_line - OSD_BOX_Y;
            const uint16_t *osd_src = osd_framebuffer[osd_y];

            // Region 1: Before OSD
            if (src) {
                video_pipeline_double_pixels_fast(dst, src, OSD_BOX_X);
            } else {
                for (uint32_t i = 0; i < (uint32_t)OSD_BOX_X; i++)
                    dst[i] = 0;
            }

            // Region 2: OSD box
            video_pipeline_double_pixels_fast(dst + OSD_BOX_X, osd_src, OSD_BOX_W);

            // Region 3: After OSD
            if (src) {
                video_pipeline_double_pixels_fast(dst + OSD_BOX_X + OSD_BOX_W, src + OSD_BOX_X + OSD_BOX_W,
                                                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
            } else {
                for (uint32_t i = OSD_BOX_X + OSD_BOX_W; i < h_words; i++)
                    dst[i] = 0;
            }
        } else {
            if (src) {
                video_pipeline_double_pixels_fast(dst, src, LINE_WIDTH);
            } else {
                for (uint32_t i = 0; i < h_words; i++)
                    dst[i] = 0;
            }
        }
    }
}

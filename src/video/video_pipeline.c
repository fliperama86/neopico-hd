#include "video_pipeline.h"

#include "pico_hdmi/video_output.h"

#include "line_ring.h"
#include "pico.h"
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
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t pair = src32[i];
        uint32_t p0 = pair & 0xFFFF;
        uint32_t p1 = pair >> 16;
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

void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    uint32_t h_words = MODE_H_ACTIVE_PIXELS / 2;
    uint32_t fb_line = active_line / 2;

    // // Bounds check
    // if (fb_line >= FRAME_HEIGHT) {
    //     for (uint32_t i = 0; i < h_words; i++)
    //         dst[i] = 0;
    //     return;
    // }

    // // Vertical centering
    // if (fb_line < V_OFFSET || fb_line >= V_OFFSET + MVS_HEIGHT) {
    //     for (uint32_t i = 0; i < h_words; i++)
    //         dst[i] = 0;
    //     return;
    // }

    uint16_t mvs_line = fb_line - V_OFFSET;

    // Get source line (NULL if not ready)
    const uint16_t *src = line_ring_ready(mvs_line) ? line_ring_read_ptr(mvs_line) : NULL;

    if (src) {
        video_pipeline_double_pixels_fast(dst, src, LINE_WIDTH);
    } else {
        for (uint32_t i = 0; i < h_words; i++)
            dst[i] = 0;
    }
}

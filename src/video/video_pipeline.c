#include "video_pipeline.h"

#include "pico_hdmi/video_output_rt.h"

#include <string.h>

#include "line_ring.h"
#if NEOPICO_ENABLE_OSD
#include "osd/fast_osd.h"
#endif
#include "pico.h"
#include "video_config.h"
#if NEOPICO_EXP_GENLOCK_DYNAMIC
#include "pico_hdmi/video_output_rt.h"

#include "hardware/timer.h"

#include "video_capture.h"
#endif

// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;
#if NEOPICO_ENABLE_OSD
static bool osd_visible_latched = false;
#endif
typedef void (*pixel_scale_fn_t)(uint32_t *dst, const uint16_t *src, int count);

static bool mode_is_240p = false;
static bool mode_is_720p = false;
static uint32_t mode_h_words = 320;
static uint32_t mode_image_words = 320;
static uint32_t mode_x_margin_words = 0;
static uint32_t mode_osd_x_words = 0;
static uint32_t mode_osd_w_words = 0;
static pixel_scale_fn_t mode_scale_fn = video_pipeline_double_pixels_fast;
// Overscan/background outside active 224-line image area (RGB565): black.
#define OVERSCAN_COLOR_RGB565 0x0000
// No-signal fallback color (RGB565): mid gray.
#define NO_SIGNAL_COLOR_RGB565 0x7BEF

static inline void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
{
    const uint32_t packed = ((uint32_t)color << 16) | color;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = packed;
    }
}

static inline void video_pipeline_refresh_mode_cache(void)
{
    const uint16_t h_active = video_output_get_h_active_pixels();
    const uint16_t v_active = video_output_get_v_active_lines();

    mode_is_240p = (h_active == 1280U && v_active == 240U);
    mode_is_720p = (h_active == 1280U && v_active == 720U);
    const uint32_t h_scale = mode_is_720p ? 3U : (h_active == (LINE_WIDTH * 4U)) ? 4U : 2U;

    mode_h_words = (uint32_t)h_active / 2U;
    mode_image_words = (LINE_WIDTH * h_scale) / 2U;
    mode_x_margin_words = (mode_h_words > mode_image_words) ? ((mode_h_words - mode_image_words) / 2U) : 0U;
    mode_scale_fn = (h_scale == 4U)   ? video_pipeline_quadruple_pixels_fast
                    : (h_scale == 3U) ? video_pipeline_triple_pixels_fast
                                      : video_pipeline_double_pixels_fast;
#if NEOPICO_ENABLE_OSD
    mode_osd_x_words = mode_x_margin_words + (((uint32_t)OSD_BOX_X * h_scale) / 2U);
    mode_osd_w_words = ((uint32_t)OSD_BOX_W * h_scale) / 2U;
#else
    mode_osd_x_words = 0;
    mode_osd_w_words = 0;
#endif
}

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height)
{
    video_output_init(frame_width, frame_height);
    video_output_set_scanline_callback(video_pipeline_scanline_callback);
    video_output_set_vsync_callback(video_pipeline_vsync_callback);
    video_pipeline_refresh_mode_cache();

#if NEOPICO_ENABLE_OSD
    osd_visible_latched = osd_visible;
#endif
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
 * Fast 3x pixel scaling: reads 2 pixels, writes 3 doubled words (6 output pixels).
 * For 720p 4:3 mode (960 output pixels from 320 source pixels, centered).
 */
void __scratch_y("") video_pipeline_triple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count >> 1;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        dst[(i * 3) + 0] = p0 | (p0 << 16);
        dst[(i * 3) + 1] = p0 | (p1 << 16);
        dst[(i * 3) + 2] = p1 | (p1 << 16);
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

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Nominals that approximate MVS ~59.18 Hz at 25.2 MHz pixel clock:
//   480p: 25.2M / (800 * 532) = 59.21 Hz   (±1 → 59.10–59.32 Hz)
//   240p: 25.2M / (1600 * 266) = 59.21 Hz  (±1 → 58.99–59.43 Hz)
#define GENLOCK_NOMINAL_VTOTAL_480 532
#define GENLOCK_NOMINAL_VTOTAL_240 266
#define GENLOCK_PHASE_THRESHOLD_US 200
#define GENLOCK_PHASE_MAX_US 5000

static uint32_t genlock_last_phase = 0;
static bool genlock_phase_valid = false;

static void __scratch_x("") genlock_dynamic_update(void)
{
    uint32_t hdmi_ts = timer_hw->timerawl;
    uint32_t mvs_ts = g_mvs_vsync_timestamp;
    uint32_t phase = hdmi_ts - mvs_ts;

    uint16_t nominal =
        (video_output_active_mode->v_total_lines <= 266) ? GENLOCK_NOMINAL_VTOTAL_240 : GENLOCK_NOMINAL_VTOTAL_480;

    if (!genlock_phase_valid) {
        genlock_last_phase = phase;
        genlock_phase_valid = true;
        rt_v_total_lines = nominal;
        return;
    }

    int32_t delta = (int32_t)(phase - genlock_last_phase);
    genlock_last_phase = phase;

    // Ignore outliers (missed VSYNC, signal loss, etc.)
    if (delta > GENLOCK_PHASE_MAX_US || delta < -GENLOCK_PHASE_MAX_US) {
        rt_v_total_lines = nominal;
        return;
    }

    if (delta < -GENLOCK_PHASE_THRESHOLD_US) {
        rt_v_total_lines = nominal + 1; // HDMI faster than MVS, slow down
    } else if (delta > GENLOCK_PHASE_THRESHOLD_US) {
        rt_v_total_lines = nominal - 1; // HDMI slower than MVS, speed up
    } else {
        rt_v_total_lines = nominal; // in lock
    }
}
#endif

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 */
void __scratch_x("") video_pipeline_vsync_callback(void)
{
    video_pipeline_refresh_mode_cache();
    line_ring_output_vsync();
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    genlock_dynamic_update();
#endif
#if NEOPICO_ENABLE_OSD
    osd_visible_latched = osd_visible;
#endif
}

void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    const uint32_t h_words = mode_h_words;
    uint32_t fb_line;
    if (mode_is_720p) {
        if ((active_line % 3U) != 0U) {
            return;
        }
        fb_line = active_line / 3U;
    } else {
        fb_line = mode_is_240p ? active_line : (active_line >> 1);
    }

#if NEOPICO_ENABLE_OSD
    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    const bool osd_line_active = osd_visible_latched && (osd_line_u32 < OSD_BOX_H);
#else
    const bool osd_line_active = false;
#endif

    if (!osd_line_active) {
        const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
        // Single unsigned range check for active 224-line window.
        if (mvs_line_u32 >= MVS_HEIGHT) {
            video_pipeline_fill_rgb565(dst, h_words, OVERSCAN_COLOR_RGB565);
            return;
        }

        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        const uint16_t *src = NULL;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
        }
        if (!src) {
            video_pipeline_fill_rgb565(dst, h_words, NO_SIGNAL_COLOR_RGB565);
            return;
        }
        video_pipeline_fill_rgb565(dst, mode_x_margin_words, OVERSCAN_COLOR_RGB565);
        mode_scale_fn(dst + mode_x_margin_words, src, LINE_WIDTH);
        video_pipeline_fill_rgb565(dst + mode_x_margin_words + mode_image_words,
                                   h_words - mode_x_margin_words - mode_image_words, OVERSCAN_COLOR_RGB565);
        return;
    }

#if NEOPICO_ENABLE_OSD
    // OSD-active path: draw OSD even if capture source is unavailable.
    const uint32_t mvs_line_u32 = fb_line - V_OFFSET;
    const uint16_t *src = NULL;
    if (mvs_line_u32 < MVS_HEIGHT) {
        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
        }
    }

    const uint16_t *osd_src = osd_framebuffer[osd_line_u32];
    if (!src) {
        // No capture source: render OSD over fallback color without double-writing the OSD span.
        video_pipeline_fill_rgb565(dst, mode_osd_x_words, NO_SIGNAL_COLOR_RGB565);
        mode_scale_fn(dst + mode_osd_x_words, osd_src, OSD_BOX_W);
        video_pipeline_fill_rgb565(dst + mode_osd_x_words + mode_osd_w_words,
                                   h_words - mode_osd_x_words - mode_osd_w_words, NO_SIGNAL_COLOR_RGB565);
        return;
    }

    // Before OSD
    video_pipeline_fill_rgb565(dst, mode_x_margin_words, OVERSCAN_COLOR_RGB565);
    mode_scale_fn(dst + mode_x_margin_words, src, OSD_BOX_X);
    // OSD region (blit from OSD framebuffer)
    mode_scale_fn(dst + mode_osd_x_words, osd_src, OSD_BOX_W);
    // After OSD
    mode_scale_fn(dst + mode_osd_x_words + mode_osd_w_words, src + OSD_BOX_X + OSD_BOX_W,
                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    video_pipeline_fill_rgb565(dst + mode_x_margin_words + mode_image_words,
                               h_words - mode_x_margin_words - mode_image_words, OVERSCAN_COLOR_RGB565);
#endif
}

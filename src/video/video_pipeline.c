#include "video_pipeline.h"

#if NEOPICO_USE_NONRT_HDMI
#include "pico_hdmi/video_output.h"
#else
#include "pico_hdmi/video_output_rt.h"
#endif

#include <string.h>

#include "line_ring.h"
#if NEOPICO_ENABLE_OSD
#include "osd/fast_osd.h"
#endif
#if NEOPICO_EXP_REBOOT_MODE_SWITCH
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#endif
#include "pico.h"
#include "video_config.h"
#if NEOPICO_EXP_GENLOCK_DYNAMIC && !NEOPICO_USE_NONRT_HDMI
#include "pico_hdmi/video_output_rt.h"

#include "hardware/timer.h"

#include "video_capture.h"
#endif

#ifndef NEOPICO_VIDEO_TEST_PATTERN
#define NEOPICO_VIDEO_TEST_PATTERN 0
#endif

#ifndef NEOPICO_VIDEO_240P
#define NEOPICO_VIDEO_240P 0
#endif

#ifndef NEOPICO_VIDEO_720P
#define NEOPICO_VIDEO_720P 0
#endif

#ifndef NEOPICO_EXP_REBOOT_MODE_SWITCH
#define NEOPICO_EXP_REBOOT_MODE_SWITCH 0
#endif

#if NEOPICO_EXP_REBOOT_MODE_SWITCH && (NEOPICO_USE_NONRT_HDMI || NEOPICO_VIDEO_720P)
#error "NEOPICO_EXP_REBOOT_MODE_SWITCH is currently limited to rt 480p/240p builds"
#endif

// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;
#if NEOPICO_ENABLE_OSD
static bool osd_visible_latched = false;
#endif
typedef void (*pixel_scale_fn_t)(uint32_t *dst, const uint16_t *src, int count);
// Overscan/background outside active 224-line image area (RGB565): black.
#define OVERSCAN_COLOR_RGB565 0x0000
// No-signal fallback color (RGB565): mid gray.
#define NO_SIGNAL_COLOR_RGB565 0x7BEF

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
static bool reboot_requested_240p = (NEOPICO_VIDEO_240P != 0);
#define REBOOT_MODE_BOOT_MAGIC 0x4e505253U
#define REBOOT_MODE_BOOT_CHECK_XOR 0xa5a55a5aU

static inline uint32_t reboot_mode_boot_check(uint32_t enabled)
{
    return REBOOT_MODE_BOOT_MAGIC ^ enabled ^ REBOOT_MODE_BOOT_CHECK_XOR;
}
#elif NEOPICO_VIDEO_720P
#define VIDEO_PIPELINE_H_WORDS (1280U / 2U)
#define VIDEO_PIPELINE_H_SCALE 3U
#define VIDEO_PIPELINE_SCALE_PIXELS(dst, src, count) video_pipeline_triple_pixels_fast((dst), (src), (count))
static inline bool video_pipeline_map_active_line(uint32_t active_line, uint32_t *fb_line)
{
    if ((active_line % 3U) != 0U) {
        return false;
    }
    *fb_line = active_line / 3U;
    return true;
}
#elif NEOPICO_VIDEO_240P
#define VIDEO_PIPELINE_H_WORDS (1280U / 2U)
#define VIDEO_PIPELINE_H_SCALE 4U
#define VIDEO_PIPELINE_SCALE_PIXELS(dst, src, count) video_pipeline_quadruple_pixels_fast((dst), (src), (count))
static inline bool video_pipeline_map_active_line(uint32_t active_line, uint32_t *fb_line)
{
    *fb_line = active_line;
    return true;
}
#else
#define VIDEO_PIPELINE_H_WORDS (640U / 2U)
#define VIDEO_PIPELINE_H_SCALE 2U
#define VIDEO_PIPELINE_SCALE_PIXELS(dst, src, count) video_pipeline_double_pixels_fast((dst), (src), (count))
static inline bool video_pipeline_map_active_line(uint32_t active_line, uint32_t *fb_line)
{
    *fb_line = active_line >> 1;
    return true;
}
#endif

#define VIDEO_PIPELINE_IMAGE_WORDS ((LINE_WIDTH * VIDEO_PIPELINE_H_SCALE) / 2U)
#define VIDEO_PIPELINE_X_MARGIN_WORDS                                                                                  \
    ((VIDEO_PIPELINE_H_WORDS > VIDEO_PIPELINE_IMAGE_WORDS)                                                             \
         ? ((VIDEO_PIPELINE_H_WORDS - VIDEO_PIPELINE_IMAGE_WORDS) / 2U)                                                \
         : 0U)
#if NEOPICO_ENABLE_OSD
#define VIDEO_PIPELINE_OSD_X_WORDS                                                                                     \
    (VIDEO_PIPELINE_X_MARGIN_WORDS + (((uint32_t)OSD_BOX_X * VIDEO_PIPELINE_H_SCALE) / 2U))
#define VIDEO_PIPELINE_OSD_W_WORDS (((uint32_t)OSD_BOX_W * VIDEO_PIPELINE_H_SCALE) / 2U)
#endif

static void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
    __attribute__((noinline, noclone));

static void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
{
    const uint32_t packed = ((uint32_t)color << 16) | color;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = packed;
    }
}

#if NEOPICO_VIDEO_TEST_PATTERN
static uint16_t test_pattern_line[LINE_WIDTH] __attribute__((aligned(4)));
static bool test_pattern_line_ready = false;

static void video_pipeline_init_test_pattern_line(void)
{
    static const uint16_t colors[] = {
        0x0000, // black
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFE0, // yellow
        0xF81F, // magenta
        0x07FF, // cyan
        0xFFFF, // white
    };
    const uint32_t color_count = (uint32_t)(sizeof(colors) / sizeof(colors[0]));
    for (uint32_t x = 0; x < LINE_WIDTH; x++) {
        test_pattern_line[x] = colors[(x * color_count) / LINE_WIDTH];
    }
    test_pattern_line_ready = true;
}
#endif

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height)
{
    video_output_init(frame_width, frame_height);
    video_output_set_scanline_callback(video_pipeline_scanline_callback);
    video_output_set_vsync_callback(video_pipeline_vsync_callback);
#if NEOPICO_EXP_REBOOT_MODE_SWITCH
    reboot_requested_240p = (video_output_active_mode->v_active_lines == 240U);
#endif

#if NEOPICO_ENABLE_OSD
    osd_visible_latched = osd_visible;
#endif
}

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
void video_pipeline_request_reboot_240p(bool enabled)
{
    reboot_requested_240p = enabled;
    const uint32_t mode = enabled ? 1U : 0U;
    watchdog_hw->scratch[0] = REBOOT_MODE_BOOT_MAGIC;
    watchdog_hw->scratch[1] = mode;
    watchdog_hw->scratch[2] = reboot_mode_boot_check(mode);
    __dmb();
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

bool video_pipeline_reboot_requested_240p(void)
{
    return reboot_requested_240p;
}

bool video_pipeline_take_reboot_240p_boot_request(bool *enabled)
{
    const uint32_t magic = watchdog_hw->scratch[0];
    const uint32_t mode = watchdog_hw->scratch[1];
    const uint32_t check = watchdog_hw->scratch[2];
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;

    if ((magic != REBOOT_MODE_BOOT_MAGIC) || (mode > 1U) || (check != reboot_mode_boot_check(mode))) {
        return false;
    }

    if (enabled) {
        *enabled = (mode != 0U);
    }
    return true;
}
#endif

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

#if NEOPICO_EXP_GENLOCK_DYNAMIC && !NEOPICO_USE_NONRT_HDMI
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
    line_ring_output_vsync();
#if NEOPICO_EXP_GENLOCK_DYNAMIC && !NEOPICO_USE_NONRT_HDMI
    genlock_dynamic_update();
#endif
#if NEOPICO_ENABLE_OSD
    osd_visible_latched = osd_visible;
#endif
}

void __scratch_x("") video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
    const bool mode_is_240p = (video_output_active_mode->v_active_lines == 240U);
    const uint32_t h_words = mode_is_240p ? (1280U / 2U) : (640U / 2U);
    const uint32_t image_words = h_words;
    const uint32_t x_margin_words = 0;
    const pixel_scale_fn_t scale_pixels =
        mode_is_240p ? video_pipeline_quadruple_pixels_fast : video_pipeline_double_pixels_fast;
    const uint32_t fb_line = mode_is_240p ? active_line : (active_line >> 1);
#if NEOPICO_ENABLE_OSD
    const uint32_t osd_x_words = mode_is_240p ? ((uint32_t)OSD_BOX_X * 2U) : (uint32_t)OSD_BOX_X;
    const uint32_t osd_w_words = mode_is_240p ? ((uint32_t)OSD_BOX_W * 2U) : (uint32_t)OSD_BOX_W;
#endif
#define VIDEO_PIPELINE_SCALE_SELECTED(dst_arg, src_arg, count_arg) scale_pixels((dst_arg), (src_arg), (count_arg))
#else
    const uint32_t h_words = VIDEO_PIPELINE_H_WORDS;
    const uint32_t image_words = VIDEO_PIPELINE_IMAGE_WORDS;
    const uint32_t x_margin_words = VIDEO_PIPELINE_X_MARGIN_WORDS;
#if NEOPICO_ENABLE_OSD
    const uint32_t osd_x_words = VIDEO_PIPELINE_OSD_X_WORDS;
    const uint32_t osd_w_words = VIDEO_PIPELINE_OSD_W_WORDS;
#endif
#define VIDEO_PIPELINE_SCALE_SELECTED(dst_arg, src_arg, count_arg)                                                     \
    VIDEO_PIPELINE_SCALE_PIXELS((dst_arg), (src_arg), (count_arg))

#if NEOPICO_VIDEO_TEST_PATTERN && NEOPICO_VIDEO_720P
    if (!test_pattern_line_ready) {
        video_pipeline_init_test_pattern_line();
    }
    if ((active_line % 3U) != 0U) {
        return;
    }
    video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
    VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, test_pattern_line, LINE_WIDTH);
    video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                               OVERSCAN_COLOR_RGB565);
    return;
#endif

    uint32_t fb_line;
    if (!video_pipeline_map_active_line(active_line, &fb_line)) {
        return;
    }
#endif

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
        video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, LINE_WIDTH);
        video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                                   OVERSCAN_COLOR_RGB565);
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
        video_pipeline_fill_rgb565(dst, osd_x_words, NO_SIGNAL_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
        video_pipeline_fill_rgb565(dst + osd_x_words + osd_w_words, h_words - osd_x_words - osd_w_words,
                                   NO_SIGNAL_COLOR_RGB565);
        return;
    }

    // Before OSD
    video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
    VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, OSD_BOX_X);
    // OSD region (blit from OSD framebuffer)
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
    // After OSD
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words + osd_w_words, src + OSD_BOX_X + OSD_BOX_W,
                                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                               OVERSCAN_COLOR_RGB565);
#endif
#undef VIDEO_PIPELINE_SCALE_SELECTED
}

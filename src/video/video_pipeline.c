#include "video_pipeline.h"

#include "pico_hdmi/video_output_rt.h"

#include "hardware/irq.h"
#include "hardware/structs/watchdog.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"

#include <string.h>

#include "line_ring.h"
#include "osd/fast_osd.h"
#include "pico.h"
#include "settings.h"
#include "video_config.h"

// NEOPICO_EXP_GENLOCK_DYNAMIC's default-0 guard lives in video_pipeline.h
// (included first), next to the VIDEO_PIPELINE_VSYNC_RAM placement macro.
#if NEOPICO_EXP_GENLOCK_DYNAMIC
#include "video_capture.h"
#endif

#ifndef NEOPICO_VIDEO_TEST_PATTERN
#define NEOPICO_VIDEO_TEST_PATTERN 0
#endif

// Scanline effect toggle (off by default)
bool fx_scanlines_enabled = false;
static bool osd_visible_latched = false;
typedef void (*pixel_scale_fn_t)(uint32_t *dst, const uint16_t *src, int count);
typedef void (*pixel_scale_osd_fn_t)(uint32_t *dst, const uint16_t *game, const uint16_t *osd, int count);
// Overscan/background outside active 224-line image area (RGB565): black.
#define OVERSCAN_COLOR_RGB565 0x0000
// Missing/not-ready capture-line fallback: International Orange
// (aerospace), #FF4F00, converted to RGB565.
#define NO_SIGNAL_COLOR_RGB565 0xFA60

static void __scratch_x("000_video_pipeline_modes")
    video_pipeline_scanline_callback_reboot_modes(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);

static video_pipeline_reboot_mode_t reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
#define REBOOT_MODE_BOOT_MAGIC 0x4e505253U
#define REBOOT_MODE_BOOT_CHECK_XOR 0xa5a55a5aU
// Resolution-change confirmation marker, packed into the SINGLE user-safe
// scratch register that survives a plain watchdog_reboot. The SDK's
// watchdog_reboot(0,0,..) clobbers scratch[4..7], and the existing reboot mode
// already owns scratch[0..2], so only scratch[3] is left. Layout:
//   [31:8] magic | [7:4] previous (revert-to) mode | [3:0] check (prev ^ 0xA)
// The new mode is NOT persisted to flash until the user confirms; cancel/timeout
// reboots to the previous mode (flash still holds the last confirmed resolution).
#define REBOOT_PENDING_MAGIC 0x4e505000U // "NPP" in bits [31:8]

bool video_pipeline_reboot_mode_available(uint8_t mode)
{
    switch ((video_pipeline_reboot_mode_t)mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
        case VIDEO_PIPELINE_REBOOT_MODE_720P:
            return true;
        default:
            return false;
    }
}

static inline uint32_t reboot_mode_boot_check(uint32_t mode)
{
    return REBOOT_MODE_BOOT_MAGIC ^ mode ^ REBOOT_MODE_BOOT_CHECK_XOR;
}

static void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
    __attribute__((noinline, noclone));

static void __scratch_y("") video_pipeline_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
{
    const uint32_t packed = ((uint32_t)color << 16) | color;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = packed;
    }
}

// Fake OSD transparency: black background pixels retain 12.5% of the captured
// game pixel underneath; nonblack OSD pixels remain fully opaque. Process two
// packed RGB565 pixels at a time so selection remains branch-free per pixel.
#define VIDEO_PIPELINE_RGB565_RETAIN_1_8_MASK_2PX 0xC718C718U
_Static_assert((OSD_BOX_X & 1U) == 0U, "fake-blend OSD X must be two-pixel aligned");
_Static_assert((OSD_BOX_W & 1U) == 0U, "fake-blend OSD width must contain complete pixel pairs");

static inline __attribute__((always_inline)) uint32_t video_pipeline_osd_fake_blend_pair(uint32_t game_pair,
                                                                                         uint32_t osd_pair)
{
    const uint32_t dim_pair = (game_pair & VIDEO_PIPELINE_RGB565_RETAIN_1_8_MASK_2PX) >> 3;
    const uint32_t lo = osd_pair & 0xFFFFU;
    const uint32_t hi = osd_pair >> 16;
    const uint32_t lo_mask = 0U - (uint32_t)(lo != (uint32_t)OSD_COLOR_BG);
    const uint32_t hi_mask = 0U - (uint32_t)(hi != (uint32_t)OSD_COLOR_BG);
    const uint32_t osd_mask = (lo_mask & 0x0000FFFFU) | (hi_mask << 16);
    return (osd_pair & osd_mask) | (dim_pair & ~osd_mask);
}

static void __scratch_y("")
    video_pipeline_double_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                const uint16_t *restrict osd, int count)
        __attribute__((noinline, noclone));
static void __scratch_y("")
    video_pipeline_triple_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                const uint16_t *restrict osd, int count)
        __attribute__((noinline, noclone));
static void __scratch_y("")
    video_pipeline_quadruple_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                   const uint16_t *restrict osd, int count)
        __attribute__((noinline, noclone));

static void __scratch_y("")
    video_pipeline_double_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                const uint16_t *restrict osd, int count)
{
    const uint32_t *game32 = (const uint32_t *)game;
    const uint32_t *osd32 = (const uint32_t *)osd;
    const int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        const uint32_t blended = video_pipeline_osd_fake_blend_pair(game32[i], osd32[i]);
        const uint32_t p0 = blended & 0xFFFFU;
        const uint32_t p1 = blended >> 16;
        dst[0] = p0 | (p0 << 16);
        dst[1] = p1 | (p1 << 16);
        dst += 2;
    }
}

static void __scratch_y("")
    video_pipeline_triple_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                const uint16_t *restrict osd, int count)
{
    const uint32_t *game32 = (const uint32_t *)game;
    const uint32_t *osd32 = (const uint32_t *)osd;
    const int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        const uint32_t blended = video_pipeline_osd_fake_blend_pair(game32[i], osd32[i]);
        const uint32_t p0 = blended & 0xFFFFU;
        const uint32_t p1 = blended >> 16;
        dst[(i * 3) + 0] = p0 | (p0 << 16);
        dst[(i * 3) + 1] = p0 | (p1 << 16);
        dst[(i * 3) + 2] = p1 | (p1 << 16);
    }
}

static void __scratch_y("")
    video_pipeline_quadruple_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                   const uint16_t *restrict osd, int count)
{
    const uint32_t *game32 = (const uint32_t *)game;
    const uint32_t *osd32 = (const uint32_t *)osd;
    const int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        const uint32_t blended = video_pipeline_osd_fake_blend_pair(game32[i], osd32[i]);
        const uint32_t p0 = blended & 0xFFFFU;
        const uint32_t p1 = blended >> 16;
        const uint32_t d0 = p0 | (p0 << 16);
        const uint32_t d1 = p1 | (p1 << 16);
        dst[(i * 4) + 0] = d0;
        dst[(i * 4) + 1] = d0;
        dst[(i * 4) + 2] = d1;
        dst[(i * 4) + 3] = d1;
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
    video_output_set_vsync_callback(video_pipeline_vsync_callback);
    if (video_output_active_mode->h_active_pixels == 1280U && video_output_active_mode->v_active_lines == 720U) {
        reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_720P;
    } else if (video_output_active_mode->v_active_lines == 240U) {
        reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_240P;
    } else {
        reboot_requested_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
    video_output_set_scanline_callback(video_pipeline_scanline_callback_reboot_modes);

    osd_visible_latched = osd_visible;
}

#define VIDEO_PIPELINE_REBOOT_REQUEST_RAM(name) name

void VIDEO_PIPELINE_REBOOT_REQUEST_RAM(video_pipeline_request_reboot_mode)(video_pipeline_reboot_mode_t mode)
{
    if (!video_pipeline_reboot_mode_available((uint8_t)mode)) {
        mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
    reboot_requested_mode = mode;
    // Persistence is NOT done here: a normal/confirmed/revert reboot must not
    // write flash (only an explicit settings_save on confirm does). Clear any
    // stale pending-confirmation marker so this boot is treated as confirmed.
    watchdog_hw->scratch[0] = REBOOT_MODE_BOOT_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)mode;
    watchdog_hw->scratch[2] = reboot_mode_boot_check((uint32_t)mode);
    watchdog_hw->scratch[3] = 0;
    watchdog_hw->scratch[4] = 0;
    watchdog_hw->scratch[5] = 0;
    __dmb();
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

// Reboot into `mode` but flag it as PENDING confirmation, carrying the
// `previous` (revert-to) mode across the reboot. Does NOT persist to flash.
void VIDEO_PIPELINE_REBOOT_REQUEST_RAM(video_pipeline_request_reboot_mode_pending)(
    video_pipeline_reboot_mode_t mode, video_pipeline_reboot_mode_t previous)
{
    if (!video_pipeline_reboot_mode_available((uint8_t)mode)) {
        mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
    if (!video_pipeline_reboot_mode_available((uint8_t)previous)) {
        previous = VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
    reboot_requested_mode = mode;
    watchdog_hw->scratch[0] = REBOOT_MODE_BOOT_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)mode;
    watchdog_hw->scratch[2] = reboot_mode_boot_check((uint32_t)mode);
    watchdog_hw->scratch[3] =
        REBOOT_PENDING_MAGIC | (((uint32_t)previous & 0xFU) << 4) | (((uint32_t)previous ^ 0xAU) & 0xFU);
    __dmb();
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

bool video_pipeline_take_pending_confirmation(video_pipeline_reboot_mode_t *previous_mode)
{
    const uint32_t packed = watchdog_hw->scratch[3];
    watchdog_hw->scratch[3] = 0;

    const uint32_t mode = (packed >> 4) & 0xFU;
    const uint32_t check = packed & 0xFU;
    if (((packed & 0xFFFFFF00U) != REBOOT_PENDING_MAGIC) || !video_pipeline_reboot_mode_available((uint8_t)mode) ||
        (check != ((mode ^ 0xAU) & 0xFU))) {
        return false;
    }
    if (previous_mode) {
        *previous_mode = (video_pipeline_reboot_mode_t)mode;
    }
    return true;
}

video_pipeline_reboot_mode_t video_pipeline_reboot_requested_mode(void)
{
    return reboot_requested_mode;
}

bool video_pipeline_take_reboot_mode_boot_request(video_pipeline_reboot_mode_t *requested_mode)
{
    const uint32_t magic = watchdog_hw->scratch[0];
    const uint32_t mode = watchdog_hw->scratch[1];
    const uint32_t check = watchdog_hw->scratch[2];
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;

    if ((magic != REBOOT_MODE_BOOT_MAGIC) || !video_pipeline_reboot_mode_available((uint8_t)mode) ||
        (check != reboot_mode_boot_check(mode))) {
        return false;
    }

    if (requested_mode) {
        *requested_mode = (video_pipeline_reboot_mode_t)mode;
    }
    return true;
}

void video_pipeline_request_reboot_240p(bool enabled)
{
    video_pipeline_request_reboot_mode(enabled ? VIDEO_PIPELINE_REBOOT_MODE_240P : VIDEO_PIPELINE_REBOOT_MODE_480P);
}

bool video_pipeline_reboot_requested_240p(void)
{
    return reboot_requested_mode == VIDEO_PIPELINE_REBOOT_MODE_240P;
}

bool video_pipeline_take_reboot_240p_boot_request(bool *enabled)
{
    video_pipeline_reboot_mode_t mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    if (!video_pipeline_take_reboot_mode_boot_request(&mode) ||
        (mode != VIDEO_PIPELINE_REBOOT_MODE_480P && mode != VIDEO_PIPELINE_REBOOT_MODE_240P)) {
        return false;
    }

    if (enabled) {
        *enabled = (mode == VIDEO_PIPELINE_REBOOT_MODE_240P);
    }
    return true;
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
// Nominals that approximate MVS ~59.18 Hz at each mode's pixel clock:
//   480p: 25.2M / (800 * 532) = 59.21 Hz   (±1 → 59.10–59.32 Hz)
//   240p: 25.2M / (1600 * 266) = 59.21 Hz  (±1 → 58.99–59.43 Hz)
#define GENLOCK_NOMINAL_VTOTAL_480 532
#define GENLOCK_NOMINAL_VTOTAL_240 266
//   720p (exact-clock 1440 h_total @ 64 MHz pixel, 22.5 us lines):
//   64M / (1440 * 751) = 59.180 Hz (+1.5 us/frame vs the 16896.0 us MVS
//   frame) — nominal creeps the phase UP very slowly; 750 (-21 us/frame)
//   pulls back. (The pre-sunset nominal 762 belonged to the deleted 372 MHz
//   1650-h_total timing and must not be reused here.)
#define GENLOCK_NOMINAL_VTOTAL_720 751
#define GENLOCK_PHASE_THRESHOLD_US 200
#define GENLOCK_PHASE_MAX_US 5000
// Output vsyncs landing shortly after the MVS vsync sample a frame base no
// capture line has been committed to yet (lines prep gray): phase must stay
// inside [~3ms, ~15ms]. A tight setpoint needs a fractional line rate and
// dithers vtotal every few frames, which makes sinks' vertical lock hunt
// (top-of-image wobble). Instead: WIDE HYSTERESIS — let the phase ramp the
// healthy zone on a constant vtotal, pull it back on a constant vtotal-1.
// Two one-line timing changes per ~42 s instead of ~18 per second.
#define GENLOCK_PHASE_PULLBACK_AT_US 14000
#define GENLOCK_PHASE_RESUME_AT_US 4000
#define GENLOCK_PHASE_SETPOINT_US 11000

// Once per frame from the vsync callback; does not need scratch residency
// (and scratch_x is at its hard boundary).
volatile uint32_t g_genlock_phase_us;      // published for the genlock OSD
volatile uint32_t g_genlock_outzone_count; // raw out-of-zone frames (incl. measurement spikes)

static void genlock_dynamic_update(void)
{
    uint32_t hdmi_ts = timer_hw->timerawl;
    uint32_t mvs_ts = g_mvs_vsync_timestamp;
    uint32_t phase = hdmi_ts - mvs_ts; // us since last MVS vsync, [0, ~16.9ms)
    g_genlock_phase_us = phase;

    const uint16_t mode_total = video_output_active_mode->v_total_lines;
    uint16_t nominal = (mode_total <= 266)   ? GENLOCK_NOMINAL_VTOTAL_240
                       : (mode_total >= 700) ? GENLOCK_NOMINAL_VTOTAL_720
                                             : GENLOCK_NOMINAL_VTOTAL_480;

    // Steady state: vtotal stays at nominal FOREVER and a proportional servo
    // on the blanking h-trim nulls the residual drift (sub-line steps are
    // invisible; vtotal steps of a whole line visibly disturb some sinks).
    // vtotal only steps during acquire (boot / signal reappearing), when the
    // phase is outside the content-safe zone and speed matters more than
    // cosmetics.
    static int applied_trim;
    static uint16_t step_cooldown;
    static uint8_t out_zone_streak;

    // A single late mvs-timestamp IRQ (Core 0 shares with USB/capture) makes
    // the phase READ as a huge excursion for one frame; acting on it puts a
    // one-frame vtotal step on the wire -- the exact whole-frame jolt this
    // sink shows. Count raw excursions (OSD "A") but only ACT on a streak a
    // measurement spike cannot produce.
    const bool out_low = phase < GENLOCK_PHASE_RESUME_AT_US;
    const bool out_high = phase > GENLOCK_PHASE_PULLBACK_AT_US;
    if (out_low || out_high) {
        g_genlock_outzone_count++;
        if (out_zone_streak < 255) {
            out_zone_streak++;
        }
    } else {
        out_zone_streak = 0;
    }

    if (out_zone_streak >= 8 && out_low) {
        rt_v_total_lines = (uint16_t)(nominal + 1); // fast acquire upward
    } else if (out_zone_streak >= 8 && out_high) {
        rt_v_total_lines = (uint16_t)(nominal - 1); // fast pull back down
    } else {
        rt_v_total_lines = nominal;
        // SLOW INTEGRATOR. Bench finding: this sink tolerates any CONSTANT
        // trim (even the clamp) but visibly glitches on trim ACTIVITY --
        // the proportional servo hunting +-1 px at frame rate was itself
        // the artifact. So: hold trim absolutely constant; only when the
        // phase wanders past the deadband, make a single 1-px adjustment,
        // then hold again (rate-limited: 10 frames while far out for
        // convergence, 60 frames near lock). At equilibrium the +-0.5 px
        // quantization means one lone pixel-step every few MINUTES.
        // Derivative gating: trim->drift->phase is a double integration, so
        // stepping on position error alone limit-cycles (observed: trim
        // sweeping -6..-30 forever, glitching at every step). Only step
        // while the phase is NOT already heading back toward the setpoint;
        // this both damps the cycle and acts as anti-windup. Settles on the
        // quantization-optimal constant trim with a lone +-1 px touch every
        // ~30-60 s.
        static uint32_t drift_prev_phase;
        static int32_t drift_per64; // us per 64 frames; 1 px ~= 12
        static uint8_t drift_ctr;
        static bool drift_valid;
        if (!drift_valid) {
            drift_prev_phase = phase;
            drift_valid = true;
        }
        if (++drift_ctr >= 64) {
            drift_per64 = (int32_t)(phase - drift_prev_phase);
            drift_prev_phase = phase;
            drift_ctr = 0;
        }

        int32_t e_us = (int32_t)phase - (int32_t)GENLOCK_PHASE_SETPOINT_US;
        if (step_cooldown) {
            step_cooldown--;
        } else if (e_us > 400 && drift_per64 >= -6) {
            if (applied_trim > -30) {
                applied_trim--;
                video_output_set_vblank_htrim_px(applied_trim);
            }
            step_cooldown = 30;
        } else if (e_us < -400 && drift_per64 <= 6) {
            if (applied_trim < 30) {
                applied_trim++;
                video_output_set_vblank_htrim_px(applied_trim);
            }
            step_cooldown = 30;
        }
    }
}
#endif

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 *
 * Placement (VIDEO_PIPELINE_VSYNC_RAM, see the header): scratch_x content
 * plus the 2 KiB core-1 stack fill the 4 KiB bank EXACTLY (the link fails on
 * a single added instruction), so the extra genlock call cannot live there.
 * With genlock ON the callback moves to scratch_y, which has over 1 KiB of
 * headroom; the servo body itself runs from normal RAM (once per frame in
 * blanking, no scratch residency needed).
 */
void VIDEO_PIPELINE_VSYNC_RAM video_pipeline_vsync_callback(void)
{
    line_ring_output_vsync();
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    genlock_dynamic_update();
#endif
    osd_visible_latched = osd_visible;
}

static void __scratch_x("000_video_pipeline_modes")
    video_pipeline_scanline_callback_reboot_modes(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    const uint32_t active_width = video_output_active_mode->h_active_pixels;
    const uint32_t active_height = video_output_active_mode->v_active_lines;
    const bool mode_is_240p = active_width == 1280U && active_height == 240U;
    const bool mode_is_3x = active_width == 1280U && active_height == 720U;
    const uint32_t h_words = active_width / 2U;
    const uint32_t h_scale = mode_is_3x ? 3U : mode_is_240p ? 4U : 2U;
    const uint32_t image_words = (LINE_WIDTH * h_scale) / 2U;
    const uint32_t x_margin_words = (h_words > image_words) ? ((h_words - image_words) / 2U) : 0U;
    const pixel_scale_fn_t scale_pixels = mode_is_3x     ? video_pipeline_triple_pixels_fast
                                          : mode_is_240p ? video_pipeline_quadruple_pixels_fast
                                                         : video_pipeline_double_pixels_fast;
    const pixel_scale_osd_fn_t scale_osd_pixels = mode_is_3x     ? video_pipeline_triple_pixels_osd_fake_blend
                                                  : mode_is_240p ? video_pipeline_quadruple_pixels_osd_fake_blend
                                                                 : video_pipeline_double_pixels_osd_fake_blend;
    uint32_t image_active_line = active_line;
    if (mode_is_3x && ((image_active_line % 3U) != 0U)) {
        return;
    }
    const uint32_t fb_line = mode_is_3x     ? (image_active_line / 3U)
                             : mode_is_240p ? image_active_line
                                            : (image_active_line >> 1);
    const uint32_t osd_x_words = x_margin_words + (((uint32_t)OSD_BOX_X * h_scale) / 2U);
    const uint32_t osd_w_words = (((uint32_t)OSD_BOX_W * h_scale) / 2U);
#define VIDEO_PIPELINE_SCALE_SELECTED(dst_arg, src_arg, count_arg) scale_pixels((dst_arg), (src_arg), (count_arg))
#define VIDEO_PIPELINE_SCALE_OSD_SELECTED(dst_arg, game_arg, osd_arg, count_arg)                                       \
    scale_osd_pixels((dst_arg), (game_arg), (osd_arg), (count_arg))

#if NEOPICO_VIDEO_TEST_PATTERN
    // Diagnostic test pattern for isolating HSTX timing issues; historically
    // scoped to the 3x-scaled (720p-class) modes only.
    if (mode_is_3x) {
        if (!test_pattern_line_ready) {
            video_pipeline_init_test_pattern_line();
        }
        if ((image_active_line % 3U) != 0U) {
            return;
        }
        video_pipeline_fill_rgb565(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, test_pattern_line, LINE_WIDTH);
        video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                                   OVERSCAN_COLOR_RGB565);
        return;
    }
#endif

    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    const bool osd_line_active = osd_visible_latched && (osd_line_u32 < OSD_BOX_H);

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
    // OSD region: opaque blit by default, or fixed 87.5% black-panel opacity.
    VIDEO_PIPELINE_SCALE_OSD_SELECTED(dst + osd_x_words, src + OSD_BOX_X, osd_src, OSD_BOX_W);
    // After OSD
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words + osd_w_words, src + OSD_BOX_X + OSD_BOX_W,
                                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    video_pipeline_fill_rgb565(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                               OVERSCAN_COLOR_RGB565);
#undef VIDEO_PIPELINE_SCALE_OSD_SELECTED
#undef VIDEO_PIPELINE_SCALE_SELECTED
}

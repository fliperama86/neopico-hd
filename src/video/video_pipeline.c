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

// FEASIBILITY SPIKE (default OFF, not for shipping): can the Core 1 per-line
// scanout path afford one 32-bit RGB888 word per output pixel instead of two
// packed RGB565 pixels per word, at 480p/720p, without HSTX FIFO underruns?
// Plain bit-replication RGB565->RGB888 only -- no DARK/SHADOW model changes,
// no line-ring format changes. See lib/pico_hdmi's matching
// PICO_HDMI_PIXEL_FORMAT_RGB888 option for the HSTX-side half of this.
#ifndef NEOPICO_EXP_RGB888_SCANOUT
#define NEOPICO_EXP_RGB888_SCANOUT 0
#endif

// 50% scanlines at 480p (permanent feature). See the hardware-validated
// 720p counterpart in lib/pico_hdmi, which dims with a per-pixel __uhadd8
// SIMD instruction in a DEFERRED fill (3-line window). 480p CANNOT use that
// approach: its scanline callback
// re-renders every source line from the line ring for BOTH physical output
// lines of a 2x pair inside the PER-LINE ISR itself (no early return, no
// reused buffer, 1-line window -- see fb_line below), which is already the
// tightest CPU budget in the system. A first version dimmed with a per-pixel
// uhadd8 kernel plus a per-line function-pointer-selection call; hardware
// testing showed that dropped 480p HDMI sync (720p was fine) -- ~320 extra
// uhadd8 per dimmed line plus the selection call's memory traffic, on EVERY
// line in EVERY mode, was too much for the per-line ISR's 1-line window.
//
// Current approach costs ZERO per-pixel work instead: a second, pre-dimmed
// LUT (g_effect_lut888_dim, generated once at init by halving every channel
// of g_effect_lut888). The scale functions already do one LUT lookup per
// source pixel; dimming becomes a matter of WHICH table that lookup reads,
// not extra work per pixel. The callback (scratch_x, no headroom to spare)
// writes only a single bool, g_scanline_dim_line, once per line, right next
// to where g_scanline_shadow is already latched -- same per-line cost as
// before, now just a bool store instead of a bool store PLUS ~320 uhadd8
// PLUS a cross-TU call. video_pipeline_double_pixels_fast (scratch_y, which
// has headroom) does the actual pointer ternary between the two large LUT
// addresses once per function call, not per pixel: an earlier version
// resolved the pointer directly in the callback and measured 32-88 bytes of
// SCRATCH_X overflow just from needing both LUT addresses at the per-line
// assignment sites, even after hoisting the selection condition to a single
// precomputed local. g_scanline_dim_line is true only for the second
// physical line of a 2x/480p pair (active_line odd); always false for 240p
// (structurally impossible: no vertical scaling) and 720p (has its own
// uhadd8 path in the library, untouched by this). RGB888-only
// (NEOPICO_EXP_RGB888_SCANOUT): the non-RGB888 path is unaffected. The OSD
// span is never dimmed -- full brightness always, so the menu stays crisp
// over scanlines; this predates the LUT rework and was unrelated to the
// sync failure, so it is unchanged.
//
// Hardware-validated 2026-08-05 (this LUT-based version, including runtime
// levels and persistence; the earlier per-pixel-kernel version above is what
// failed on hardware and was replaced by this one).

// ---------------------------------------------------------------------------
// Scanline timing trace (NEOPICO_EXP_SCANLINE_TRACE, default OFF).
//
// Records how many CPU cycles each scanline callback takes, into a RAM ring
// buffer that Core 0 dumps as raw binary on request. Deliberately does NOT
// format anything on Core 1: snprintf in the Core 1 background task is a
// documented cause of HSTX FIFO underruns in this firmware, so the expensive
// half happens on Core 0 and only after the operator has already seen the
// failure. Cost here is two PPB reads and one halfword store, roughly 0.1% of
// a 480p line, which is small enough not to be the thing under test -- the
// known-good 480p mode is the control that confirms that.
// ---------------------------------------------------------------------------
#ifndef NEOPICO_EXP_SCANLINE_TRACE
#define NEOPICO_EXP_SCANLINE_TRACE 0
#endif

#if NEOPICO_EXP_SCANLINE_TRACE
// Architectural ARMv8-M debug register addresses, used directly rather than
// pulling in cmsis_core: adding a link dependency shifts layout, and layout
// shifts have caused HSTX underruns in this firmware before. Bit positions
// match the SDK's CMSIS core_cm33.h (TRCENA bit 24, CYCCNTENA bit 0).
#define TRACE_DWT_CTRL (*(volatile uint32_t *)0xE0001000U)
#define TRACE_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004U)
#define TRACE_DEMCR (*(volatile uint32_t *)0xE000EDFCU)
#define TRACE_DEMCR_TRCENA (1U << 24U)
#define TRACE_DWT_CYCCNTENA (1U << 0U)

#define SCANLINE_TRACE_ENTRIES 16384U // power of two: 32 KiB, ~34 frames at 480p
uint16_t g_scanline_trace[SCANLINE_TRACE_ENTRIES];
volatile uint32_t g_scanline_trace_idx;

// DWT is per-core, so this must run on the core being measured (Core 1).
static inline void scanline_trace_init(void)
{
    TRACE_DEMCR |= TRACE_DEMCR_TRCENA;
    TRACE_DWT_CYCCNT = 0;
    TRACE_DWT_CTRL |= TRACE_DWT_CYCCNTENA;
}
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

#if NEOPICO_EXP_RGB888_SCANOUT
#include "mvs_effect_lut.h"

// Full-precision colour model for the 32-bit scanout path. The ring carries
// raw entropy, so the DARK half-step survives in red and blue here, which it
// cannot in an RGB565 ring. SHADOW is per line (screen-wide control), latched
// once per scanline rather than threaded through every kernel signature.
static mvs_effect_lut888_t g_effect_lut888;
static uint32_t g_scanline_shadow;

// 50% scanlines at 480p, LUT-based (see the comment at the top of this file
// for why): a full pre-dimmed companion table, so a dimmed line
// costs nothing per pixel -- the scale functions already do one LUT lookup
// per source pixel, so dimming it becomes a matter of WHICH table they read,
// not extra per-pixel work. g_scanline_dim_line (below) says whether the
// current physical output line should read this table; the pointer ternary
// itself lives in video_pipeline_double_pixels_fast, not here. Plain bss,
// not scratch: 16896 bytes is trivial against the ~180 KB of free main RAM,
// and
// scratch_x/scratch_y have none to spare.
static mvs_effect_lut888_t g_effect_lut888_dim;
// The per-line SELECT (is this line dimmed?) is a single bool, written by
// the scratch_x callback; the ACTUAL pointer ternary between the two large
// LUT addresses is done once per function call inside
// video_pipeline_double_pixels_fast (scratch_y, which has headroom) rather
// than once per line inside the callback (scratch_x, which does not): an
// earlier version wrote the resolved pointer directly from the callback and
// measured 32-88 bytes of SCRATCH_X overflow from the two literal LUT
// addresses needed at each of the per-line assignment sites, even after
// hoisting the condition to a single precomputed local.
static bool g_scanline_dim_line;
// Current scanline STRENGTH, 0..4 (video_pipeline_scanline_level_t in
// video_pipeline.h). Defaults to 50%, matching this feature's previous fixed
// behavior before runtime levels existed.
// video_pipeline_set_scanline_dim_line() below ANDs its selection on this
// being != OFF, so OFF disables dim selection entirely rather than relying
// on an identity-valued dim table (see video_pipeline_set_scanline_level()
// further down, which also owns the only writes to this after init).
static uint8_t g_scanline_level = VIDEO_PIPELINE_SCANLINE_50;

// Computing (not just storing) the dim condition inline in the callback --
// in any of several shapes/placements tried, including right next to the
// g_scanline_shadow assignment sites further down (the placement it might
// seem most natural to mirror) -- measured a persistent SCRATCH_X cost of
// 16-88 bytes, because it forced worse register allocation through this
// whole tightly-packed function, not just at the new code. Moving the
// COMPUTATION itself into this __scratch_y helper, called exactly once, up
// front (see the call site above, not duplicated at the shadow-assignment
// sites), is what actually fit: 0 bytes of SCRATCH_X headroom left, but it
// links. Guarding the call with `if (h_scale == 2U)` to skip it for
// 240p/720p (where g_scanline_dim_line is never read) was tried both here
// and at the shadow sites and measured WORSE every time (20-28 bytes over):
// the extra branch always cost more than the skipped call saved. Two
// arguments (h_scale, active_line), unconditional, is the cheapest shape
// found.
static void __attribute__((noinline, noclone)) __scratch_y("")
    video_pipeline_set_scanline_dim_line(uint32_t h_scale, uint32_t active_line)
{
    g_scanline_dim_line =
        (g_scanline_level != VIDEO_PIPELINE_SCANLINE_OFF) && (h_scale == 2U) && ((active_line & 1U) != 0U);
}

// Per-8-bit-channel dim formula for `level` (see the enum in video_pipeline.h
// for the shared values/formulas). 50% matches __uhadd8(v, 0) exactly (a
// halving-add against zero is a plain per-byte v>>1, no rounding); the other
// levels are plain integer arithmetic since this runs once per level change,
// not per pixel -- no SIMD instruction needed here (contrast the 720p path
// in pico_hdmi, which runs per PIXEL and does use __uhadd8).
static inline uint32_t video_pipeline_scanline_dim_channel(uint32_t v, uint8_t level)
{
    switch (level) {
        case VIDEO_PIPELINE_SCANLINE_25:
            return (v + (v >> 1)) >> 1;
        case VIDEO_PIPELINE_SCANLINE_50:
            return v >> 1;
        case VIDEO_PIPELINE_SCANLINE_75:
            return v >> 2;
        case VIDEO_PIPELINE_SCANLINE_100:
            return 0;
        default: // OFF, or any unexpected value: identity (never selected -- see g_scanline_dim_line above)
            return v;
    }
}

// Fills g_effect_lut888_dim from the already-generated g_effect_lut888,
// applying `level`'s dim formula to each 8-bit channel independently.
// Channels must be handled independently, never the whole packed word: the
// rg table packs r8 at bits[23:16] directly against g8 at bits[15:8] with no
// padding byte between them, so shifting the whole word right would bleed
// r8's low bit into g8's high bit.
static void video_pipeline_dim_lut888_generate(uint8_t level)
{
    for (uint32_t i = 0; i < MVS_EFFECT_RG_TABLE_ENTRIES; i++) {
        const uint32_t v = g_effect_lut888.rg[i];
        const uint32_t r8 = video_pipeline_scanline_dim_channel((v >> 16) & 0xFFU, level);
        const uint32_t g8 = video_pipeline_scanline_dim_channel((v >> 8) & 0xFFU, level);
        g_effect_lut888_dim.rg[i] = (r8 << 16) | (g8 << 8);
    }
    for (uint32_t i = 0; i < MVS_EFFECT_B_TABLE_ENTRIES; i++) {
        g_effect_lut888_dim.b[i] = video_pipeline_scanline_dim_channel(g_effect_lut888.b[i], level);
    }
}

// Plain bit-replication RGB565 -> RGB888 (NOT the DARK/SHADOW model). Packs
// as 0x00RRGGBB to match the HSTX RGB888 expand_tmds lane layout (L0=blue
// ROT=0, L1=green ROT=8, L2=red ROT=16).
static inline __attribute__((always_inline)) uint32_t video_pipeline_rgb565_to_rgb888(uint16_t c)
{
    const uint32_t r5 = (c >> 11) & 0x1FU;
    const uint32_t g6 = (c >> 5) & 0x3FU;
    const uint32_t b5 = c & 0x1FU;
    const uint32_t r8 = (r5 << 3) | (r5 >> 2);
    const uint32_t g8 = (g6 << 2) | (g6 >> 4);
    const uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 16) | (g8 << 8) | b8;
}

// One 32-bit RGB888 word per output pixel (vs 2 packed RGB565 pixels/word).
static void __scratch_y("") video_pipeline_fill_rgb888(uint32_t *dst, uint32_t words, uint32_t color888)
    __attribute__((noinline, noclone));

static void __scratch_y("") video_pipeline_fill_rgb888(uint32_t *dst, uint32_t words, uint32_t color888)
{
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = color888;
    }
}

#define VIDEO_PIPELINE_FILL(dst_arg, words_arg, rgb565color_arg)                                                       \
    video_pipeline_fill_rgb888((dst_arg), (words_arg), video_pipeline_rgb565_to_rgb888((uint16_t)(rgb565color_arg)))
#else
#define VIDEO_PIPELINE_FILL(dst_arg, words_arg, rgb565color_arg)                                                       \
    video_pipeline_fill_rgb565((dst_arg), (words_arg), (rgb565color_arg))
#endif

#if !NEOPICO_EXP_RGB888_SCANOUT
// Non-RGB888 builds (e.g. SNES, where RGB888 scanout auto-disables for any
// non-MVS capture target -- see the CMakeLists.txt guard): the 480p dim-LUT
// machinery above doesn't exist, but video_pipeline_set_scanline_level() /
// video_pipeline_get_scanline_level() below still need a place to track the
// menu's current level.
static uint8_t g_scanline_level = VIDEO_PIPELINE_SCANLINE_50;
#endif

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
#if NEOPICO_EXP_RGB888_SCANOUT
    (void)game; // opaque OSD: game pixels are not sampled on this path
    const uint32_t *osd32 = (const uint32_t *)osd;
    const int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        // Opaque OSD under 32-bit scanout. The translucent blend reads game
        // pixels, which now carry raw entropy rather than RGB565, so it would
        // need a LUT lookup per pixel ON TOP of the blend and the OSD colour
        // conversion. Measured, that path was already ~86% of the 480p line
        // budget before any of that was added, and the OSD box spans most of
        // the line. Emitting the OSD pixel directly makes these lines cheaper
        // than ordinary ones instead of twice the cost. Translucency can come
        // back if headroom appears.
        const uint32_t opaque = osd32[i];
        const uint32_t c0 = video_pipeline_rgb565_to_rgb888((uint16_t)(opaque & 0xFFFFU));
        const uint32_t c1 = video_pipeline_rgb565_to_rgb888((uint16_t)(opaque >> 16));
        dst[0] = c0;
        dst[1] = c0;
        dst[2] = c1;
        dst[3] = c1;
        dst += 4;
    }
#else
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
#endif
}

static void __scratch_y("")
    video_pipeline_triple_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                const uint16_t *restrict osd, int count)
{
#if NEOPICO_EXP_RGB888_SCANOUT
    (void)game; // opaque OSD: game pixels are not sampled on this path
    const uint32_t *osd32 = (const uint32_t *)osd;
    const int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        // Opaque OSD under 32-bit scanout. The translucent blend reads game
        // pixels, which now carry raw entropy rather than RGB565, so it would
        // need a LUT lookup per pixel ON TOP of the blend and the OSD colour
        // conversion. Measured, that path was already ~86% of the 480p line
        // budget before any of that was added, and the OSD box spans most of
        // the line. Emitting the OSD pixel directly makes these lines cheaper
        // than ordinary ones instead of twice the cost. Translucency can come
        // back if headroom appears.
        const uint32_t opaque = osd32[i];
        const uint32_t c0 = video_pipeline_rgb565_to_rgb888((uint16_t)(opaque & 0xFFFFU));
        const uint32_t c1 = video_pipeline_rgb565_to_rgb888((uint16_t)(opaque >> 16));
        dst[(i * 6) + 0] = c0;
        dst[(i * 6) + 1] = c0;
        dst[(i * 6) + 2] = c0;
        dst[(i * 6) + 3] = c1;
        dst[(i * 6) + 4] = c1;
        dst[(i * 6) + 5] = c1;
    }
#else
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
#endif
}

static void __scratch_y("")
    video_pipeline_quadruple_pixels_osd_fake_blend(uint32_t *restrict dst, const uint16_t *restrict game,
                                                   const uint16_t *restrict osd, int count)
{
#if NEOPICO_EXP_RGB888_SCANOUT
    (void)game; // opaque OSD: game pixels are not sampled on this path
    const uint32_t *osd32 = (const uint32_t *)osd;
    const int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        // Opaque OSD under 32-bit scanout. The translucent blend reads game
        // pixels, which now carry raw entropy rather than RGB565, so it would
        // need a LUT lookup per pixel ON TOP of the blend and the OSD colour
        // conversion. Measured, that path was already ~86% of the 480p line
        // budget before any of that was added, and the OSD box spans most of
        // the line. Emitting the OSD pixel directly makes these lines cheaper
        // than ordinary ones instead of twice the cost. Translucency can come
        // back if headroom appears.
        const uint32_t opaque = osd32[i];
        const uint32_t c0 = video_pipeline_rgb565_to_rgb888((uint16_t)(opaque & 0xFFFFU));
        const uint32_t c1 = video_pipeline_rgb565_to_rgb888((uint16_t)(opaque >> 16));
        dst[(i * 8) + 0] = c0;
        dst[(i * 8) + 1] = c0;
        dst[(i * 8) + 2] = c0;
        dst[(i * 8) + 3] = c0;
        dst[(i * 8) + 4] = c1;
        dst[(i * 8) + 5] = c1;
        dst[(i * 8) + 6] = c1;
        dst[(i * 8) + 7] = c1;
    }
#else
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
#endif
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
#if NEOPICO_EXP_RGB888_SCANOUT
    mvs_effect_lut888_generate(&g_effect_lut888);
    video_pipeline_dim_lut888_generate(g_scanline_level);
#endif
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
#if !NEOPICO_EXP_GENLOCK_DYNAMIC
    // Non-genlock builds: unchanged from before this feature existed (clear
    // unconditionally, byte-for-byte -- there is no other pending-kind that
    // could share this register, so nothing to preserve on a mismatch).
    watchdog_hw->scratch[3] = 0;
#endif
    const uint32_t mode = (packed >> 4) & 0xFU;
    const uint32_t check = packed & 0xFU;
    if (((packed & 0xFFFFFF00U) != REBOOT_PENDING_MAGIC) || !video_pipeline_reboot_mode_available((uint8_t)mode) ||
        (check != ((mode ^ 0xAU) & 0xFU))) {
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        // Do NOT clear scratch[3] here: on a mismatch this may belong to a
        // different pending-confirmation kind (see the genlock pending
        // marker below) still waiting to be read this boot.
#endif
        return false;
    }
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    watchdog_hw->scratch[3] = 0;
#endif
    if (previous_mode) {
        *previous_mode = (video_pipeline_reboot_mode_t)mode;
    }
    return true;
}

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Genlock-change safety net, mirroring the resolution-change one above but
// for a single on/off bit instead of a 3-way mode. Reuses the same watchdog
// scratch[3] register (the two kinds are never in flight at once: only one
// user action triggers one pending reboot at a time) tagged with a distinct
// magic, so no bit-packing/check-nibble is needed for a single boolean.
#define GENLOCK_PENDING_MAGIC_OFF 0x4e504730U // "NPG0": revert-to-OFF pending
#define GENLOCK_PENDING_MAGIC_ON 0x4e504731U  // "NPG1": revert-to-ON pending

// Reboot into the CURRENT resolution (unchanged) with the NEW genlock
// setting active, flagged PENDING confirmation, carrying `previous_enabled`
// (the revert-to value) across the reboot. Does NOT persist to flash.
void VIDEO_PIPELINE_REBOOT_REQUEST_RAM(video_pipeline_request_reboot_genlock_pending)(bool new_enabled,
                                                                                      bool previous_enabled)
{
    watchdog_hw->scratch[0] = REBOOT_MODE_BOOT_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)reboot_requested_mode;
    watchdog_hw->scratch[2] = reboot_mode_boot_check((uint32_t)reboot_requested_mode);
    watchdog_hw->scratch[3] = previous_enabled ? GENLOCK_PENDING_MAGIC_ON : GENLOCK_PENDING_MAGIC_OFF;
    (void)new_enabled; // applied on the next boot by reading the persisted setting, like resolution
    __dmb();
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

bool video_pipeline_take_genlock_pending_confirmation(bool *previous_enabled)
{
    const uint32_t packed = watchdog_hw->scratch[3];
    if (packed != GENLOCK_PENDING_MAGIC_OFF && packed != GENLOCK_PENDING_MAGIC_ON) {
        return false; // may belong to a pending resolution confirmation instead
    }
    watchdog_hw->scratch[3] = 0;
    if (previous_enabled) {
        *previous_enabled = (packed == GENLOCK_PENDING_MAGIC_ON);
    }
    return true;
}
#endif

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

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Genlock on/off is a flash-persisted setting (default off), applied at boot
// like resolution -- NOT live-toggled: at 240p the genlock raster differs in
// h_total (video_mode_240_p_genlock vs video_mode_240_p), which is baked
// into command lists at mode apply, so a live toggle would need mid-stream
// rebuilds. main() latches the boot-time setting here, once, before Core 1
// launch, so the vsync callback's gate is a single load.
static bool g_genlock_enabled;

void video_pipeline_set_genlock_enabled(bool enabled)
{
    g_genlock_enabled = enabled;
}

bool video_pipeline_genlock_enabled(void)
{
    return g_genlock_enabled;
}
#endif

/**
 * Fast 2x pixel doubling: reads 2 pixels, writes 2 doubled words.
 * Processes 32-bits at a time for efficiency.
 */
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *restrict dst, const uint16_t *restrict src, int count)
{
#if NEOPICO_EXP_RGB888_SCANOUT
    // One 32-bit RGB888 word per physical output pixel: each source pixel is
    // doubled into 2 consecutive words (was: doubled into 1 packed word).
    const uint32_t *src32 = (const uint32_t *)src;
    // 50% scanlines: g_scanline_dim_line (a single bool, set once per line
    // by the callback right next to the shadow latch) selects which table
    // to read this call, instead of the literal &g_effect_lut888. The
    // pointer ternary lives HERE (scratch_y, plenty of headroom) rather than
    // in the callback (scratch_x, none to spare) -- see the comment on
    // g_scanline_dim_line. This is the ONLY scale function that ever needs
    // the dim table -- triple/quadruple (720p/240p) are untouched, since
    // g_scanline_dim_line is always false for those modes (see the
    // callback) and 240p/720p have their own tight-budget history. One
    // pointer select here, at function entry, not per pixel.
    const mvs_effect_lut888_t *lut = g_scanline_dim_line ? &g_effect_lut888_dim : &g_effect_lut888;
    int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        uint32_t pair = src32[i];
        uint32_t c0 = mvs_effect_lut888_lookup_entropy(lut, pair & 0xFFFFU, g_scanline_shadow);
        uint32_t c1 = mvs_effect_lut888_lookup_entropy(lut, pair >> 16U, g_scanline_shadow);
        dst[(i * 4) + 0] = c0;
        dst[(i * 4) + 1] = c0;
        dst[(i * 4) + 2] = c1;
        dst[(i * 4) + 3] = c1;
    }
#else
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
#endif
}

/**
 * Fast 3x pixel scaling: reads 2 pixels, writes 3 doubled words (6 output pixels).
 * For 720p 4:3 mode (960 output pixels from 320 source pixels, centered).
 */
void __scratch_y("") video_pipeline_triple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
#if NEOPICO_EXP_RGB888_SCANOUT
    // One 32-bit RGB888 word per physical output pixel: each source pixel is
    // tripled into 3 consecutive words (was: tripled across 3 packed words).
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count >> 1;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t c0 = mvs_effect_lut888_lookup_entropy(&g_effect_lut888, two & 0xFFFFU, g_scanline_shadow);
        uint32_t c1 = mvs_effect_lut888_lookup_entropy(&g_effect_lut888, two >> 16U, g_scanline_shadow);
        dst[(i * 6) + 0] = c0;
        dst[(i * 6) + 1] = c0;
        dst[(i * 6) + 2] = c0;
        dst[(i * 6) + 3] = c1;
        dst[(i * 6) + 4] = c1;
        dst[(i * 6) + 5] = c1;
    }
#else
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
#endif
}

/**
 * Fast 4x pixel quadrupling: reads 2 pixels, writes 4 doubled words (8 output pixels).
 * For 240p direct mode (1280 output pixels from 320 source pixels).
 */
void __scratch_y("") video_pipeline_quadruple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
#if NEOPICO_EXP_RGB888_SCANOUT
    // One 32-bit RGB888 word per physical output pixel: each source pixel is
    // quadrupled into 4 consecutive words (was: quadrupled across 4 packed words).
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t c0 = mvs_effect_lut888_lookup_entropy(&g_effect_lut888, two & 0xFFFFU, g_scanline_shadow);
        uint32_t c1 = mvs_effect_lut888_lookup_entropy(&g_effect_lut888, two >> 16U, g_scanline_shadow);
        dst[(i * 8) + 0] = c0;
        dst[(i * 8) + 1] = c0;
        dst[(i * 8) + 2] = c0;
        dst[(i * 8) + 3] = c0;
        dst[(i * 8) + 4] = c1;
        dst[(i * 8) + 5] = c1;
        dst[(i * 8) + 6] = c1;
        dst[(i * 8) + 7] = c1;
    }
#else
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
#endif
}

// See video_pipeline.h for the level values/formulas and call-site
// requirements. Regenerating g_effect_lut888_dim is ~4224 entries (16896
// bytes), cheap on Core 1's background loop but far too slow for any ISR.
void video_pipeline_set_scanline_level(uint8_t level)
{
    if (level > VIDEO_PIPELINE_SCANLINE_100) {
        level = VIDEO_PIPELINE_SCANLINE_100;
    }
#if NEOPICO_EXP_RGB888_SCANOUT
    if (level != g_scanline_level) {
        if (level == VIDEO_PIPELINE_SCANLINE_OFF) {
            // Disabling: g_effect_lut888_dim's content is irrelevant once
            // selection is forced off by the level check in
            // video_pipeline_set_scanline_dim_line() -- no regen needed.
            g_scanline_level = VIDEO_PIPELINE_SCANLINE_OFF;
        } else {
            // RACE SAFETY: video_pipeline_set_scanline_dim_line() runs once
            // per line from the scratch_x callback, which can preempt this
            // Core 1 background call at any point during the regen loop
            // below. Forcing OFF FIRST makes every line touched during the
            // regen resolve to the normal (non-dim) LUT, so the ISR can
            // never read a torn g_effect_lut888_dim (part old level, part
            // new level). Flip to the real level only once the table is
            // fully consistent. A frame or so without scanlines during a
            // level change is expected and fine.
            g_scanline_level = VIDEO_PIPELINE_SCANLINE_OFF;
            video_pipeline_dim_lut888_generate(level);
            g_scanline_level = level;
        }
    }
#else
    // No 480p LUT path in this build, but the level is still the value the OSD
    // reads back, so it must track regardless.
    g_scanline_level = level;
#endif
    // 720p: pico_hdmi's own runtime setter, same 0..4 level values. Called
    // unconditionally so the menu never has to know whether the 480p LUT
    // path above actually did anything (e.g. NEOPICO_EXP_RGB888_SCANOUT
    // off).
    video_output_set_scanline_level(level);
}

uint8_t video_pipeline_get_scanline_level(void)
{
    return g_scanline_level;
}

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Nominals that approximate MVS ~59.18 Hz at each mode's pixel clock:
//   480p: 25.2M / (800 * 532) = 59.21 Hz    (±1 → 59.10–59.32 Hz)
//   240p: 25.2M / (1613 * 264) = 59.178 Hz  (±1 → 58.95–59.40 Hz)
//   (240p base raster retimed to 1613x264 -- see video_mode_240_p in
//   video_output_rt.c -- so the nominal here tracks that mode's own
//   v_total_lines/h_total_pixels rather than the old 1600x262 raster.)
#define GENLOCK_NOMINAL_VTOTAL_480 532
#define GENLOCK_NOMINAL_VTOTAL_240 264
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
static volatile uint32_t g_genlock_phase_us;      // published for the genlock OSD
static volatile uint32_t g_genlock_outzone_count; // raw out-of-zone frames (incl. measurement spikes)

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
#if NEOPICO_EXP_GENLOCK_DYNAMIC && !defined(NEOPICO_DIAG_GENLOCK_SERVO_OFF)
    // Default OFF (opt-in via OSD): g_genlock_enabled is latched once at
    // boot (see video_pipeline_set_genlock_enabled()), so this is one load.
    // When off, rt_v_total_lines/htrim are never written and every mode
    // stays at its nominal (standard) rate.
    if (g_genlock_enabled) {
        genlock_dynamic_update();
    }
#endif
    osd_visible_latched = osd_visible;
}

#if NEOPICO_EXP_SCANLINE_TRACE
// The implementation has several early returns (e.g. the 3x path skips two of
// every three lines), so timing is done by a wrapper rather than by threading
// a record through every exit. Skipped lines then show up as near-zero
// samples, which is itself diagnostic.
static void __scratch_x("000_video_pipeline_modes")
    video_pipeline_scanline_callback_impl(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);

static void __scratch_x("000_video_pipeline_modes")
    video_pipeline_scanline_callback_reboot_modes(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    static bool trace_ready;
    if (!trace_ready) {
        // DWT is per-core and Core 1 enters through the library's core1 entry
        // point, so there is no firmware-side init hook: arm it on first use.
        scanline_trace_init();
        trace_ready = true;
    }
    const uint32_t t0 = TRACE_DWT_CYCCNT;
    video_pipeline_scanline_callback_impl(v_scanline, active_line, dst);
    const uint32_t elapsed = TRACE_DWT_CYCCNT - t0;
    g_scanline_trace[g_scanline_trace_idx & (SCANLINE_TRACE_ENTRIES - 1U)] =
        (elapsed > 0xFFFFU) ? 0xFFFFU : (uint16_t)elapsed;
    g_scanline_trace_idx++;
}

static void __scratch_x("000_video_pipeline_modes")
    video_pipeline_scanline_callback_impl(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
#else
static void __scratch_x("000_video_pipeline_modes")
    video_pipeline_scanline_callback_reboot_modes(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
#endif
    (void)v_scanline;

    const uint32_t active_width = video_output_active_mode->h_active_pixels;
    const uint32_t active_height = video_output_active_mode->v_active_lines;
    const bool mode_is_240p = active_width == 1280U && active_height == 240U;
    const bool mode_is_3x = active_width == 1280U && active_height == 720U;
    // "words" below means 32-bit output words: 2 packed RGB565 pixels/word
    // normally, or 1 RGB888 pixel/word under the feasibility spike.
    const uint32_t h_words =
#if NEOPICO_EXP_RGB888_SCANOUT
        active_width;
#else
        active_width / 2U;
#endif
    const uint32_t h_scale = mode_is_3x ? 3U : mode_is_240p ? 4U : 2U;
#if NEOPICO_EXP_RGB888_SCANOUT
    // 50% scanlines: unlike g_scanline_shadow (which depends on captured
    // per-line data and can only be set once a ready line is found),
    // g_scanline_dim_line depends only on h_scale/active_line, both already
    // known here -- so it is safe, and measured cheapest, to set it
    // unconditionally once, up front, rather than duplicated at the two
    // g_scanline_shadow assignment sites further down (see
    // video_pipeline_set_scanline_dim_line's comment for the numbers).
    video_pipeline_set_scanline_dim_line(h_scale, active_line);
#endif
    const uint32_t image_words =
#if NEOPICO_EXP_RGB888_SCANOUT
        LINE_WIDTH * h_scale;
#else
        (LINE_WIDTH * h_scale) / 2U;
#endif
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
    const uint32_t osd_x_words = x_margin_words +
#if NEOPICO_EXP_RGB888_SCANOUT
                                 ((uint32_t)OSD_BOX_X * h_scale);
#else
                                 (((uint32_t)OSD_BOX_X * h_scale) / 2U);
#endif
    const uint32_t osd_w_words =
#if NEOPICO_EXP_RGB888_SCANOUT
        (uint32_t)OSD_BOX_W * h_scale;
#else
        (((uint32_t)OSD_BOX_W * h_scale) / 2U);
#endif
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
        VIDEO_PIPELINE_FILL(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, test_pattern_line, LINE_WIDTH);
        VIDEO_PIPELINE_FILL(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
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
            VIDEO_PIPELINE_FILL(dst, h_words, OVERSCAN_COLOR_RGB565);
            return;
        }

        const uint16_t mvs_line = (uint16_t)mvs_line_u32;
        const uint16_t *src = NULL;
        if (line_ring_ready(mvs_line)) {
            src = line_ring_read_ptr(mvs_line);
#if NEOPICO_EXP_RGB888_SCANOUT
            g_scanline_shadow = line_ring_read_shadow(mvs_line);
#endif
        }
        if (!src) {
            VIDEO_PIPELINE_FILL(dst, h_words, NO_SIGNAL_COLOR_RGB565);
            return;
        }
        VIDEO_PIPELINE_FILL(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, LINE_WIDTH);
        VIDEO_PIPELINE_FILL(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
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
#if NEOPICO_EXP_RGB888_SCANOUT
            g_scanline_shadow = line_ring_read_shadow(mvs_line);
#endif
        }
    }

    const uint16_t *osd_src = osd_framebuffer[osd_line_u32];
    if (!src) {
        // No capture source: render OSD over fallback color without double-writing the OSD span.
        VIDEO_PIPELINE_FILL(dst, osd_x_words, NO_SIGNAL_COLOR_RGB565);
        VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words, osd_src, OSD_BOX_W);
        VIDEO_PIPELINE_FILL(dst + osd_x_words + osd_w_words, h_words - osd_x_words - osd_w_words,
                            NO_SIGNAL_COLOR_RGB565);
        return;
    }

    // Before OSD
    VIDEO_PIPELINE_FILL(dst, x_margin_words, OVERSCAN_COLOR_RGB565);
    VIDEO_PIPELINE_SCALE_SELECTED(dst + x_margin_words, src, OSD_BOX_X);
    // OSD region: opaque blit by default, or fixed 87.5% black-panel opacity.
    VIDEO_PIPELINE_SCALE_OSD_SELECTED(dst + osd_x_words, src + OSD_BOX_X, osd_src, OSD_BOX_W);
    // After OSD
    VIDEO_PIPELINE_SCALE_SELECTED(dst + osd_x_words + osd_w_words, src + OSD_BOX_X + OSD_BOX_W,
                                  LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    VIDEO_PIPELINE_FILL(dst + x_margin_words + image_words, h_words - x_margin_words - image_words,
                        OVERSCAN_COLOR_RGB565);
#undef VIDEO_PIPELINE_SCALE_OSD_SELECTED
#undef VIDEO_PIPELINE_SCALE_SELECTED
}

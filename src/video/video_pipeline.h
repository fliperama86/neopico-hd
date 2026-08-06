#ifndef VIDEO_PIPELINE_H
#define VIDEO_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "pico.h"

// Video effect toggles
extern bool fx_scanlines_enabled;

/**
 * Initialize the video pipeline.
 * Sets up HDMI output and registers scanline/vsync callbacks.
 *
 * @param frame_width Output frame width (e.g., 640)
 * @param frame_height Output frame height (e.g., 480)
 */
void video_pipeline_init(uint32_t frame_width, uint32_t frame_height);

/**
 * Fast 2x pixel doubling without any effect.
 * Processes 32-bits (2 pixels) at a time for efficiency.
 */
void __scratch_y("") video_pipeline_double_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

/**
 * Fast 3x pixel scaling for 720p 4:3 pillarboxed mode.
 * Two source pixels produce six output pixels (three uint32_t words).
 */
void __scratch_y("") video_pipeline_triple_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

/**
 * Fast 4x pixel quadrupling for 240p direct mode.
 * Each source pixel produces 4 output pixels (2 uint32_t words).
 */
void __scratch_y("") video_pipeline_quadruple_pixels_fast(uint32_t *dst, const uint16_t *src, int count);

typedef enum {
    VIDEO_PIPELINE_REBOOT_MODE_480P = 0,
    VIDEO_PIPELINE_REBOOT_MODE_240P = 1,
    VIDEO_PIPELINE_REBOOT_MODE_720P = 2,
} video_pipeline_reboot_mode_t;

/**
 * Request a reboot-based output mode switch. The request is consumed during the
 * next boot before HDMI output starts.
 */
void video_pipeline_request_reboot_mode(video_pipeline_reboot_mode_t mode);
video_pipeline_reboot_mode_t video_pipeline_reboot_requested_mode(void);
bool video_pipeline_reboot_mode_available(uint8_t mode);
bool video_pipeline_take_reboot_mode_boot_request(video_pipeline_reboot_mode_t *mode);

// Resolution-change safety net: reboot into `mode` flagged PENDING confirmation,
// carrying `previous` (the revert-to mode) across the reboot. Not persisted to
// flash. take_pending_confirmation() consumes the marker at boot (returns true +
// the previous mode if this boot is awaiting confirmation).
void video_pipeline_request_reboot_mode_pending(video_pipeline_reboot_mode_t mode,
                                                video_pipeline_reboot_mode_t previous);
bool video_pipeline_take_pending_confirmation(video_pipeline_reboot_mode_t *previous_mode);

void video_pipeline_request_reboot_240p(bool enabled);
bool video_pipeline_reboot_requested_240p(void);
bool video_pipeline_take_reboot_240p_boot_request(bool *enabled);

/**
 * VSYNC callback - called once per frame to sync input/output buffers.
 *
 * Placement: scratch_x content plus the 2 KiB core-1 stack fill the 4 KiB
 * bank exactly, so the genlock build's extra call cannot live there; with
 * genlock ON the callback moves to scratch_y (over 1 KiB of headroom).
 */
#ifndef NEOPICO_EXP_GENLOCK_DYNAMIC
#define NEOPICO_EXP_GENLOCK_DYNAMIC 0
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Genlock on/off: a flash-persisted setting (default off), applied at boot
// like resolution -- not live-toggled (see video_pipeline.c). Call
// video_pipeline_set_genlock_enabled() once at boot, before Core 1 launch.
void video_pipeline_set_genlock_enabled(bool enabled);
bool video_pipeline_genlock_enabled(void);

// Genlock-change safety net, mirroring the resolution one above for a single
// on/off bit: reboot with the new setting active (persisted to flash by the
// caller beforehand, mirroring the resolution selector's optimistic-save
// pattern) but flagged PENDING confirmation, carrying `previous_enabled` (the
// revert-to value) across the reboot. Not persisted itself.
void video_pipeline_request_reboot_genlock_pending(bool new_enabled, bool previous_enabled);
bool video_pipeline_take_genlock_pending_confirmation(bool *previous_enabled);
#endif

// Scanline STRENGTH, shared BY NUMBER with pico_hdmi's
// video_output_set_scanline_level() (720p path, lib/pico_hdmi): a caller
// passes the same uint8_t 0..4 to both, and both apply the same per-channel
// factors below. A level is nonetheless somewhat DARKER at 720p, because the
// geometry differs: this path dims one row of a pair (average (1 + d)/2)
// while 720p dims the outer two rows of a triple, to keep the bright row
// centred on the source line, averaging (1 + 2d)/3. See video_output_rt.c
// for why equalizing them is blocked on the dim pass's measured cost budget.
//
// Per-8-bit-channel dim formulas, applied exactly as written in
// video_pipeline.c and (on both dimmed rows) in video_output_rt.c:
//   OFF  -> v                            (100% brightness, no dimming)
//   25%  -> (v + (v >> 1)) >> 1          (75% brightness)
//   50%  -> v >> 1                       (50% brightness)
//   75%  -> v >> 2                       (25% brightness)
//   100% -> 0                            (fully black)
typedef enum {
    VIDEO_PIPELINE_SCANLINE_OFF = 0,
    VIDEO_PIPELINE_SCANLINE_25 = 1,
    VIDEO_PIPELINE_SCANLINE_50 = 2,
    VIDEO_PIPELINE_SCANLINE_75 = 3,
    VIDEO_PIPELINE_SCANLINE_100 = 4,
} video_pipeline_scanline_level_t;

// LIVE, not staged (see menu_diag_experiment.c's Video-screen comment):
// applies immediately. Safe to call from the Core 1 background/menu context;
// never from the scanline ISR. Also forwards to pico_hdmi's
// video_output_set_scanline_level() for 720p, so callers (the OSD menu) only
// ever need this one function -- matching every other video_pipeline_*
// wrapper in this header, none of which touch pico_hdmi directly.
void video_pipeline_set_scanline_level(uint8_t level);

// Current level. The pipeline owns this value (boot restores it from flash
// before Core 1 launches), so the OSD must read it back rather than assume a
// default, otherwise the menu shows a stale value after a reboot.
uint8_t video_pipeline_get_scanline_level(void);

#if NEOPICO_EXP_GENLOCK_DYNAMIC
#define VIDEO_PIPELINE_VSYNC_RAM __scratch_y("genlock_vsync")
#else
#define VIDEO_PIPELINE_VSYNC_RAM __scratch_x("")
#endif
void VIDEO_PIPELINE_VSYNC_RAM video_pipeline_vsync_callback(void);

#endif // VIDEO_PIPELINE_H

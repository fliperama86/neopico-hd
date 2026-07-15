#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include "pico/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef NEOPICO_MVS_COLOR_MODEL_MENU
#define NEOPICO_MVS_COLOR_MODEL_MENU 0
#endif

#if NEOPICO_MVS_COLOR_MODEL_MENU
#include "mvs_color_model.h"
#endif

/**
 * Initialize target-specific video capture.
 *
 * @param active_height Active video lines from the selected capture target.
 */
void video_capture_init(uint active_height);

/**
 * Run the video capture loop (never returns)
 * Captures lines into the global line ring buffer
 * Signals VSYNC to Core 1 at frame boundaries
 */
void video_capture_run(void);

/**
 * Get current frame count
 */
uint32_t video_capture_get_frame_count(void);

#if NEOPICO_MVS_COLOR_MODEL_MENU
// Request a color model. Before capture starts this establishes the initial
// model; while running, Core 0 applies it atomically at the next input VSYNC.
void video_capture_set_color_model(mvs_color_model_t model);
mvs_color_model_t video_capture_get_color_model(void);
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC
/**
 * Timestamp (timer_hw->timerawl) of the most recent MVS VSYNC, written by Core 0.
 */
extern volatile uint32_t g_mvs_vsync_timestamp;
#endif

#endif // VIDEO_CAPTURE_H

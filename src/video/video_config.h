#ifndef VIDEO_CONFIG_H
#define VIDEO_CONFIG_H

#include "capture_profile.h"

// Framebuffer dimensions (DVI output resolution, 2x scaled to 640x480)
#define FRAME_WIDTH CAPTURE_FRAME_WIDTH
#define FRAME_HEIGHT CAPTURE_FRAME_HEIGHT

// Active source video area. Keep MVS_HEIGHT as a compatibility alias used by
// the existing shared output pipeline.
#define SOURCE_HEIGHT CAPTURE_ACTIVE_HEIGHT
#define MVS_HEIGHT SOURCE_HEIGHT

// Vertical offset for centering source video in framebuffer
#define V_OFFSET ((FRAME_HEIGHT - MVS_HEIGHT) / 2)

#endif // VIDEO_CONFIG_H

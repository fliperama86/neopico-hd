/**
 * Video Subsystem Configuration
 * Shared settings for video capture and output
 */

#ifndef VIDEO_CONFIG_H
#define VIDEO_CONFIG_H

// =============================================================================
// Video Frame Configuration
// =============================================================================

// Framebuffer dimensions (DVI output resolution)
#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240  // 480p: 240 * 2 = 480 DVI lines (line doubling)

// MVS active video area
#define MVS_HEIGHT 224    // Neo Geo MVS vertical resolution

// Vertical offset for centering MVS in framebuffer
#define V_OFFSET ((FRAME_HEIGHT - MVS_HEIGHT) / 2)  // 8 lines top/bottom border

#endif // VIDEO_CONFIG_H

#ifndef VIDEO_CONFIG_H
#define VIDEO_CONFIG_H

// Framebuffer dimensions (DVI output resolution, 2x scaled to 640x480)
#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240

// MVS active video area
#define MVS_HEIGHT 224

// Vertical offset for centering MVS in framebuffer
#define V_OFFSET ((FRAME_HEIGHT - MVS_HEIGHT) / 2)

#endif // VIDEO_CONFIG_H

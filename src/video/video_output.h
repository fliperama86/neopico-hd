#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include <stdint.h>
#include "pico/types.h"

/**
 * Initialize DVI video output
 *
 * @param framebuffer Pointer to RGB565 framebuffer
 * @param frame_width Width of framebuffer (e.g., 320)
 * @param frame_height Height of framebuffer (e.g., 240)
 */
void video_output_init(uint16_t *framebuffer, uint frame_width, uint frame_height);

/**
 * Start DVI output on Core 1
 * Must be called after video_output_init()
 */
void video_output_start(void);

/**
 * Get pointer to DVI instance (for audio integration)
 */
struct dvi_inst* video_output_get_dvi(void);

#endif // VIDEO_OUTPUT_H

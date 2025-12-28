#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include "pico/types.h"
#include "audio_common.h"

// Forward declaration
struct dvi_inst;

/**
 * Initialize HDMI audio output
 *
 * @param dvi DVI instance to output audio to
 * @param sample_rate Output sample rate (typically 48000)
 */
void audio_output_init(struct dvi_inst *dvi, uint32_t sample_rate);

/**
 * Write audio samples to HDMI output
 * Called by audio pipeline to output processed samples
 *
 * @param samples Processed audio samples to output
 * @param count Number of samples
 */
void audio_output_write(const ap_sample_t *samples, uint32_t count);

#endif // AUDIO_OUTPUT_H

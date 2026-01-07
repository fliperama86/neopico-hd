/**
 * Audio Pipeline - Lowpass Filter
 *
 * Simple 2-pole IIR (biquad) lowpass filter for anti-aliasing before SRC.
 * Cutoff ~20kHz at 49kHz sample rate.
 */

#ifndef LOWPASS_H
#define LOWPASS_H

#include "audio_common.h"

// Biquad filter state (per channel)
typedef struct {
    int32_t x1, x2;  // Previous inputs (Q16 fixed-point)
    int32_t y1, y2;  // Previous outputs (Q16 fixed-point)
} lowpass_channel_t;

typedef struct {
    lowpass_channel_t left;
    lowpass_channel_t right;
    bool enabled;
} lowpass_t;

// Initialize filter
void lowpass_init(lowpass_t *lp);

// Enable/disable
void lowpass_set_enabled(lowpass_t *lp, bool enabled);

// Process buffer in-place
void lowpass_process_buffer(lowpass_t *lp, audio_sample_t *samples, uint32_t count);

#endif // LOWPASS_H

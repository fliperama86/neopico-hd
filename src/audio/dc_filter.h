/**
 * Audio Pipeline - DC Blocking Filter
 *
 * High-pass IIR filter to remove DC offset from audio.
 * y[n] = x[n] - x[n-1] + alpha * y[n-1]
 *
 * Cutoff frequency ~10Hz at 55kHz sample rate with alpha=0.9995
 */

#ifndef DC_FILTER_H
#define DC_FILTER_H

#include "audio_common.h"

// DC filter state (per channel)
typedef struct {
    int32_t prev_in;
    int32_t prev_out;
} dc_filter_channel_t;

// DC filter instance (stereo)
typedef struct {
    dc_filter_channel_t left;
    dc_filter_channel_t right;
    bool enabled;
} dc_filter_t;

// Initialize DC filter
void dc_filter_init(dc_filter_t *f);

// Enable/disable filter
void dc_filter_set_enabled(dc_filter_t *f, bool enabled);

// Toggle filter state, returns new state
bool dc_filter_toggle(dc_filter_t *f);

// Process one sample (in-place)
void dc_filter_process(dc_filter_t *f, audio_sample_t *sample);

// Process buffer of samples (in-place)
void dc_filter_process_buffer(dc_filter_t *f, audio_sample_t *samples, uint32_t count);

#endif // DC_FILTER_H

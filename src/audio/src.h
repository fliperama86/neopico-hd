/**
 * Audio Pipeline - Sample Rate Conversion
 *
 * Converts from input sample rate (~55.5kHz) to output rate (48kHz).
 * Multiple algorithms available, selectable at runtime.
 */

#ifndef SRC_H
#define SRC_H

#include "audio_common.h"

// Default rates
// Neo Geo MVS (MV1C) outputs at 8MHz / 144 = 55,555.555... Hz
// Using 55556 minimizes the rounding error (0.44 Hz vs 0.56 Hz with 55555)
#define SRC_INPUT_RATE_DEFAULT 55556
#define SRC_OUTPUT_RATE_DEFAULT 48000

// SRC instance
typedef struct
{
    src_mode_t mode;
    uint32_t input_rate;
    uint32_t output_rate;

    // Internal state for algorithms
    uint32_t accumulator;    // For DROP mode (bresenham)
    uint32_t phase;          // For LINEAR mode (fixed-point position)
    audio_sample_t prev_sample; // For LINEAR mode (interpolation)
    bool have_prev;          // LINEAR mode: do we have a previous sample?
} src_t;

// Initialize SRC
void src_init(src_t *s, uint32_t input_rate, uint32_t output_rate);

// Set mode
void src_set_mode(src_t *s, src_mode_t mode);

// Cycle to next mode, returns new mode
src_mode_t src_cycle_mode(src_t *s);

// Get current mode
static inline src_mode_t src_get_mode(src_t *s)
{
    return s->mode;
}

// Process samples
// Input: buffer of samples at input rate
// Output: buffer for samples at output rate
// Returns: number of output samples written
// Updates in_consumed with number of input samples consumed
uint32_t src_process(src_t *s,
                     const audio_sample_t *in, uint32_t in_count,
                     audio_sample_t *out, uint32_t out_max,
                     uint32_t *in_consumed);

#endif // SRC_H

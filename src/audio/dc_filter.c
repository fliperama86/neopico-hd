/**
 * DC Blocking Filter Implementation
 *
 * Simple high-pass IIR filter: y[n] = x[n] - x[n-1] + alpha * y[n-1]
 * Removes DC offset while passing audio frequencies.
 */

#include "dc_filter.h"

// Alpha coefficient in fixed-point (Q16)
// alpha = 0.9995 gives ~10Hz cutoff at 55kHz
// 0.9995 * 65536 = 65503
#define DC_ALPHA 65503

void dc_filter_init(dc_filter_t *f) {
    f->left.prev_in = 0;
    f->left.prev_out = 0;
    f->right.prev_in = 0;
    f->right.prev_out = 0;
    f->enabled = false;
}

void dc_filter_set_enabled(dc_filter_t *f, bool enabled) {
    f->enabled = enabled;
    if (!enabled) {
        // Reset state when disabled
        f->left.prev_in = 0;
        f->left.prev_out = 0;
        f->right.prev_in = 0;
        f->right.prev_out = 0;
    }
}

bool dc_filter_toggle(dc_filter_t *f) {
    dc_filter_set_enabled(f, !f->enabled);
    return f->enabled;
}

// Process single channel
static inline int16_t dc_filter_process_channel(dc_filter_channel_t *ch, int16_t in) {
    // y[n] = x[n] - x[n-1] + alpha * y[n-1]
    // Using Q16 fixed-point for alpha
    int32_t out = (int32_t)in - ch->prev_in + ((ch->prev_out * DC_ALPHA) >> 16);

    // Clamp to int16 range
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;

    ch->prev_in = in;
    ch->prev_out = out;

    return (int16_t)out;
}

void dc_filter_process(dc_filter_t *f, ap_sample_t *sample) {
    if (!f->enabled) return;

    sample->left = dc_filter_process_channel(&f->left, sample->left);
    sample->right = dc_filter_process_channel(&f->right, sample->right);
}

void dc_filter_process_buffer(dc_filter_t *f, ap_sample_t *samples, uint32_t count) {
    if (!f->enabled) return;

    for (uint32_t i = 0; i < count; i++) {
        dc_filter_process(f, &samples[i]);
    }
}

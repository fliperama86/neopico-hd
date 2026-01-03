/**
 * Lowpass Filter Implementation
 *
 * 2-pole Butterworth lowpass (biquad) at ~20kHz cutoff for 49kHz sample rate.
 * Uses Q16 fixed-point arithmetic for efficiency on RP2350.
 *
 * Transfer function: H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
 *
 * Coefficients calculated for:
 * - Sample rate: 49140 Hz
 * - Cutoff: 20000 Hz
 * - Type: Butterworth lowpass
 */

#include "lowpass.h"

// Biquad coefficients in Q16 fixed-point
// For Butterworth LPF at fc=20kHz, fs=49.14kHz:
// Using bilinear transform with frequency pre-warping
// These give gentle rolloff above 20kHz
#define B0  18552   // ~0.283 * 65536
#define B1  37104   // ~0.566 * 65536
#define B2  18552   // ~0.283 * 65536
#define A1  -7864   // ~-0.120 * 65536
#define A2  17520   // ~0.267 * 65536

void lowpass_init(lowpass_t *lp) {
    lp->left.x1 = 0;
    lp->left.x2 = 0;
    lp->left.y1 = 0;
    lp->left.y2 = 0;
    lp->right.x1 = 0;
    lp->right.x2 = 0;
    lp->right.y1 = 0;
    lp->right.y2 = 0;
    lp->enabled = true;  // On by default for anti-aliasing
}

void lowpass_set_enabled(lowpass_t *lp, bool enabled) {
    lp->enabled = enabled;
    if (!enabled) {
        // Reset state
        lp->left.x1 = lp->left.x2 = 0;
        lp->left.y1 = lp->left.y2 = 0;
        lp->right.x1 = lp->right.x2 = 0;
        lp->right.y1 = lp->right.y2 = 0;
    }
}

// Process single sample through biquad
// y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
// Optimized: b1 = 2*b0, b2 = b0. So FIR = b0 * (x[n] + 2*x[n-1] + x[n-2])
static inline int16_t lowpass_process_sample(lowpass_channel_t *ch, int16_t in) {
    // FIR Part (optimized)
    // Sum inputs: x[n] + 2*x[n-1] + x[n-2]
    // Max value: 32767 * 4 = 131068, fits in int32
    int32_t sum_x = (int32_t)in + ((int32_t)ch->x1 << 1) + ch->x2;
    
    // Compute output (accumulator in Q16)
    int64_t acc = (int64_t)B0 * sum_x;
    
    // IIR Part
    acc -= (int64_t)A1 * (ch->y1 >> 16);
    acc -= (int64_t)A2 * (ch->y2 >> 16);

    // Result is in Q16
    int32_t y0 = (int32_t)acc;

    // Update state
    ch->x2 = ch->x1;
    ch->x1 = in;
    ch->y2 = ch->y1;
    ch->y1 = y0;

    // Convert back to int16
    int32_t out = y0 >> 16;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;

    return (int16_t)out;
}

void lowpass_process_buffer(lowpass_t *lp, ap_sample_t *samples, uint32_t count) {
    if (!lp->enabled) return;

    for (uint32_t i = 0; i < count; i++) {
        samples[i].left = lowpass_process_sample(&lp->left, samples[i].left);
        samples[i].right = lowpass_process_sample(&lp->right, samples[i].right);
    }
}

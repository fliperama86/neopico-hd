/**
 * Sample Rate Conversion Implementation
 *
 * Three modes:
 * - NONE: Passthrough (plays fast, ~15% at 55.5kHz->48kHz)
 * - DROP: Bresenham-style sample dropping (minimal CPU)
 * - LINEAR: Linear interpolation (better quality)
 */

#include "src.h"

void src_init(src_t *s, uint32_t input_rate, uint32_t output_rate) {
    s->mode = SRC_MODE_DROP;  // Default to DROP (good balance)
    s->input_rate = input_rate;
    s->output_rate = output_rate;
    s->accumulator = 0;
    s->phase = 0;
    s->prev_sample.left = 0;
    s->prev_sample.right = 0;
    s->have_prev = false;
}

void src_set_mode(src_t *s, src_mode_t mode) {
    if (mode >= SRC_MODE_COUNT) {
        mode = SRC_MODE_NONE;
    }
    s->mode = mode;

    // Reset state on mode change
    s->accumulator = 0;
    s->phase = 0;
    s->have_prev = false;
}

src_mode_t src_cycle_mode(src_t *s) {
    src_mode_t new_mode = (src_mode_t)((s->mode + 1) % SRC_MODE_COUNT);
    src_set_mode(s, new_mode);
    return new_mode;
}

// NONE mode: direct passthrough
static uint32_t src_process_none(src_t *s,
                                  const ap_sample_t *in, uint32_t in_count,
                                  ap_sample_t *out, uint32_t out_max,
                                  uint32_t *in_consumed) {
    uint32_t count = (in_count < out_max) ? in_count : out_max;
    for (uint32_t i = 0; i < count; i++) {
        out[i] = in[i];
    }
    *in_consumed = count;
    return count;
}

// DROP mode: Bresenham-style sample rate conversion
// Only output a sample when accumulator overflows
static uint32_t src_process_drop(src_t *s,
                                  const ap_sample_t *in, uint32_t in_count,
                                  ap_sample_t *out, uint32_t out_max,
                                  uint32_t *in_consumed) {
    uint32_t out_count = 0;
    uint32_t consumed = 0;

    for (uint32_t i = 0; i < in_count && out_count < out_max; i++) {
        consumed++;

        // Bresenham: accumulate output_rate, emit when >= input_rate
        s->accumulator += s->output_rate;
        if (s->accumulator >= s->input_rate) {
            s->accumulator -= s->input_rate;
            out[out_count++] = in[i];
        }
    }

    *in_consumed = consumed;
    return out_count;
}

// LINEAR mode: Linear interpolation between samples
// Uses fixed-point phase accumulator (16.16 format)
static uint32_t src_process_linear(src_t *s,
                                    const ap_sample_t *in, uint32_t in_count,
                                    ap_sample_t *out, uint32_t out_max,
                                    uint32_t *in_consumed) {
    if (in_count == 0) {
        *in_consumed = 0;
        return 0;
    }

    // Phase increment per output sample (16.16 fixed-point)
    // ratio = input_rate / output_rate
    // For 55555/48000 = 1.157, phase_inc = 0x12828 (~1.157 in 16.16)
    uint32_t phase_inc = ((uint64_t)s->input_rate << 16) / s->output_rate;

    uint32_t out_count = 0;
    uint32_t in_idx = 0;

    // Initialize with first sample if we don't have previous
    if (!s->have_prev && in_count > 0) {
        s->prev_sample = in[0];
        s->have_prev = true;
        in_idx = 1;
    }

    while (out_count < out_max && in_idx <= in_count) {
        // Get integer and fractional parts of phase
        uint32_t int_phase = s->phase >> 16;
        uint32_t frac = s->phase & 0xFFFF;

        // Advance input index based on integer phase
        while (int_phase > 0 && in_idx < in_count) {
            s->prev_sample = in[in_idx++];
            int_phase--;
            s->phase -= 0x10000;
        }

        // If we've run out of input, stop
        if (in_idx >= in_count && int_phase > 0) {
            break;
        }

        // Get next sample for interpolation
        ap_sample_t next;
        if (in_idx < in_count) {
            next = in[in_idx];
        } else {
            // Use previous sample if at end
            next = s->prev_sample;
        }

        // Linear interpolation
        // out = prev + (next - prev) * frac / 65536
        int32_t diff_l = next.left - s->prev_sample.left;
        int32_t diff_r = next.right - s->prev_sample.right;

        out[out_count].left = s->prev_sample.left + ((diff_l * (int32_t)frac) >> 16);
        out[out_count].right = s->prev_sample.right + ((diff_r * (int32_t)frac) >> 16);
        out_count++;

        // Advance phase
        s->phase += phase_inc;
    }

    *in_consumed = in_idx;
    return out_count;
}

uint32_t src_process(src_t *s,
                     const ap_sample_t *in, uint32_t in_count,
                     ap_sample_t *out, uint32_t out_max,
                     uint32_t *in_consumed) {
    switch (s->mode) {
        case SRC_MODE_NONE:
            return src_process_none(s, in, in_count, out, out_max, in_consumed);
        case SRC_MODE_DROP:
            return src_process_drop(s, in, in_count, out, out_max, in_consumed);
        case SRC_MODE_LINEAR:
            return src_process_linear(s, in, in_count, out, out_max, in_consumed);
        default:
            *in_consumed = 0;
            return 0;
    }
}

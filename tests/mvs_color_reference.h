#ifndef NEOPICO_HD_TEST_MVS_COLOR_REFERENCE_H
#define NEOPICO_HD_TEST_MVS_COLOR_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>

// Independent MiSTer reference model pinned to:
// https://github.com/MiSTer-devel/NeoGeo_MiSTer/blob/
// 2325e6c4303dc9a3fd554b18d9833e992ccd444f/neogeo.sv#L2205-L2222
static inline uint8_t mister_reference_channel(uint32_t value5, bool dark, bool shadow)
{
    value5 &= 0x1FU;

    // MiSTer forms a six-bit channel by repeating the RGB555 MSB, then
    // subtracts the palette DARK bit with underflow clamped to zero.
    uint32_t value6 = (value5 << 1U) | (value5 >> 4U);
    if (dark) {
        if (value6 == 0U) {
            value6 = 0U;
        } else {
            value6--;
        }
    }

    // Exact MiSTer concatenation: {value6[5:0], value6[4:3]}.
    uint32_t value8 = (value6 << 2U) | ((value6 >> 3U) & 0x3U);
    if (shadow) {
        value8 >>= 1U;
    }
    return (uint8_t)value8;
}

typedef struct {
    uint8_t channel[4][32];
} mame_reference_t;

// Reproduce MAME's legacy compute_resistor_weights() calculation for one
// five-resistor channel. MAME uses the normal network's scaler for all four
// states and rounds combine_weights() by adding 0.5 before integer conversion.
static inline double mame_reference_compute_weights(int pulldown, double scaler, double weights[5])
{
    static const int resistances[5] = {3900, 2200, 1000, 470, 220};
    double unscaled[5];
    double maximum = 0.0;

    for (unsigned selected = 0; selected < 5U; selected++) {
        double conductance_low = (pulldown == 0) ? (1.0 / 1e12) : (1.0 / pulldown);
        double conductance_high = 1.0 / 1e12;

        for (unsigned bit = 0; bit < 5U; bit++) {
            if (bit == selected) {
                conductance_high += 1.0 / resistances[bit];
            } else {
                conductance_low += 1.0 / resistances[bit];
            }
        }

        const double resistance_low = 1.0 / conductance_low;
        const double resistance_high = 1.0 / conductance_high;
        double output = 255.0 * resistance_low / (resistance_high + resistance_low);
        if (output < 0.0) {
            output = 0.0;
        } else if (output > 255.0) {
            output = 255.0;
        }
        unscaled[selected] = output;
        maximum += output;
    }

    const double selected_scaler = (scaler < 0.0) ? (255.0 / maximum) : scaler;
    for (unsigned bit = 0; bit < 5U; bit++) {
        weights[bit] = unscaled[bit] * selected_scaler;
    }
    return selected_scaler;
}

// Independent MAME resistor-network reference pinned to:
// https://github.com/mamedev/mame/blob/
// e47c0f33c5be3ee286ff65bed13458c2920340d2/src/mame/neogeo/neogeo_v.cpp#L23-L64
static inline void mame_reference_init(mame_reference_t *reference)
{
    double weights[4][5];
    const double scaler = mame_reference_compute_weights(0, -1.0, weights[0]);
    (void)mame_reference_compute_weights(8200, scaler, weights[1]);
    (void)mame_reference_compute_weights(150, scaler, weights[2]);

    // The pinned MAME function accepts an integer pulldown, so the parallel
    // 8200/150-ohm result is truncated to 147 ohms at the call boundary.
    const int combined_pulldown = (int)(1.0 / ((1.0 / 8200.0) + (1.0 / 150.0)));
    (void)mame_reference_compute_weights(combined_pulldown, scaler, weights[3]);

    for (uint32_t state = 0; state < 4U; state++) {
        for (uint32_t value = 0; value < 32U; value++) {
            double output = 0.0;
            for (uint32_t bit = 0; bit < 5U; bit++) {
                if ((value & (1U << bit)) != 0U) {
                    output += weights[state][bit];
                }
            }
            reference->channel[state][value] = (uint8_t)(output + 0.5);
        }
    }
}

static inline uint8_t mame_reference_channel(const mame_reference_t *reference, uint32_t value5, bool dark, bool shadow)
{
    const uint32_t state = (dark ? 1U : 0U) | (shadow ? 2U : 0U);
    return reference->channel[state][value5 & 0x1FU];
}

#endif // NEOPICO_HD_TEST_MVS_COLOR_REFERENCE_H

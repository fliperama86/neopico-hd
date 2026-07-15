#ifndef NEOPICO_HD_MVS_EFFECT_MODEL_H
#define NEOPICO_HD_MVS_EFFECT_MODEL_H

#include <stdint.h>

// Compile exactly one DARK/SHADOW color model into firmware. MiSTer is the
// default digital reference. MAME models the Neo Geo analog resistor network.
#define MVS_EFFECT_MODEL_MISTER 1
#define MVS_EFFECT_MODEL_MAME 2

#ifndef MVS_EFFECT_MODEL
#define MVS_EFFECT_MODEL MVS_EFFECT_MODEL_MISTER
#endif

#define MVS_EFFECT_STATE_SHADOW 0x1U
#define MVS_EFFECT_STATE_DARK 0x2U
#define MVS_EFFECT_STATE_MASK 0x3U

#if MVS_EFFECT_MODEL == MVS_EFFECT_MODEL_MISTER

#define MVS_EFFECT_MODEL_NAME "MiSTer"

// Exact per-channel behavior pinned to MiSTer Neo Geo commit
// 2325e6c4303dc9a3fd554b18d9833e992ccd444f, neogeo.sv lines 2205-2222.
static inline uint8_t mvs_effect_model_channel(uint32_t value5, uint32_t effect_state)
{
    value5 &= 0x1FU;

    // Form six bits by repeating the RGB555 MSB, then subtract DARK with
    // underflow clamped to zero.
    uint32_t value6 = (value5 << 1U) | (value5 >> 4U);
    if ((effect_state & MVS_EFFECT_STATE_DARK) != 0U && value6 != 0U) {
        value6--;
    }

    // Exact MiSTer concatenation: {value6[5:0], value6[4:3]}.
    uint32_t value8 = (value6 << 2U) | ((value6 >> 3U) & 0x3U);
    if ((effect_state & MVS_EFFECT_STATE_SHADOW) != 0U) {
        value8 >>= 1U;
    }
    return (uint8_t)value8;
}

#elif MVS_EFFECT_MODEL == MVS_EFFECT_MODEL_MAME

#define MVS_EFFECT_MODEL_NAME "MAME"

// Exact channel results pinned to MAME commit
// e47c0f33c5be3ee286ff65bed13458c2920340d2, neogeo_v.cpp lines 23-64.
// The values are precomputed from MAME's resistor-network implementation so
// firmware performs no floating-point work. Row order matches the captured
// flags directly: normal, SHADOW, DARK, DARK+SHADOW.
static inline uint8_t mvs_effect_model_channel(uint32_t value5, uint32_t effect_state)
{
    static const uint8_t channel[4][32] = {
        {0,   8,   14,  22,  30,  38,  44,  52,  65,  73,  79,  86,  95,  103, 109, 117,
         138, 146, 152, 160, 169, 176, 182, 190, 203, 211, 217, 225, 233, 241, 247, 255},
        {0,  4,  8,  12, 17, 21, 25,  29,  36,  40,  44,  48,  53,  57,  61,  65,
         77, 81, 85, 89, 94, 98, 102, 106, 113, 117, 121, 125, 130, 134, 138, 142},
        {0,   8,   14,  21,  30,  38,  44,  51,  64,  71,  77,  85,  94,  101, 107, 115,
         136, 144, 150, 158, 166, 174, 180, 188, 200, 208, 214, 221, 230, 238, 244, 251},
        {0,  4,  8,  12, 17, 21, 24,  29,  36,  40,  43,  48,  53,  57,  60,  64,
         76, 81, 84, 88, 93, 97, 101, 105, 112, 116, 120, 124, 129, 133, 136, 141},
    };

    return channel[effect_state & MVS_EFFECT_STATE_MASK][value5 & 0x1FU];
}

#else
#error "MVS_EFFECT_MODEL must be MVS_EFFECT_MODEL_MISTER or MVS_EFFECT_MODEL_MAME"
#endif

#endif // NEOPICO_HD_MVS_EFFECT_MODEL_H

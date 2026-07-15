#ifndef NEOPICO_HD_MVS_COLOR_MODEL_H
#define NEOPICO_HD_MVS_COLOR_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "mvs_color.h"

// Normal-pixel color conversion only. DARK and SHADOW are deliberately not
// inputs to this API so selecting a color model cannot enable either effect.
typedef enum {
    MVS_COLOR_MODEL_DIGITAL = 0,
    MVS_COLOR_MODEL_ANALOG = 1,
    MVS_COLOR_MODEL_COUNT,
} mvs_color_model_t;

_Static_assert(MVS_COLOR_MODEL_DIGITAL == 0 && MVS_COLOR_MODEL_ANALOG == 1 && MVS_COLOR_MODEL_COUNT == 2,
               "color-model values are persistent settings and LUT row indices");

static inline bool mvs_color_model_is_valid(uint32_t model)
{
    return model < MVS_COLOR_MODEL_COUNT;
}

// Normal-state channel levels from the Neo Geo five-resistor DAC model pinned
// to MAME commit e47c0f33c5be3ee286ff65bed13458c2920340d2. Values are
// precomputed so firmware performs no floating-point work.
static inline uint8_t mvs_color_model_analog_channel(uint32_t value5)
{
    static const uint8_t channel[32] = {
        0,   8,   14,  22,  30,  38,  44,  52,  65,  73,  79,  86,  95,  103, 109, 117,
        138, 146, 152, 160, 169, 176, 182, 190, 203, 211, 217, 225, 233, 241, 247, 255,
    };
    return channel[value5 & 0x1FU];
}

static inline uint16_t mvs_color_model_pack_rgb565(mvs_color_model_t model, uint32_t r5, uint32_t g5, uint32_t b5)
{
    if (model == MVS_COLOR_MODEL_ANALOG) {
        const uint32_t r8 = mvs_color_model_analog_channel(r5);
        const uint32_t g8 = mvs_color_model_analog_channel(g5);
        const uint32_t b8 = mvs_color_model_analog_channel(b5);
        return (uint16_t)(((r8 >> 3U) << 11U) | ((g8 >> 2U) << 5U) | (b8 >> 3U));
    }

    // Digital is the stable exact RGB555-to-RGB565 mapping.
    return mvs_pack_rgb565(r5, g5, b5);
}

#endif // NEOPICO_HD_MVS_COLOR_MODEL_H

#ifndef NEOPICO_HD_MVS_EFFECT_LUT_H
#define NEOPICO_HD_MVS_EFFECT_LUT_H

#include <stdint.h>

#include "mvs_color.h"
#include "mvs_effect_model.h"

// A cube-wide black clamp depends on all three channels and cannot be
// represented by independent RG and B tables without per-pixel branching.
#if MVS_BLACK_LEVEL_CLAMP != 0
#error "The split DARK/SHADOW LUT requires MVS_BLACK_LEVEL_CLAMP=0"
#endif

#define MVS_EFFECT_STATE_COUNT 4U
#define MVS_EFFECT_RG_COLOR_BITS 10U
#define MVS_EFFECT_RG_COLOR_COUNT (1U << MVS_EFFECT_RG_COLOR_BITS)
#define MVS_EFFECT_B_COLOR_BITS 5U
#define MVS_EFFECT_B_COLOR_COUNT (1U << MVS_EFFECT_B_COLOR_BITS)
#define MVS_EFFECT_RG_TABLE_ENTRIES (MVS_EFFECT_STATE_COUNT * MVS_EFFECT_RG_COLOR_COUNT)
#define MVS_EFFECT_B_TABLE_ENTRIES (MVS_EFFECT_STATE_COUNT * MVS_EFFECT_B_COLOR_COUNT)
#define MVS_EFFECT_LUT_BYTES ((MVS_EFFECT_RG_TABLE_ENTRIES + MVS_EFFECT_B_TABLE_ENTRIES) * sizeof(uint16_t))

typedef struct {
    uint16_t rg[MVS_EFFECT_RG_TABLE_ENTRIES];
    uint16_t b[MVS_EFFECT_B_TABLE_ENTRIES];
} mvs_effect_lut_t;

_Static_assert(sizeof(mvs_effect_lut_t) == 8448U, "split effect LUT size changed");

static inline uint32_t mvs_effect_normalize_color_idx(uint32_t color_idx)
{
    uint32_t normalized = (color_idx ^ (MVS_RAW_COLOR_MASK & MVS_CAPTURE_COLOR_MASK)) & MVS_CAPTURE_COLOR_MASK;
#if MVS_REVERSE_15BIT
    normalized = mvs_reverse_15(normalized);
#endif
    return normalized;
}

static inline void mvs_effect_lut_generate(mvs_effect_lut_t *lut)
{
    for (uint32_t effect_state = 0; effect_state < MVS_EFFECT_STATE_COUNT; effect_state++) {
        const uint32_t rg_base = effect_state << MVS_EFFECT_RG_COLOR_BITS;
        const uint32_t b_base = effect_state << MVS_EFFECT_B_COLOR_BITS;

        for (uint32_t raw_rg = 0; raw_rg < MVS_EFFECT_RG_COLOR_COUNT; raw_rg++) {
            const uint32_t r5 = mvs_correct_5bit((raw_rg >> 5U) & 0x1FU, MVS_INVERT_R, MVS_REVERSE_R);
            const uint32_t g5 = mvs_correct_5bit(raw_rg & 0x1FU, MVS_INVERT_G, MVS_REVERSE_G);
            const uint32_t r8 = mvs_effect_model_channel(r5, effect_state);
            const uint32_t g8 = mvs_effect_model_channel(g5, effect_state);
            lut->rg[rg_base | raw_rg] = (uint16_t)(((r8 >> 3U) << 11U) | ((g8 >> 2U) << 5U));
        }

        for (uint32_t raw_b = 0; raw_b < MVS_EFFECT_B_COLOR_COUNT; raw_b++) {
            const uint32_t b5 = mvs_correct_5bit(raw_b, MVS_INVERT_B, MVS_REVERSE_B);
            const uint32_t b8 = mvs_effect_model_channel(b5, effect_state);
            lut->b[b_base | raw_b] = (uint16_t)(b8 >> 3U);
        }
    }
}

static inline uint16_t mvs_effect_lut_lookup_color(const mvs_effect_lut_t *lut, uint32_t color_idx,
                                                   uint32_t effect_state)
{
    const uint32_t normalized = mvs_effect_normalize_color_idx(color_idx);
    const uint32_t state = effect_state & MVS_EFFECT_STATE_MASK;
    const uint32_t rg_index = (state << MVS_EFFECT_RG_COLOR_BITS) | (normalized >> MVS_EFFECT_B_COLOR_BITS);
    const uint32_t b_index = (state << MVS_EFFECT_B_COLOR_BITS) | (normalized & (MVS_EFFECT_B_COLOR_COUNT - 1U));
    return (uint16_t)(lut->rg[rg_index] | lut->b[b_index]);
}

static inline uint16_t mvs_effect_lut_lookup_raw(const mvs_effect_lut_t *lut, uint32_t raw)
{
    const uint32_t color_idx = (raw >> 2U) & MVS_CAPTURE_COLOR_MASK;
    const uint32_t effect_state = (raw >> 17U) & MVS_EFFECT_STATE_MASK;
    return mvs_effect_lut_lookup_color(lut, color_idx, effect_state);
}

#endif // NEOPICO_HD_MVS_EFFECT_LUT_H

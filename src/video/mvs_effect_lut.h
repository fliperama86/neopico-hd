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

// ---------------------------------------------------------------------------
// RGB888 variant, for the 32-bit scanout path.
//
// Same split-table shape, but each entry holds full 8-bit channels packed as
// 0x00RRGGBB to match the HSTX RGB888 lane layout (L0=blue, L1=green, L2=red).
// This is what makes 32-bit scanout worth doing: the RGB565 tables discard the
// DARK half-step in red and blue, because a 5-bit channel cannot represent it.
// ---------------------------------------------------------------------------

#define MVS_EFFECT_LUT888_BYTES ((MVS_EFFECT_RG_TABLE_ENTRIES + MVS_EFFECT_B_TABLE_ENTRIES) * sizeof(uint32_t))

typedef struct {
    uint32_t rg[MVS_EFFECT_RG_TABLE_ENTRIES];
    uint32_t b[MVS_EFFECT_B_TABLE_ENTRIES];
} mvs_effect_lut888_t;

_Static_assert(sizeof(mvs_effect_lut888_t) == 16896U, "split RGB888 effect LUT size changed");

static inline void mvs_effect_lut888_generate(mvs_effect_lut888_t *lut)
{
    for (uint32_t effect_state = 0; effect_state < MVS_EFFECT_STATE_COUNT; effect_state++) {
        const uint32_t rg_base = effect_state << MVS_EFFECT_RG_COLOR_BITS;
        const uint32_t b_base = effect_state << MVS_EFFECT_B_COLOR_BITS;

        for (uint32_t raw_rg = 0; raw_rg < MVS_EFFECT_RG_COLOR_COUNT; raw_rg++) {
            const uint32_t r5 = mvs_correct_5bit((raw_rg >> 5U) & 0x1FU, MVS_INVERT_R, MVS_REVERSE_R);
            const uint32_t g5 = mvs_correct_5bit(raw_rg & 0x1FU, MVS_INVERT_G, MVS_REVERSE_G);
            const uint32_t r8 = mvs_effect_model_channel(r5, effect_state);
            const uint32_t g8 = mvs_effect_model_channel(g5, effect_state);
            lut->rg[rg_base | raw_rg] = (r8 << 16U) | (g8 << 8U);
        }

        for (uint32_t raw_b = 0; raw_b < MVS_EFFECT_B_COLOR_COUNT; raw_b++) {
            const uint32_t b5 = mvs_correct_5bit(raw_b, MVS_INVERT_B, MVS_REVERSE_B);
            lut->b[b_base | raw_b] = mvs_effect_model_channel(b5, effect_state);
        }
    }
}

// Ring words in the RGB888 path carry raw capture entropy, not a converted
// pixel: bit 15 is DARK and bits 14:0 are the raw RGB555 field. SHADOW is a
// screen-wide control (system latch bit 0), so it is tracked per line rather
// than spending a 17th bit per pixel.
#define MVS_ENTROPY_DARK_BIT 15U

static inline uint16_t mvs_entropy_pack_raw(uint32_t raw)
{
    const uint32_t color_idx = (raw >> 2U) & MVS_CAPTURE_COLOR_MASK;
    const uint32_t dark = (raw >> 18U) & 1U;
    return (uint16_t)((dark << MVS_ENTROPY_DARK_BIT) | color_idx);
}

static inline uint32_t mvs_effect_lut888_lookup_entropy(const mvs_effect_lut888_t *lut, uint32_t entropy,
                                                        uint32_t line_shadow)
{
    const uint32_t normalized = mvs_effect_normalize_color_idx(entropy & MVS_CAPTURE_COLOR_MASK);
    const uint32_t state = ((entropy >> MVS_ENTROPY_DARK_BIT) << 1U) | (line_shadow & 1U);
    const uint32_t rg_index = (state << MVS_EFFECT_RG_COLOR_BITS) | (normalized >> MVS_EFFECT_B_COLOR_BITS);
    const uint32_t b_index = (state << MVS_EFFECT_B_COLOR_BITS) | (normalized & (MVS_EFFECT_B_COLOR_COUNT - 1U));
    return lut->rg[rg_index] | lut->b[b_index];
}

#endif // NEOPICO_HD_MVS_EFFECT_LUT_H

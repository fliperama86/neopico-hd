#ifndef NEOPICO_HD_MVS_DIGITAL_EFFECT_H
#define NEOPICO_HD_MVS_DIGITAL_EFFECT_H

#include <stdint.h>

#include "mvs_color.h"

#define MVS_DIGITAL_EFFECT_STATE_SHADOW 0x1U
#define MVS_DIGITAL_EFFECT_STATE_DARK 0x2U
#define MVS_DIGITAL_EFFECT_STATE_MASK 0x3U
#define MVS_DIGITAL_EFFECT_RAW_MASK (MVS_DIGITAL_EFFECT_STATE_MASK << 17U)

static inline uint32_t mvs_digital_effect_decrement6(uint32_t value6)
{
#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP == 1
    // USAT interprets the wrapped result of 0 - 1 as signed -1 and clamps it
    // to zero. Values 1 through 63 remain an exact decrement. This fixed
    // two-instruction sequence avoids a data-dependent branch.
    __asm__("subs %0, %0, #1\n\tusat %0, #6, %0" : "+r"(value6) : : "cc");
    return value6;
#else
    return value6 - (value6 != 0U);
#endif
}

// Exact Digital-reference conversion after RGB565 truncation. DARK subtracts
// one from each expanded six-bit channel with saturation. SHADOW then halves
// those channels. Packing the resulting values is equivalent to generating
// the reference RGB888 value first and truncating it to RGB565.
static inline uint16_t mvs_digital_effect_pack_rgb565(uint32_t r5, uint32_t g5, uint32_t b5, uint32_t effect_state)
{
    r5 &= 0x1FU;
    g5 &= 0x1FU;
    b5 &= 0x1FU;

    const uint32_t state = effect_state & MVS_DIGITAL_EFFECT_STATE_MASK;
    uint32_t g6 = (g5 << 1U) | (g5 >> 4U);
    uint32_t pixel = (r5 << 11U) | (g6 << 5U) | b5;

    // Normal and SHADOW-only pixels need no per-channel DARK arithmetic.
    if (state == 0U) {
        return (uint16_t)pixel;
    }
    if (state == MVS_DIGITAL_EFFECT_STATE_SHADOW) {
        return (uint16_t)((pixel >> 1U) & 0x7BEFU);
    }

    uint32_t r6 = (r5 << 1U) | (r5 >> 4U);
    uint32_t b6 = (b5 << 1U) | (b5 >> 4U);

    r6 = mvs_digital_effect_decrement6(r6);
    g6 = mvs_digital_effect_decrement6(g6);
    b6 = mvs_digital_effect_decrement6(b6);

    pixel = ((r6 >> 1U) << 11U) | (g6 << 5U) | (b6 >> 1U);
    if ((state & MVS_DIGITAL_EFFECT_STATE_SHADOW) != 0U) {
        pixel = (pixel >> 1U) & 0x7BEFU;
    }
    return (uint16_t)pixel;
}

static inline uint16_t mvs_digital_effect_rgb565_color(uint32_t color_idx, uint32_t effect_state)
{
    uint32_t r5;
    uint32_t g5;
    uint32_t b5;
    mvs_correct_color_idx(color_idx & MVS_CAPTURE_COLOR_MASK, &r5, &g5, &b5);
    return mvs_digital_effect_pack_rgb565(r5, g5, b5, effect_state);
}

static inline uint32_t mvs_digital_effect_reverse32(uint32_t value)
{
#if defined(__arm__) || defined(__thumb__)
    uint32_t reversed;
    __asm__("rbit %0, %1" : "=r"(reversed) : "r"(value));
    return reversed;
#else
    value = ((value >> 1U) & UINT32_C(0x55555555)) | ((value & UINT32_C(0x55555555)) << 1U);
    value = ((value >> 2U) & UINT32_C(0x33333333)) | ((value & UINT32_C(0x33333333)) << 2U);
    value = ((value >> 4U) & UINT32_C(0x0F0F0F0F)) | ((value & UINT32_C(0x0F0F0F0F)) << 4U);
    value = ((value >> 8U) & UINT32_C(0x00FF00FF)) | ((value & UINT32_C(0x00FF00FF)) << 8U);
    return (value >> 16U) | (value << 16U);
#endif
}

static inline uint16_t mvs_digital_effect_normal_rgb565_raw(uint32_t raw)
{
#if MVS_RAW_COLOR_MASK == 0 && MVS_REVERSE_15BIT == 0 && MVS_INVERT_R == 0 && MVS_INVERT_G == 0 &&                     \
    MVS_INVERT_B == 0 && MVS_REVERSE_R == 1 && MVS_REVERSE_G == 1 && MVS_REVERSE_B == 1
    const uint32_t reversed = mvs_digital_effect_reverse32(raw);
    const uint32_t r5 = (reversed >> 15U) & 0x1FU;
    const uint32_t g5 = (reversed >> 20U) & 0x1FU;
    const uint32_t b5 = (reversed >> 25U) & 0x1FU;
    return mvs_pack_rgb565(r5, g5, b5);
#else
    const uint32_t color_idx = (raw >> 2U) & MVS_CAPTURE_COLOR_MASK;
    return mvs_digital_effect_rgb565_color(color_idx, 0U);
#endif
}

static inline uint16_t mvs_digital_effect_rgb565_raw(uint32_t raw)
{
    const uint32_t effect_state = (raw >> 17U) & MVS_DIGITAL_EFFECT_STATE_MASK;

#if MVS_RAW_COLOR_MASK == 0 && MVS_REVERSE_15BIT == 0 && MVS_INVERT_R == 0 && MVS_INVERT_G == 0 &&                     \
    MVS_INVERT_B == 0 && MVS_REVERSE_R == 1 && MVS_REVERSE_G == 1 && MVS_REVERSE_B == 1
    // One Cortex-M33 RBIT corrects the bit order of all three PCB-wired
    // channels. The fields land in ascending R/G/B order at bits 15 through
    // 29. UBFX-style masks exclude captured CSYNC and PCLK from blue.
    const uint32_t reversed = mvs_digital_effect_reverse32(raw);
    const uint32_t r5 = (reversed >> 15U) & 0x1FU;
    const uint32_t g5 = (reversed >> 20U) & 0x1FU;
    const uint32_t b5 = (reversed >> 25U) & 0x1FU;
    return mvs_digital_effect_pack_rgb565(r5, g5, b5, effect_state);
#else
    const uint32_t color_idx = (raw >> 2U) & MVS_CAPTURE_COLOR_MASK;
    return mvs_digital_effect_rgb565_color(color_idx, effect_state);
#endif
}

#endif // NEOPICO_HD_MVS_DIGITAL_EFFECT_H

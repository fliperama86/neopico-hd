#ifndef NEOPICO_HD_MVS_COLOR_H
#define NEOPICO_HD_MVS_COLOR_H

#include <stdbool.h>
#include <stdint.h>

// MVS input color correction. Keep these defaults in one shared location so
// firmware LUT generation and host-side exhaustive tests use identical code.
#ifndef MVS_INVERT_R
#define MVS_INVERT_R 0
#endif
#ifndef MVS_INVERT_G
#define MVS_INVERT_G 0
#endif
#ifndef MVS_INVERT_B
#define MVS_INVERT_B 0
#endif

// The PCB wires R4/G4/B4 to the lower GPIO in each five-bit field.
#ifndef MVS_REVERSE_R
#define MVS_REVERSE_R 1
#endif
#ifndef MVS_REVERSE_G
#define MVS_REVERSE_G 1
#endif
#ifndef MVS_REVERSE_B
#define MVS_REVERSE_B 1
#endif

// XOR mask applied to the captured 15-bit color before channel extraction.
#ifndef MVS_RAW_COLOR_MASK
#define MVS_RAW_COLOR_MASK 0
#endif

// Reverse the entire captured 15-bit word before channel correction.
#ifndef MVS_REVERSE_15BIT
#define MVS_REVERSE_15BIT 0
#endif

// Clamp an RGB cube at and below this level to black. Zero disables it.
#ifndef MVS_BLACK_LEVEL_CLAMP
#define MVS_BLACK_LEVEL_CLAMP 0
#endif

#define MVS_CAPTURE_COLOR_BITS 15U
#define MVS_CAPTURE_COLOR_SIZE (1U << MVS_CAPTURE_COLOR_BITS)
#define MVS_CAPTURE_COLOR_MASK (MVS_CAPTURE_COLOR_SIZE - 1U)

static inline uint32_t mvs_reverse_15(uint32_t x)
{
    x &= 0x7FFF;
    return ((x & 0x0001U) << 14) | ((x & 0x0002U) << 12) | ((x & 0x0004U) << 10) | ((x & 0x0008U) << 8) |
           ((x & 0x0010U) << 6) | ((x & 0x0020U) << 4) | ((x & 0x0040U) << 2) | ((x & 0x0080U) << 0) |
           ((x & 0x0100U) >> 2) | ((x & 0x0200U) >> 4) | ((x & 0x0400U) >> 6) | ((x & 0x0800U) >> 8) |
           ((x & 0x1000U) >> 10) | ((x & 0x2000U) >> 12) | ((x & 0x4000U) >> 14);
}

static inline uint32_t mvs_correct_5bit(uint32_t x, int invert, int reverse)
{
    if (reverse) {
        x = ((x & 1U) << 4) | ((x & 2U) << 2) | (x & 4U) | ((x & 8U) >> 2) | ((x & 16U) >> 4);
    }
    if (invert) {
        x ^= 0x1F;
    }
    return x & 0x1F;
}

static inline uint16_t mvs_pack_rgb565(uint32_t r5, uint32_t g5, uint32_t b5)
{
    const uint32_t g6 = (g5 << 1U) | (g5 >> 4U);
    return (uint16_t)((r5 << 11U) | (g6 << 5U) | b5);
}

static inline void mvs_correct_color_idx(uint32_t color_idx, uint32_t *r5, uint32_t *g5, uint32_t *b5)
{
    uint32_t color15 = color_idx ^ (MVS_RAW_COLOR_MASK & 0x7FFFU);
#if MVS_REVERSE_15BIT
    color15 = mvs_reverse_15(color15);
#endif

    *r5 = mvs_correct_5bit((color15 >> 10) & 0x1F, MVS_INVERT_R, MVS_REVERSE_R);
    *g5 = mvs_correct_5bit((color15 >> 5) & 0x1F, MVS_INVERT_G, MVS_REVERSE_G);
    *b5 = mvs_correct_5bit(color15 & 0x1F, MVS_INVERT_B, MVS_REVERSE_B);
}

static inline bool mvs_is_clamped_black(uint32_t r5, uint32_t g5, uint32_t b5)
{
    return MVS_BLACK_LEVEL_CLAMP > 0 && r5 <= MVS_BLACK_LEVEL_CLAMP && g5 <= MVS_BLACK_LEVEL_CLAMP &&
           b5 <= MVS_BLACK_LEVEL_CLAMP;
}

#endif // NEOPICO_HD_MVS_COLOR_H

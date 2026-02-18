#ifndef SELFTEST_LAYOUT_H
#define SELFTEST_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

// Diagnostic bitfield format consumed by selftest_layout_update().
// Uses the old selftest logical mapping (bit N == GPIO N+22 in the original sampler).
#define SELFTEST_BIT_DAT (1U << 0)
#define SELFTEST_BIT_WS (1U << 1)
#define SELFTEST_BIT_BCK (1U << 2)
#define SELFTEST_BIT_CSYNC (1U << 5)
#define SELFTEST_BIT_PCLK (1U << 6)
#define SELFTEST_BIT_B4 (1U << 7)
#define SELFTEST_BIT_B3 (1U << 8)
#define SELFTEST_BIT_B2 (1U << 9)
#define SELFTEST_BIT_B1 (1U << 10)
#define SELFTEST_BIT_B0 (1U << 11)
#define SELFTEST_BIT_G4 (1U << 12)
#define SELFTEST_BIT_G3 (1U << 13)
#define SELFTEST_BIT_G2 (1U << 14)
#define SELFTEST_BIT_G1 (1U << 15)
#define SELFTEST_BIT_G0 (1U << 16)
#define SELFTEST_BIT_R4 (1U << 17)
#define SELFTEST_BIT_R3 (1U << 18)
#define SELFTEST_BIT_R2 (1U << 19)
#define SELFTEST_BIT_R1 (1U << 20)
#define SELFTEST_BIT_R0 (1U << 21)
#define SELFTEST_BIT_SHADOW (1U << 22)
#define SELFTEST_VIDEO_BITS_MASK                                                                                       \
    (SELFTEST_BIT_CSYNC | SELFTEST_BIT_PCLK | SELFTEST_BIT_B4 | SELFTEST_BIT_B3 | SELFTEST_BIT_B2 | SELFTEST_BIT_B1 |  \
     SELFTEST_BIT_B0 | SELFTEST_BIT_G4 | SELFTEST_BIT_G3 | SELFTEST_BIT_G2 | SELFTEST_BIT_G1 | SELFTEST_BIT_G0 |       \
     SELFTEST_BIT_R4 | SELFTEST_BIT_R3 | SELFTEST_BIT_R2 | SELFTEST_BIT_R1 | SELFTEST_BIT_R0 | SELFTEST_BIT_SHADOW)

// Draws the static self-test screen layout and initializes icon states.
void selftest_layout_reset(void);

// Bounded dynamic updates for layout-only mode (call at low rate).
void selftest_layout_update(uint32_t frame_count, bool has_snapshot, uint32_t toggled_bits);

// Update baseline sync telemetry lines in the self-test OSD.
// in_fps_x100/out_fps_x100 are fixed-point FPS with 2 decimals.
void selftest_layout_set_baseline(uint32_t in_fps_x100, uint32_t out_fps_x100, int32_t phase, int32_t slip_fpm);

#endif // SELFTEST_LAYOUT_H

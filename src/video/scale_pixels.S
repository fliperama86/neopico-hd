// =============================================================================
// scale_pixels.S - hand-written Cortex-M33 horizontal pixel expansion kernels.
//
// Replaces the C video_pipeline_triple_pixels_fast / _quadruple_pixels_fast
// when NEOPICO_TRIPLE_ASM=1. These run per-line in the Core 1 alarm IRQ during
// 720p (3x) / 240p (4x) ring prep, so they are the hottest path in the
// precomposed pipeline. The win is batching the per-source-word stores into a
// single STM and post-incrementing loads.
//
// FRESH M33 code (NOT ported from any RP2040-era asm — see opl-asm-bug: ported
// asm has silently mis-behaved on RP2350). Correctness is gated at boot by
// video_pipeline_scale_selftest() comparing these against C references over
// several pixel counts (incl. odd); g_scale_asm_selftest_ok carries the verdict
// (shown on the Genlock OSD as ASM:OK / ASM:BAD).
//
// ABI: void f(uint32_t *dst /*r0*/, const uint16_t *src /*r1*/, int count /*r2*/)
// Placed in .scratch_y to match the C originals' __scratch_y("") residency
// (RAM execution, no XIP stalls in IRQ context).
// =============================================================================

#if NEOPICO_TRIPLE_ASM

    .syntax unified
    .cpu cortex-m33
    .thumb

// -----------------------------------------------------------------------------
// 3x expansion: each source pixel emitted 3 times. For a source word
// A = (a1:a0) the three output words are:
//   [a0|a0<<16]  (a0 replicated)
//   A            (a0 low, a1 high - the source word, UNCHANGED)
//   [a1|a1<<16]  (a1 replicated)
// 320 src px -> 160 src words -> 480 out words. Main loop does 2 src words
// (4 px -> 6 out words) per iteration; a tail handles one leftover pair.
// STM stores registers in ASCENDING REGISTER-NUMBER order to ascending
// addresses, so the registers are allocated to match the memory layout.
// -----------------------------------------------------------------------------
    .section .scratch_y.video_pipeline_triple, "ax", %progbits
    .global video_pipeline_triple_pixels_fast
    .thumb_func
    .type video_pipeline_triple_pixels_fast, %function
video_pipeline_triple_pixels_fast:
    push    {r4-r9}
    lsrs    r2, r2, #1              // r2 = pairs = count / 2
    beq     3f
    lsrs    r3, r2, #1              // r3 = pairs / 2 = main (by-2) iterations
    beq     2f
1:  ldr     r5, [r1], #4           // r5 = A  (also the middle output word)
    ldr     r8, [r1], #4           // r8 = B  (also the middle output word)
    uxth    r4, r5                 // a0
    orr     r4, r4, r4, lsl #16    // r4 = a0 replicated
    lsr     r6, r5, #16            // a1
    orr     r6, r6, r6, lsl #16    // r6 = a1 replicated
    uxth    r7, r8                 // b0
    orr     r7, r7, r7, lsl #16    // r7 = b0 replicated
    lsr     r9, r8, #16            // b1
    orr     r9, r9, r9, lsl #16    // r9 = b1 replicated
    stmia   r0!, {r4, r5, r6, r7, r8, r9}  // a0rep,A,a1rep,b0rep,B,b1rep
    subs    r3, r3, #1
    bne     1b
2:  tst     r2, #1                 // one trailing single pair?
    beq     3f
    ldr     r5, [r1], #4
    uxth    r4, r5
    orr     r4, r4, r4, lsl #16
    lsr     r6, r5, #16
    orr     r6, r6, r6, lsl #16
    stmia   r0!, {r4, r5, r6}      // a0rep, A, a1rep
3:  pop     {r4-r9}
    bx      lr
    .size video_pipeline_triple_pixels_fast, . - video_pipeline_triple_pixels_fast

// -----------------------------------------------------------------------------
// 4x expansion: each source pixel emitted 4 times. For A = (a1:a0):
//   [a0|a0<<16], [a0|a0<<16], [a1|a1<<16], [a1|a1<<16]
// One source word -> 4 output words per iteration.
// -----------------------------------------------------------------------------
    .section .scratch_y.video_pipeline_quadruple, "ax", %progbits
    .global video_pipeline_quadruple_pixels_fast
    .thumb_func
    .type video_pipeline_quadruple_pixels_fast, %function
video_pipeline_quadruple_pixels_fast:
    push    {r4-r7}
    lsrs    r2, r2, #1              // r2 = pairs = count / 2
    beq     2f
1:  ldr     r4, [r1], #4           // two
    uxth    r5, r4                 // p0
    orr     r5, r5, r5, lsl #16    // r5 = d0
    lsr     r6, r4, #16            // p1
    orr     r6, r6, r6, lsl #16    // r6 = d1
    mov     r4, r5                 // r4 = d0 (copy)
    mov     r7, r6                 // r7 = d1 (copy)
    stmia   r0!, {r4, r5, r6, r7}  // d0, d0, d1, d1
    subs    r2, r2, #1
    bne     1b
2:  pop     {r4-r7}
    bx      lr
    .size video_pipeline_quadruple_pixels_fast, . - video_pipeline_quadruple_pixels_fast

#endif // NEOPICO_TRIPLE_ASM

# NeoPico-HD: Line-Streaming Pipeline

## Current Status

**Working:** 256-line ring buffer with soft frame sync. Video stable, audio working.

**Memory:** ~241 KB BSS (down from ~417 KB original)

---

## Architecture

```
ORIGINAL (Frame-buffered + LUT):
  Core 0: Capture full frame → interp0 + 256KB LUT → framebuf[320x240]
  Core 1: ISR reads framebuf → HDMI output
  Memory: ~417 KB

CURRENT (256-line ring buffer):
  Core 0: Capture line → direct RGB555→RGB565 → ring[256 lines]
  Core 1: ISR reads from ring → HDMI output
  On output VSYNC: Sync read position to latest input frame
  Memory: ~241 KB
```

---

## How It Works

### Line Ring Buffer

The ring buffer holds 256 lines (more than one full 224-line frame):

- **Core 0 (Producer):** Writes captured MVS lines sequentially
- **Core 1 (Consumer):** Reads lines for HDMI output with 2x scaling

### Frame Synchronization

At each **output VSYNC**, Core 1 syncs its read position to the latest input frame:

```c
// Called at output VSYNC
static inline void line_ring_output_vsync(void) {
    g_line_ring.read_frame_start = g_line_ring.frame_base_idx;
}
```

This approach:
- Allows independent input/output clocks (~59.6 Hz vs ~60 Hz)
- Occasionally duplicates or skips a frame to maintain sync
- No visible tearing due to full-frame buffer margin

---

## 40-Line Streaming Attempt (Shelved)

We investigated reducing the buffer to 40 lines (~25 KB) inspired by cps2_digiav's approach.

### What We Tried

1. **HSTX Hard Reset on Input VSYNC**
   - Reset HSTX shift register and clock generator via CSR.EN toggle
   - **Result:** Display lost signal entirely

2. **Soft Resync (flag only)**
   - Just sync read position without HSTX reset
   - **Result:** Flickering - 40-line buffer too small for phase variation

3. **Clock Adjustment (125 MHz)**
   - Slow output to ~59.5 Hz so it trails input
   - **Result:** Still flickering - phase mismatch within frames

### Why It Failed

The fundamental issue: with only 40 lines of buffer, the phase relationship between input and output VSYNC must be tightly controlled. Without hardware genlock:

- Output VSYNC timing is independent of input
- Phase can drift by tens of lines within a frame
- 40 lines isn't enough margin for worst-case phase offset

### cps2_digiav Difference

cps2_digiav uses an FPGA with precise control over output timing. Their `V_STARTLINE` mechanism can instantly reposition the output scanline counter. The RP2350's HSTX doesn't support mid-stream repositioning without losing signal.

### Future Options

If 40-line streaming is revisited:
1. **PLL-based genlock:** Lock output pixel clock to input (complex)
2. **Frame skip detection:** Predict phase and pre-adjust
3. **Larger minimum buffer:** Find the threshold (64? 80? 128 lines?)

---

## Memory Budget

| Component | Size |
|-----------|------|
| line_ring (256 lines) | 164 KB |
| Audio buffers | ~8 KB |
| Other (stacks, DMA, etc.) | ~69 KB |
| **Total BSS** | **~241 KB** |

The RP2350 has 520 KB SRAM, leaving ~279 KB free for future features (OSD, settings, etc.).

---

## Files

| File | Purpose |
|------|---------|
| `src/video/line_ring.h` | Ring buffer API and data structure |
| `src/video/video_capture.c` | Core 0 capture loop, writes to ring |
| `lib/pico_dvi2/src/video_output.c` | Core 1 HDMI output, reads from ring |
| `src/main.c` | Scanline callback with 2x scaling |

---

## Future: DARK/SHADOW Support

The PIO capture preserves all 18 bits including DARK/SHADOW flags. To restore intensity effects:

1. Set `#define ENABLE_DARK_SHADOW 1` in video_capture.c
2. Add back `g_pixel_lut` (256 KB) and `generate_intensity_lut()`
3. Restore `interp0` configuration
4. Memory will increase by ~256 KB

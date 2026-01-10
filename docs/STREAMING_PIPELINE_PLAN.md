# NeoPico-HD: Line-Streaming Pipeline Implementation Plan

## Current Status

**Working:** 256-line buffer with current-frame display mode. Video stable, audio working.

**Memory:** ~241 KB BSS (down from ~417 KB original)

**Goal:** True 40-line streaming (~25 KB) with HSTX resync for perfect frame sync.

---

## Architecture Evolution

```
ORIGINAL (Frame-buffered + LUT):
  Core 0: Capture full frame → interp0 + 256KB LUT → framebuf[320x240]
  Core 1: ISR reads framebuf → HDMI output
  Memory: ~417 KB

CURRENT (256-line buffer, no LUT):
  Core 0: Capture line → direct RGB555→RGB565 → ring[256 lines]
  Core 1: ISR reads from ring → HDMI output
  On VSYNC: Core 1 syncs to current input frame
  Memory: ~241 KB

TARGET (40-line streaming + HSTX resync):
  Core 0: Capture line → direct RGB555→RGB565 → ring[40 lines]
  Core 1: ISR reads from ring → HDMI output
  On Input VSYNC: Core 1 resyncs HSTX → output restarts at line 0
  Memory: ~100 KB
```

---

## Lessons from cps2_digiav

The cps2_digiav project (same author as OSSC) handles arcade digital capture with these key insights:

### 1. No True Genlock Needed
- Input and output run at independent clocks
- No attempt to match frequencies
- Instead: **resync at frame boundaries**

### 2. V_STARTLINE Mechanism
```verilog
// When input frame_change detected:
if (frame_change) begin
    v_cnt <= V_STARTLINE;  // Jump output to calculated safe position
end
```
This ensures output is always at a safe phase relative to input.

### 3. 40-Line Buffer is Sufficient
- Buffer holds ~40 lines (not full frame)
- Output reads while input writes
- V_STARTLINE ensures read never catches write

### 4. Calculated Safe Position
```c
// From cps2_digiav video_modes.c
framesync_line = output_backporch - v_linediff;
// Ensures output starts at position where it won't catch up to input
```

---

## HSTX Resync Approach (Our Solution)

Based on RP2350 datasheet analysis:

### Key Discovery
From datasheet Section 12.11:
> "When EN is 0, the FIFO is not popped. The shift counter and clock generator are also reset to their initial state."

**We can reset HSTX by toggling CSR.EN!**

### Implementation Plan

#### 1. Add Resync Flag to line_ring.h
```c
typedef struct {
    uint16_t lines[LINE_RING_SIZE][LINE_WIDTH];
    volatile uint32_t write_idx;
    volatile uint32_t frame_base_idx;
    volatile uint32_t read_frame_start;
    volatile bool resync_pending;        // NEW: Core 0 requests resync
} line_ring_t;

// Core 0: Request resync on input VSYNC
static inline void line_ring_request_resync(void) {
    g_line_ring.frame_base_idx = g_line_ring.write_idx;
    __dmb();
    g_line_ring.resync_pending = true;
}

// Core 1: Check if resync needed
static inline bool line_ring_should_resync(void) {
    if (g_line_ring.resync_pending) {
        g_line_ring.resync_pending = false;
        g_line_ring.read_frame_start = g_line_ring.frame_base_idx;
        return true;
    }
    return false;
}
```

#### 2. Add HSTX Resync Function to video_output.c
```c
static void __scratch_x("") hstx_resync(void) {
    // 1. Abort DMA chains
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);

    // 2. Disable HSTX (resets shift register and clock generator)
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // 3. Drain HSTX FIFO
    while (!(hstx_fifo_hw->stat & HSTX_FIFO_STAT_EMPTY_BITS)) {
        (void)hstx_fifo_hw->fifo;
    }

    // 4. Reset scanline counter to start of frame
    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;

    // 5. Re-enable HSTX
    hstx_ctrl_hw->csr |= HSTX_CTRL_CSR_EN_BITS;

    // 6. Restart DMA from beginning of frame
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_channel_set_read_addr(DMACH_PING, vblank_line_vsync_off, false);
    dma_channel_set_trans_count(DMACH_PING, count_of(vblank_line_vsync_off), true);
}
```

#### 3. Trigger Resync in DMA ISR
```c
void __scratch_x("") dma_irq_handler() {
    // ... existing channel handling ...

    // Check for resync request during blanking period
    if (v_scanline < MODE_V_FRONT_PORCH && line_ring_should_resync()) {
        hstx_resync();
        return;  // Handler will be called again when DMA completes
    }

    // ... rest of existing handler ...
}
```

#### 4. Reduce Buffer to 40 Lines
```c
#define LINE_RING_SIZE 40       // 40 lines like cps2_digiav
#define LINE_WIDTH 320
#define LINES_PER_FRAME 224
```

---

## Timing Analysis

### Why 40 Lines is Enough

With HSTX resync:
- Output resets to line 0 when input starts new frame
- Output reads line N while input writes line N + margin
- Margin determined by when resync happens vs when input starts writing

**Phase relationship after resync:**
- Input: Just started frame, at line 0
- Output: Just reset, at line 0 (or small offset from blanking)
- Both advance together, output trails by blanking duration

**During frame:**
- Input writes 224 lines in ~16.78ms
- Output reads 480 lines in ~16.67ms (2x scaling)
- Output consumption rate ≈ input production rate (due to 2x scaling)
- 40-line margin handles jitter and blanking offset

### Frame Rate Handling

- Input: ~59.6 Hz
- Output: 60 Hz
- Output is ~0.7% faster

**Without resync:** Output gains ~1 frame every 2.5 seconds → rolling

**With resync:** Every input VSYNC resets output → perfect sync, no drift

---

## Memory Budget (Target)

| Component | Current | Target | Change |
|-----------|---------|--------|--------|
| line_ring | 164 KB | 25 KB | -139 KB |
| audio buffers | ~8 KB | ~8 KB | - |
| other | ~69 KB | ~69 KB | - |
| **Total BSS** | **~241 KB** | **~102 KB** | **-139 KB** |

**Additional savings: 139 KB freed for OSD/future features**

---

## Implementation Steps

### Phase 1: HSTX Resync (Current Focus)
- [ ] Add `resync_pending` flag to line_ring.h
- [ ] Implement `hstx_resync()` function
- [ ] Add resync check in DMA ISR during blanking
- [ ] Update `line_ring_vsync()` to request resync
- [ ] Test: verify no visible glitch during resync

### Phase 2: Reduce Buffer Size
- [ ] Change LINE_RING_SIZE from 256 to 40
- [ ] Update line_ring_ready() for smaller buffer
- [ ] Test: verify stable video with 40-line buffer
- [ ] Measure memory savings

### Phase 3: Optimization
- [ ] Profile resync timing
- [ ] Minimize resync duration
- [ ] Add debug counters for resync events
- [ ] Stress test with various games

---

## Potential Issues & Mitigations

### 1. Resync Glitch
**Risk:** Brief visual artifact during HSTX reset
**Mitigation:** Only resync during output blanking period

### 2. Audio Continuity
**Risk:** Audio glitch during resync
**Mitigation:** Audio runs on separate DMA, should be unaffected

### 3. Resync Timing
**Risk:** Resync takes too long, misses blanking window
**Mitigation:** Keep resync code minimal, in SRAM (.scratch_x)

### 4. Display Compatibility
**Risk:** Some displays don't handle resync well
**Mitigation:** Tink4K and similar scalers handle non-standard signals

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/video/line_ring.h` | Add resync_pending, update for 40-line buffer |
| `lib/pico_dvi2/src/video_output.c` | Add hstx_resync(), trigger in ISR |
| `src/video/video_capture.c` | Call line_ring_request_resync() on VSYNC |

---

## Verification Checklist

- [ ] Video stable with no rolling
- [ ] No visible glitch during resync
- [ ] Audio continuous
- [ ] Memory usage ~102 KB BSS
- [ ] Works with multiple games
- [ ] 1+ hour stability test
- [ ] Works with Tink4K and direct HDMI

---

## Future: Re-enabling DARK/SHADOW

When ready to restore DARK/SHADOW support:

1. Set `#define ENABLE_DARK_SHADOW 1` in video_capture.c
2. Add back `g_pixel_lut` (256 KB) and `generate_intensity_lut()`
3. Restore `interp0` configuration
4. Memory will increase by ~256 KB

The PIO capture already preserves all 18 bits including DARK/SHADOW flags.

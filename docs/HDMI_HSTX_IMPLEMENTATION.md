# HDMI & HSTX Implementation Reference

Comprehensive record of the RP2350 HSTX implementation and HDMI audio findings.

## 1. System Architecture

- **Core 0**: Handles pixel-perfect video capture (PIO1+DMA). Performs hardware-accelerated RGB565 conversion using Interpolators + LUT.
- **Core 1**: Dedicated to HSTX HDMI output and the audio pipeline (polling PIO2, SRC, and TERC4 encoding).
- **Inter-core**: Single 320x240 RGB565 framebuffer + Audio Ring Buffer.

## 2. HSTX Video Pipeline

### Timing (640x480 @ 60Hz)

- **Pixel Clock**: 25.2 MHz (System Clock 126 MHz / 5).
- **Line Length**: Exactly 800 cycles mandatory for HDMI stability.
- **Command Lists**: Uses `HSTX_CMD_RAW_REPEAT` for blanking and `HSTX_CMD_TMDS` for active pixels.
- **NOPs**: `HSTX_CMD_NOP` separators are used to manage hardware state transitions.

### Sync Polarity

- HDMI uses **active-low** sync bits ($0 = \text{Active}$, $1 = \text{Idle}$).
- TMDS control symbols:
  - `0x354u`: $V=0, H=0$
  - `0x0abu`: $V=0, H=1$
  - `0x154u`: $V=1, H=0$
  - `0x2abu`: $V=1, H=1$

### Stability & Scaling

- **RAW_N_SHIFTS**: Must be set to **1** in the HSTX command expander. A value of 0 actually means 32 shifts, which causes massive horizontal sync drift.
- **Line Doubling**: The 240p MVS source is scaled to 480p by doubling every line (`fb_line = dvi_line >> 1`). This ensures the full HDMI active area is filled with stable data.

## 3. HDMI Audio (Data Islands)

### Encoding

- **Mode**: TERC4 (4-bit to 10-bit).
- **Structure**: 36 pixel clocks (2 Guard + 32 Packet + 2 Guard).
- **Placement**: Sent during the horizontal sync pulse of every scanline.

### Critical Discoveries

1. **Validity Bit (V)**: MUST be set to **0 (Valid)**. Most TVs mute if $V=1$.
2. **ACR Packet**: Byte order is **Big Endian**.
   - $N = 6144$
   - $CTS = 25200$ (for 126MHz sys_clk)
3. **Throughput**: Sending packets on active video lines is required to meet the 48kHz rate (~200 packets/frame).
4. **Header Flag**: The very first symbol of the first header byte correctly omits the Data Island flag (bit 3) for compatibility.

## 4. Hardware Resource Mapping (RP2350B)

| Peripheral | Purpose       | GPIO Range     |
| ---------- | ------------- | -------------- |
| **HSTX**   | HDMI Output   | 12-19          |
| **PIO1**   | Video Capture | 25-43 (Bank 1) |
| **PIO2**   | I2S Audio     | 0-2 (Bank 0)   |

## 5. Reference References

- `pico_lib` (Shuichi Takano): Original HDMI audio logic.
- `hsdaoh-rp2350` (Steve Markgraf): HSTX Data Island proof-of-concept.
- `PicoDVI` (Luke Wren): Timing and TMDS reference.

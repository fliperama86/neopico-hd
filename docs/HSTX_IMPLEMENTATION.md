# HDMI & HSTX Implementation Reference

Comprehensive record of the RP2350 HSTX implementation and HDMI audio findings.

## 1. System Architecture

- **Core 0**: Handles pixel-perfect video capture (PIO1+DMA). Performs RGB565 conversion through a LUT path (32K default, 64K optional with `NEOPICO_ENABLE_DARK_SHADOW=ON`).
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

### Native 240p AVI Compatibility

The native 240p output uses a non-CEA 1280x240 timing with VIC 0. Production builds define `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1`, which emits the hardware-validated conservative AVI payload (`PB1=0x00`, `PB2=0x08`, `PR=0`). The active-format/aspect payload with `PR=3` caused black video with continuing HDMI audio on tested scaler paths. This compatibility behavior is limited to VIC 0; standard 480p and 720p metadata is unchanged.

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

### Pacing & Clock Accuracy

Audio packets are paced by a per-scanline accumulator in `hstx_di_queue_tick()`. The samples-per-line value is derived from the actual pixel clock and `h_total`, never from an assumed 60 Hz field rate (an early version assumed exactly 60 Hz, which over-delivered ~91 samples/s at 240p's 60.114 Hz and caused periodic dropouts; fixed in pico_hdmi `e2a4022`).

That fix still floor-truncated the value to 16.16 fixed point, delivering slightly fewer samples than the ACR-advertised rate on the same clock: about 0.18 samples/s short at 480p/240p and 0.09 at 720p. Since ACR (N=6144, CTS=pixel_clock/1000, integer-exact for 25.2 and 74.4 MHz) tells the sink to play exactly 48000 Hz, the sink's buffer drains slowly and conceals with a brief mute: a rare, TV-dependent audio drop every tens of minutes.

Since pico_hdmi `b6422ee`, `PICO_HDMI_EXACT_AUDIO_PACING` (default **ON**) carries the exact division remainder in a rational (Bresenham) accumulator, so long-term delivery equals the nominal sample rate exactly in every runtime mode, with bounded 1/65536-sample jitter. Rollback: build with `-DPICO_HDMI_EXACT_AUDIO_PACING=OFF`. The legacy non-RT path (`video_output.c`) keeps the truncated setter.

### Underrun Diagnostics

If the producer (Core 1 audio task) cannot keep the Data Island queue non-empty, the scanline ISR splices a pre-encoded 4-sample silence packet and increments `hstx_di_queue_silence_count`. Build with `-DNEOPICO_DIAG_AUDIO_OSD=ON` (default OFF) to display this counter as `AU<n>` on the selftest OSD screen: a counter that climbs while a drop is heard means producer starvation; a flat counter means the drop happened inside the sink.

## 4. Hardware Resource Mapping (RP2350B)

| Peripheral | Purpose       | GPIO Range     |
| ---------- | ------------- | -------------- |
| **HSTX**   | HDMI Output   | 12-19          |
| **PIO1**   | Video Capture | 27-45 (Bank 1) |
| **PIO2**   | I2S Audio     | 0-2 (Bank 0)   |

### External Connection Requirements

- **HDMI +5V**: Pin 18 on the HDMI connector MUST be powered with 5V.
- **Compatibility Note**: While the Morph4K may work without it, the RetroTINK and many TVs require this for signal detection.

## 5. Reference References

- `pico_lib` (Shuichi Takano): Original HDMI audio logic.
- `hsdaoh-rp2350` (Steve Markgraf): HSTX Data Island proof-of-concept.

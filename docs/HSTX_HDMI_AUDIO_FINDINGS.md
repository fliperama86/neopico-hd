# HSTX HDMI Audio Implementation - Key Findings

## Summary
Implementing HDMI audio on RP2350 using HSTX peripheral, adapted from PicoDVI.

## Critical Discoveries

### 1. ACR Packet Byte Order (BIG ENDIAN)
**Problem**: CTS value was showing wrong on HDMI analyzer - middle byte correct, surrounding bytes wrong.

**Root Cause**: ACR packet uses **big-endian** byte order per HDMI spec, but we used little-endian.

**Fix** in `data_packet.c`:
```c
// WRONG (little-endian):
packet->subpacket[0][1] = cts & 0xFF;         // SB1 = CTS[7:0]
packet->subpacket[0][2] = (cts >> 8) & 0xFF;  // SB2 = CTS[15:8]
packet->subpacket[0][3] = (cts >> 16) & 0x0F; // SB3 = CTS[19:16]

// CORRECT (big-endian):
packet->subpacket[0][1] = (cts >> 16) & 0x0F; // SB1 = CTS[19:16]
packet->subpacket[0][2] = (cts >> 8) & 0xFF;  // SB2 = CTS[15:8]
packet->subpacket[0][3] = cts & 0xFF;         // SB3 = CTS[7:0]
```

### 2. RP2350 Default Clock is 150MHz, NOT 125MHz
**Problem**: TMDS clock measured at 30.01 MHz, expected 25.175 MHz.

**Root Cause**: RP2350 SDK defaults to **150 MHz** system clock (not 125 MHz like RP2040).
- With CLKDIV=5: 150 / 5 = **30 MHz** actual pixel clock
- Code comments incorrectly assumed 125 MHz

**Impact**: CTS must match actual pixel clock for audio clock recovery.
- CTS for 25.175 MHz: 25175
- CTS for 30 MHz: **30000**

### 3. HSTX CLKDIV and N_SHIFTS Must Match
**Problem**: Changing CLKDIV without N_SHIFTS broke video output.

**Root Cause**: Both control timing and must be synchronized.
```c
hstx_ctrl_hw->csr =
    5u << HSTX_CTRL_CSR_CLKDIV_LSB |   // Must match N_SHIFTS
    5u << HSTX_CTRL_CSR_N_SHIFTS_LSB | // Must match CLKDIV
    2u << HSTX_CTRL_CSR_SHIFT_LSB |
    HSTX_CTRL_CSR_EN_BITS;
```

### 4. Data Island Structure (HSTX vs PicoDVI)
**Key Difference**:
- PicoDVI uses 2 symbols per 32-bit word (20 bits per lane)
- HSTX uses 1 symbol per 32-bit word (30 bits: 3 lanes × 10 bits)

**Data Island Layout** (36 HSTX words):
- Words 0-1: Leading guard band (2 clocks)
- Words 2-33: Packet data (32 clocks)
- Words 34-35: Trailing guard band (2 clocks)

### 5. TERC4 Encoding for HSTX
Lane packing per word:
```c
uint32_t word = (lane0 & 0x3FF) | ((lane1 & 0x3FF) << 10) | ((lane2 & 0x3FF) << 20);
```

Bit shuffle for subpacket bytes:
- 4 bytes from 4 subpackets interleaved per symbol position
- Nibbles extracted and TERC4 encoded to lanes 1 & 2
- Lane 0 carries header bits + sync

### 6. Audio Sample Rate Undersampling
**Problem**: Only 132 samples/frame sent (33 packets × 4 samples) vs 800 needed for 48kHz@60fps.

**Impact**: Only 16% of required samples - may cause audio to not work or be choppy.

**Solution Needed**: Send audio packets during horizontal blanking of active lines too, not just vblank.

## Packet Schedule (Current Implementation)
```
Line 0:      AVI InfoFrame (front porch)
Lines 1-9:   Blank lines (front porch)
Line 10:     ACR packet (vsync)
Line 11:     Audio InfoFrame (vsync)
Lines 12-44: Audio Sample packets (back porch)
Lines 45+:   Active video
```

## Verified Working
- HDMI mode detection
- ACR packet (N=6144, CTS correctly decoded)
- AVI InfoFrame (displayed on RetroTink)
- Data island encoding (guard bands, preamble, TERC4)
- Audio sample packet generation (non-zero samples)

## Still Debugging
- Actual audio output (no sound yet)
- Audio InfoFrame display on analyzer
- Sample rate matching

## Reference Implementations
- `pico_lib` by Shuichi Takano - original PicoDVI audio implementation
- `hsdaoh-rp2350` - HSTX data acquisition reference

# HSTX HDMI Audio Implementation Plan

Systematic analysis and implementation plan for adding HDMI audio to RP2350 HSTX video output.

## 1. Problem Decomposition

| Sub-problem | Description | Status |
|-------------|-------------|--------|
| **A. HDMI vs DVI** | DVI is video-only; HDMI adds audio via Data Islands | Understood |
| **B. Encoding modes** | Video uses TMDS (8b/10b), Data Islands use TERC4 (4b/10b) | Understood |
| **C. Timing windows** | Audio packets go in blanking periods only | Understood |
| **D. Packet types** | ACR, Audio Sample, Audio InfoFrame required | Understood |
| **E. Clock recovery** | N/CTS values let sink regenerate audio clock | Understood |
| **F. HSTX RAW mode** | Can HSTX output pre-encoded TERC4 via RAW? | **To verify** |

---

## 2. Protocol Background

### 2.1 HDMI vs DVI Feature Comparison

| Feature | DVI | HDMI |
|---------|-----|------|
| Video | TMDS encoded | TMDS encoded |
| Control periods | Control symbols | Control symbols |
| Data Islands | Not supported | TERC4 encoded |
| Audio | Not possible | Via Data Islands |

**Conclusion**: Audio requires HDMI-specific Data Island periods using TERC4 encoding.

### 2.2 Encoding Modes

| Mode | Input | Output | Used For |
|------|-------|--------|----------|
| **TMDS** | 8-bit value | 10-bit symbol | Active video pixels |
| **Control** | 2-bit code | 10-bit symbol | Sync signals (4 possible symbols) |
| **TERC4** | 4-bit value | 10-bit symbol | Data Islands (audio, InfoFrames) |

### 2.3 TERC4 Symbol Table

```c
// 4-bit input -> 10-bit output
uint16_t TERC4Syms[16] = {
    0b1010011100,  // 0
    0b1001100011,  // 1
    0b1011100100,  // 2
    0b1011100010,  // 3
    0b0101110001,  // 4
    0b0100011110,  // 5
    0b0110001110,  // 6
    0b0100111100,  // 7
    0b1011001100,  // 8
    0b0100111001,  // 9
    0b0110011100,  // 10
    0b1011000110,  // 11
    0b1010001110,  // 12
    0b1001110001,  // 13
    0b0101100011,  // 14
    0b1011000011,  // 15
};
```

### 2.4 Data Island Structure

```
One Data Island = 36 pixel clocks total:

┌──────────┬──────────────────────────────┬──────────┐
│ Guard    │ Packet Data (32 clocks)      │ Guard    │
│ Band (2) │ Header + 4 Subpackets        │ Band (2) │
└──────────┴──────────────────────────────┴──────────┘

Lane 0 (Blue):   [GB] [HB0 HB1 ... HB31] [GB]  (header bits + control)
Lane 1 (Green):  [GB] [SP0 SP1 ... SP31] [GB]  (subpacket even bits)
Lane 2 (Red):    [GB] [SP0 SP1 ... SP31] [GB]  (subpacket odd bits)
```

Guard Band symbol: `0b0100110011` (same on lanes 1 & 2)

### 2.5 Required Packet Types

| Packet Type | Header[0] | Purpose | Minimum Frequency |
|-------------|-----------|---------|-------------------|
| Audio Clock Regeneration (ACR) | 0x01 | N/CTS for clock recovery | Once per frame |
| Audio Sample | 0x02 | Actual audio data | ~800 per frame (48kHz) |
| Audio InfoFrame | 0x84 | Audio format metadata | Once per frame |

### 2.6 Audio Clock Regeneration

For 480p60 (25.175 MHz pixel clock) with 48kHz audio:

```
N = 6144 (standard value for 48kHz from HDMI spec)
CTS = N × f_pixel / (128 × f_audio)
CTS = 6144 × 25175000 / (128 × 48000) = 25175
```

The sink uses these to regenerate the audio clock:
```
f_audio = f_pixel × N / (128 × CTS)
```

---

## 3. SDK Video Pipeline Analysis

### 3.1 HSTX Command Modes

From `pico-examples/hstx/dvi_out_hstx_encoder`:

```c
#define HSTX_CMD_RAW         (0x0u << 12)   // Output pre-encoded 30-bit value
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)   // Output value N times
#define HSTX_CMD_TMDS        (0x2u << 12)   // Hardware TMDS encode pixels
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)   // TMDS encode and repeat
#define HSTX_CMD_NOP         (0xfu << 12)   // No operation
```

### 3.2 Current Blanking Line Structure

```c
// Vblank line (vsync inactive)
static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | 16,   // Front porch: 16 clocks
    SYNC_V1_H1,                  // Control symbol (vsync=1, hsync=1)
    HSTX_CMD_RAW_REPEAT | 96,   // Sync width: 96 clocks
    SYNC_V1_H0,                  // Control symbol (vsync=1, hsync=0)
    HSTX_CMD_RAW_REPEAT | 688,  // Back porch + active: 688 clocks
    SYNC_V1_H1,                  // Control symbol
    HSTX_CMD_NOP
};
```

### 3.3 Control Symbol Encoding

```c
// Pre-encoded TMDS control symbols (10 bits each)
#define TMDS_CTRL_00 0x354u  // hsync=0, vsync=0
#define TMDS_CTRL_01 0x0abu  // hsync=1, vsync=0
#define TMDS_CTRL_10 0x154u  // hsync=0, vsync=1
#define TMDS_CTRL_11 0x2abu  // hsync=1, vsync=1

// Combined 30-bit symbols (3 lanes)
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
```

**Key insight**: Lane 0 carries hsync/vsync in control symbols. Lanes 1 & 2 always use CTRL_00 during control periods.

---

## 4. Audio Integration Strategy

### 4.1 Available Blanking Windows

| Period | Duration (clocks) | Can fit Data Island? |
|--------|-------------------|---------------------|
| H Front Porch | 16 | No (need 36 min) |
| H Sync | 96 | Yes (2 islands = 72) |
| H Back Porch | 48 | Yes (1 island = 36) |
| V Blanking | 45 lines × 800 | Yes (primary location) |

**Strategy**: Insert Data Islands in horizontal back porch during vblank lines.

### 4.2 Modified Vblank Line with Data Island

```c
// Vblank line with one Data Island in back porch
static uint32_t vblank_line_with_data_island[] = {
    // Front porch (16 clocks) - unchanged
    HSTX_CMD_RAW_REPEAT | 16,
    SYNC_V1_H1,

    // Sync width (96 clocks) - unchanged
    HSTX_CMD_RAW_REPEAT | 96,
    SYNC_V1_H0,

    // Back porch part 1 (12 clocks of control)
    HSTX_CMD_RAW_REPEAT | 12,
    SYNC_V1_H1,

    // Data Island (36 clocks) - NEW
    HSTX_CMD_RAW | 36,           // Output 36 pre-encoded words
    data_island[0],               // Guard band
    data_island[1],               // Packet data...
    // ... 34 more words ...
    data_island[35],              // Guard band

    // Remaining blanking (640 clocks)
    HSTX_CMD_RAW_REPEAT | 640,
    SYNC_V1_H1,

    HSTX_CMD_NOP
};
```

### 4.3 Pre-encoded Data Island Word Format

For HSTX RAW mode, each 32-bit word contains 30 bits (3 lanes × 10 bits):

```c
uint32_t make_hstx_word(uint16_t lane0, uint16_t lane1, uint16_t lane2) {
    return (lane0 & 0x3FF) |
           ((lane1 & 0x3FF) << 10) |
           ((lane2 & 0x3FF) << 20);
}
```

---

## 5. Implementation Phases

### Phase 1: Verify HSTX_CMD_RAW Behavior

**Objective**: Confirm that `HSTX_CMD_RAW | N` outputs N sequential words correctly.

**Test**:
1. Create alternating pattern of two different control symbols
2. Output using `HSTX_CMD_RAW | N` followed by N words
3. Verify timing matches expected (N pixel clocks)

**Success criteria**: Video still displays correctly; oscilloscope shows expected pattern.

**Risk**: If HSTX_CMD_RAW doesn't work as expected, alternative approaches needed.

---

### Phase 2: Pre-encode Static Data Islands

**Objective**: Create correctly-formatted, pre-encoded Data Island buffers.

**Packets to encode**:

1. **Null packet** (for testing structure)
   - Header: 0x00, 0x00, 0x00
   - Subpackets: all zeros

2. **ACR packet** (Audio Clock Regeneration)
   - Header: 0x01, 0x00, 0x00
   - N = 6144, CTS = 25175

3. **Audio InfoFrame**
   - Header: 0x84, 0x01, 0x0A
   - 48kHz, 16-bit, stereo

**Validation**: Hex dump of encoded buffers matches expected TERC4 symbols.

---

### Phase 3: Insert Data Island in Vblank

**Objective**: Modify vblank line to include Data Island without breaking video.

**Steps**:
1. Create new vblank command list with Data Island slot
2. Replace one vblank line (e.g., line 5 of vblank)
3. Fill with Null packet initially

**Success criteria**:
- Video displays correctly (no corruption)
- Sink reports HDMI mode (not DVI) - check sink OSD or info display

**Validation method**:
- Visual inspection of video
- Sink device info screen (should show "HDMI" not "DVI")

---

### Phase 4: Add Required Audio Packets

**Objective**: Send ACR and Audio InfoFrame to enable audio subsystem.

**Steps**:
1. Send ACR packet once per frame (during vblank)
2. Send Audio InfoFrame once per frame
3. Continue sending Null packets for other data island slots

**Success criteria**:
- Sink detects audio capability
- Sink shows audio format info (48kHz, stereo)
- No audio output yet (expected - no samples sent)

**Validation**: Sink OSD/info shows audio detected.

---

### Phase 5: Add Silent Audio Samples

**Objective**: Send valid Audio Sample packets with zero data.

**Steps**:
1. Create Audio Sample packet with silence (0x0000 samples)
2. Send at appropriate rate (~800 packets per frame for 48kHz)
3. Distribute across available Data Island slots

**Success criteria**:
- Sink shows audio stream active
- No audio artifacts (pops, clicks)

**Audio sample rate calculation**:
```
Samples per frame = 48000 / 60 = 800 samples/frame
Samples per packet = 4 (max)
Packets per frame = 200 minimum
```

---

### Phase 6: Add Test Tone

**Objective**: Generate audible sine wave.

**Steps**:
1. Pre-compute 48kHz sine wave table (440 Hz)
2. Fill Audio Sample packets with actual data
3. Verify audio output

**Success criteria**: Audible 440 Hz tone from sink.

---

## 6. Known Unknowns

| Question | Impact | Resolution Method |
|----------|--------|-------------------|
| Does `HSTX_CMD_RAW \| N` work for sequential different values? | Critical | Phase 1 testing |
| HSTX FIFO depth - can it buffer 36+ words? | Critical | Phase 1 testing |
| DMA timing during Data Island insertion | Medium | Phase 3 testing |
| Sink compatibility with minimal Data Islands | Medium | Phase 3/4 testing |

---

## 7. Reference Implementations

### 7.1 PicoDVI (lib/PicoDVI/)

- Uses PIO-based TMDS encoding (not HSTX)
- Has working audio implementation
- Key files:
  - `software/libdvi/data_packet.c` - packet encoding
  - `software/libdvi/audio_ring.h` - audio buffer management

### 7.2 hsdaoh-rp2350 (reference/hsdaoh-rp2350/)

- Uses HSTX for video
- Has Data Island support (for metadata, not audio)
- Key files:
  - `libpicohsdaoh/data_packet.c` - TERC4 encoding
  - `libpicohsdaoh/picohsdaoh.c` - HSTX integration

### 7.3 Official SDK (reference/pico-examples/)

- HSTX DVI example (no audio)
- Authoritative for video pipeline
- Key file:
  - `hstx/dvi_out_hstx_encoder/dvi_out_hstx_encoder.c`

---

## 8. Success Metrics

| Phase | Metric | How to Verify |
|-------|--------|---------------|
| 1 | RAW mode timing correct | Oscilloscope/logic analyzer |
| 2 | Packets encode correctly | Hex comparison |
| 3 | Video unaffected by Data Islands | Visual inspection |
| 3 | Sink reports HDMI mode | Sink OSD |
| 4 | Audio capability detected | Sink OSD |
| 5 | Silent audio plays cleanly | No pops/clicks |
| 6 | Test tone audible | Human hearing |

---

## 9. Fallback Options

If HSTX cannot support Data Islands:

1. **Use PicoDVI instead** - Proven audio support, but uses PIO
2. **External HDMI transmitter IC** - Handles all encoding
3. **DVI-only mode** - Accept no audio capability

---

## Appendix A: Timing Calculations

### 480p60 Timing

```
Pixel clock:     25.175 MHz (or 25.2 MHz approximation)
H Total:         800 pixels (16 + 96 + 48 + 640)
V Total:         525 lines (10 + 2 + 33 + 480)
Frame rate:      25175000 / (800 × 525) = 59.94 Hz
```

### Audio Timing

```
Sample rate:     48000 Hz
Samples/frame:   48000 / 59.94 = 800.8 samples
Samples/line:    800.8 / 525 = 1.525 samples
```

Need approximately 1-2 Audio Sample packets per scanline during blanking.

### Data Island Capacity

```
Data Island size:        36 pixel clocks
H blanking available:    160 clocks (16 + 96 + 48)
Islands per H blank:     4 maximum (144 clocks)
V blank lines:           45
Total islands/frame:     45 × 4 = 180 (if using all vblank)
```

This exceeds the ~200 packets needed, but leaves no room in active lines. For robust implementation, also use horizontal blanking during active video period.

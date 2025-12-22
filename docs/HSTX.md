# Deep Analysis: HSTX for NeoPico-HD

## Executive Summary

HSTX (High-Speed Serial Transmit) is a dedicated peripheral on the RP2350 that could **dramatically simplify** the neopico-hd architecture. The key benefits are:

1. **Frees all 3 PIOs** for capture tasks
1. **Hardware TMDS encoding** - zero CPU overhead for video output
1. **No overclocking required** - runs at native 150 MHz
1. **Deterministic timing** - eliminates the fragile interrupt coordination
1. **Pre-computed data islands** - audio packets built once, DMA’d to output

However, there are significant implementation challenges, particularly around **HDMI audio** which requires porting data island encoding from existing projects.

-----

## Part 1: Current Architecture Pain Points

### The PicoDVI Complexity

Your current implementation uses the `ikjordan/PicoDVI` fork which has accumulated significant complexity:

```
Current Resource Usage:
┌─────────────────────────────────────────────────────────────────┐
│ PIO0 SM0-2   → DVI TMDS encoding (software, CPU-intensive)     │
│ PIO1 SM0     → MVS sync detection                               │
│ PIO1 SM1     → MVS pixel capture                                │
│ PIO2 SM0     → I2S audio capture                                │
│                                                                 │
│ Core 0       → MVS capture + audio processing (timing-critical) │
│ Core 1       → DVI output (IRQ-driven, also timing-critical)    │
│                                                                 │
│ System Clock → 252 MHz (overclocked for 480p audio)             │
└─────────────────────────────────────────────────────────────────┘
```

### Key Problems Documented in Your Code

From `AUDIO_VIDEO_INTEGRATION.md`:

1. **480p Required for Audio**: At 240p/126MHz, CPU can’t encode TMDS + data islands simultaneously
1. **No Timer Interrupts**: `add_repeating_timer_ms()` causes horizontal video trembling
1. **Vblank-Only Audio**: Audio buffer filling restricted to vsync wait periods
1. **Clock Mismatch Resolution**: Had to unify at 252 MHz

From `main.c`:

```c
// CRITICAL: All Core 0 audio work must happen during safe periods
// (vsync wait, vblank lines, after frame capture) to avoid 
// disrupting MVS capture timing.
```

This is fundamentally a **resource contention problem** - PicoDVI’s software TMDS encoding fights with your capture needs.

-----

## Part 2: What HSTX Actually Is

### Hardware Overview

HSTX is a **dedicated high-speed serial transmitter** on RP2350 with these features:

|Feature        |Specification                         |
|---------------|--------------------------------------|
|Output Pins    |GPIO 12-19 only (fixed)               |
|Max Clock      |150 MHz (300 MT/s in DDR mode)        |
|Data Width     |8 lanes (4 differential pairs for DVI)|
|Encoding       |**Hardware TMDS encoder** built-in    |
|Data Source    |DMA from memory, or direct from PIO   |
|CPU Involvement|**Zero** during output                |

### The Critical Difference: Hardware TMDS

PicoDVI does this in software:

```
Pixel → CPU encodes to 10-bit TMDS → PIO shifts out bits
        ↑
        This takes CPU cycles!
```

HSTX does this in hardware:

```
Pixel → HSTX hardware encodes to TMDS → Direct output
        ↑
        Zero CPU cycles!
```

### HSTX Command Expander

The HSTX has a **command expander** that interprets commands in the data stream:

|Command               |Function                                            |
|----------------------|----------------------------------------------------|
|`HSTX_CMD_RAW`        |Output pre-encoded 10-bit symbols (for data islands)|
|`HSTX_CMD_RAW_REPEAT` |Repeat a raw symbol N times                         |
|`HSTX_CMD_TMDS`       |Hardware TMDS encode 8-bit pixel data               |
|`HSTX_CMD_TMDS_REPEAT`|Repeat TMDS-encoded pixel                           |

This means:

- **Video pixels**: Feed 8-bit RGB, HSTX encodes to TMDS automatically
- **Data islands**: Pre-compute TERC4-encoded symbols, feed as RAW
- **Control periods**: Pre-compute sync symbols, feed as RAW_REPEAT

-----

## Part 3: HDMI Audio via HSTX - The Implementation Path

### What’s Already Proven

**Steve Markgraf’s hsdaoh-rp2350** proves HSTX can output HDMI with data islands:

> “hsdaoh-rp2350 is based on the dvi_out_hstx_encoder example, and code by
> Shuichi Takano implementing the HDMI data island encoding”

The key files from `hsdaoh-rp2350/libpicohsdaoh/`:

|File           |Purpose                                     |
|---------------|--------------------------------------------|
|`data_packet.c`|TERC4 encoding, BCH parity, packet structure|
|`picohsdaoh.c` |HSTX initialization, DMA command stream     |

### Data Island Structure (HDMI Spec)

Data islands carry audio and metadata during horizontal/vertical blanking:

```
┌──────────────────────────────────────────────────────────────────────┐
│                        HDMI Line Structure                           │
├──────────────────────────────────────────────────────────────────────┤
│ [Active Video] [Front Porch] [Sync] [Back Porch] [Active Video]...  │
│                      ↓                    ↓                          │
│               Data Islands          Data Islands                     │
│               (Audio here)          (InfoFrames)                     │
└──────────────────────────────────────────────────────────────────────┘

Data Island Structure (32 pixel times):
┌────────┬──────────┬────────┬────────────────────┬────────┐
│Guard(2)│Preamble(8)│Header(4)│ Subpackets (4×7) │Guard(2)│
│        │          │ bytes  │     bytes each    │        │
└────────┴──────────┴────────┴────────────────────┴────────┘
```

### Required Packet Types for Audio

|Packet Type      |Header Byte|Purpose                    |Frequency           |
|-----------------|-----------|---------------------------|--------------------|
|Audio Clock Regen|0x01       |N/CTS values for clock sync|Every frame         |
|Audio Sample     |0x02       |Actual PCM samples (L-PCM) |On data availability|
|Audio InfoFrame  |0x84       |Channel count, sample rate |Once per 2 frames   |
|AVI InfoFrame    |0x82       |Video format metadata      |Once per frame      |

### What Needs to be Ported

From Shuichi Takano’s `pico_lib` (C++), these functions need C ports:

```cpp
// Audio Sample Packet - carries actual audio data
int setAudioSample(const std::array<int16_t, 2> *p, int n, int frameCt);

// Audio InfoFrame - metadata about audio stream  
void setAudioInfoFrame(int channelCount, int sampleFreq, ...);

// Audio Clock Regeneration - CTS/N values for 48kHz sync
void setAudioClockRegeneration(uint32_t cts, uint32_t n);
```

The hsdaoh project **removed** these because it only needs AVI InfoFrame for video. They’d need to be re-added.

-----

## Part 4: Resource Comparison

### Current Architecture (PicoDVI)

```
┌─────────────────────────────────────────────────────────────────┐
│                        PicoDVI Architecture                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  PIO0 (gpio_base=16)         PIO1 (gpio_base=0)                 │
│  ┌─────────────────┐         ┌─────────────────┐                │
│  │ SM0: TMDS D0    │         │ SM0: MVS Sync   │                │
│  │ SM1: TMDS D1    │         │ SM1: MVS Pixel  │                │
│  │ SM2: TMDS D2    │         │ SM2: (free)     │                │
│  │ SM3: (free)     │         │ SM3: (free)     │                │
│  └────────┬────────┘         └─────────────────┘                │
│           │                                                      │
│           │ IRQ-driven                                          │
│           ↓                                                      │
│  ┌─────────────────┐         ┌─────────────────┐                │
│  │    Core 1       │         │    Core 0       │                │
│  │  DVI Output     │←────────│  MVS Capture    │                │
│  │  (scanline CB)  │ Shared  │  Audio Process  │                │
│  └─────────────────┘ Memory  └─────────────────┘                │
│                                                                  │
│  PIO2 (gpio_base=16)                                            │
│  ┌─────────────────┐                                            │
│  │ SM0: I2S Cap    │                                            │
│  └─────────────────┘                                            │
│                                                                  │
│  Clock: 252 MHz (overclocked)                                   │
│  DVI Pins: GPIO 25-32                                           │
└─────────────────────────────────────────────────────────────────┘
```

### Proposed HSTX Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        HSTX Architecture                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  HSTX (Hardware)              PIO0 (gpio_base=0)                │
│  ┌─────────────────┐         ┌─────────────────┐                │
│  │ GPIO 12-19      │         │ SM0: MVS Sync   │                │
│  │ HW TMDS Encode  │         │ SM1: MVS Pixel  │                │
│  │ DMA-fed         │         │ SM2: (free)     │                │
│  │ Zero CPU!       │         │ SM3: (free)     │                │
│  └────────┬────────┘         └─────────────────┘                │
│           │                                                      │
│           │ DMA only                                            │
│           ↓                                                      │
│  ┌─────────────────┐         ┌─────────────────┐                │
│  │   DMA Engine    │         │    Core 0       │                │
│  │  (autonomous)   │←────────│  MVS Capture    │                │
│  └─────────────────┘ Command │  Audio Process  │                │
│                      Buffer  │  (more headroom)│                │
│                              └─────────────────┘                │
│                                                                  │
│  PIO1 (gpio_base=16)         PIO2 (gpio_base=16)                │
│  ┌─────────────────┐         ┌─────────────────┐                │
│  │ SM0: (free)     │         │ SM0: I2S Cap    │                │
│  │ SM1: (free)     │         │ SM1: (free)     │                │
│  │ SM2: (free)     │         │ SM2: (free)     │                │
│  │ SM3: (free)     │         │ SM3: (free)     │                │
│  └─────────────────┘         └─────────────────┘                │
│                                                                  │
│  Clock: 150 MHz (native, no overclock)                          │
│  HSTX Pins: GPIO 12-19 (fixed)                                  │
│  Core 1: COMPLETELY FREE                                        │
└─────────────────────────────────────────────────────────────────┘
```

### Resource Freed by HSTX

|Resource  |PicoDVI            |HSTX      |Benefit                 |
|----------|-------------------|----------|------------------------|
|PIO0      |3 SMs for TMDS     |**Free**  |+3 SMs for capture/other|
|Core 1    |DVI IRQ handler    |**Free**  |Entire core available   |
|CPU Cycles|TMDS encoding      |**Zero**  |More headroom for audio |
|Clock     |252 MHz (overclock)|150 MHz   |More stable, less heat  |
|Timing    |IRQ-sensitive      |DMA-driven|No interrupt conflicts  |

-----

## Part 5: The Pin Conflict Problem

### HSTX Fixed Pinout

HSTX **must** use GPIO 12-19. There’s no flexibility here - it’s hardwired in silicon.

Standard HSTX DVI pinout (Pico-DVI-Sock compatible):

```
GPIO 12 - D2- (Lane 2 negative)
GPIO 13 - D2+ (Lane 2 positive)
GPIO 14 - CLK- (Clock negative)
GPIO 15 - CLK+ (Clock positive)
GPIO 16 - D1- (Lane 1 negative)
GPIO 17 - D1+ (Lane 1 positive)
GPIO 18 - D0- (Lane 0 negative)
GPIO 19 - D0+ (Lane 0 positive)
```

### Your Current Pin Conflict

```
Current neopico-hd pins:
GPIO 0:      MVS PCLK
GPIO 1-5:    MVS Green (5 bits)
GPIO 6-10:   MVS Blue (5 bits)
GPIO 11-15:  MVS Red (5 bits)    ← CONFLICTS with HSTX (12-15)
GPIO 22:     MVS CSYNC
GPIO 25-32:  DVI output (PicoDVI)
GPIO 36-38:  I2S audio
```

**GPIO 11-15 overlap with HSTX GPIO 12-15!**

### Solution: Remap MVS Red to Bank 1

Since you’re using RP2350B (48 GPIOs), you can move Red to high GPIOs:

```
Proposed HSTX-compatible pins:
GPIO 0:      MVS PCLK
GPIO 1-5:    MVS Green (5 bits)
GPIO 6-10:   MVS Blue (5 bits)
GPIO 11:     (free - buffer zone)
GPIO 12-19:  HSTX DVI output (fixed)
GPIO 20-21:  (free)
GPIO 22:     MVS CSYNC
GPIO 23-29:  (free)
GPIO 36-38:  I2S audio
GPIO 40-44:  MVS Red (5 bits) ← MOVED to Bank 1
```

### PIO gpio_base Considerations

For MVS capture with this new layout:

- Green (GPIO 1-5) and Blue (GPIO 6-10) are in Bank 0
- Red (GPIO 40-44) is in Bank 1
- PCLK (GPIO 0) is in Bank 0
- CSYNC (GPIO 22) is in Bank 0

**Challenge**: PIO can only access 32 consecutive GPIOs per `gpio_base` setting.

Options:

1. **Two PIOs for capture**: PIO0 (gpio_base=0) for PCLK+G+B+CSYNC, PIO1 (gpio_base=32) for Red
1. **Serial Red capture**: Capture R separately and recombine in software
1. **Different pin arrangement**: Put all RGB contiguous in Bank 1

Actually, a cleaner approach:

```
Alternative layout (all capture pins contiguous):
GPIO 0-11:   (free for other uses)
GPIO 12-19:  HSTX DVI (fixed)
GPIO 20:     MVS PCLK
GPIO 21-25:  MVS Green (5 bits)
GPIO 26-30:  MVS Blue (5 bits)
GPIO 31-35:  MVS Red (5 bits)
GPIO 36-38:  I2S audio
GPIO 39:     MVS CSYNC
```

With `gpio_base=16`, PIO can access GPIO 16-47, covering:

- PCLK (20)
- RGB (21-35)
- CSYNC (39)
- I2S (36-38)

All in one 32-pin window!

-----

## Part 6: Implementation Roadmap

### Phase 1: Validate HSTX Video Output (No Audio)

**Goal**: Get basic DVI video output working via HSTX

1. Use MichaelBell’s `dvhstx` library as starting point
1. Test with simple color bars
1. Verify timing with RetroTINK 4K

**Hardware needed**:

- Rewire DVI connector to HSTX pins (GPIO 12-19)
- Or use Adafruit HSTX-to-DVI adapter

**Code reference**: `pico-examples/hstx/dvi_out_hstx_encoder`

### Phase 2: Remap MVS Capture Pins

**Goal**: Move MVS capture to non-conflicting pins

1. Design new pin mapping (see above)
1. Update `mvs_capture.pio` for new pins
1. Update `neopico_config.h`
1. Test capture still works

**This requires hardware changes** - new wiring or new PCB.

### Phase 3: Port Data Island Encoding

**Goal**: Add HDMI InfoFrame support (no audio yet)

1. Port `data_packet.c` from hsdaoh-rp2350
1. Implement AVI InfoFrame
1. Test with HDMI analyzer or TV info display

**Code to port**:

```c
// From hsdaoh-rp2350/libpicohsdaoh/data_packet.c
void init_avi_infoframe(void);
void encode_terc4_packet(uint8_t *packet, uint8_t *output);
void compute_bch_parity(uint8_t *data, uint8_t *ecc);
```

### Phase 4: Add Audio Sample Packets

**Goal**: Full HDMI audio output

1. Port audio packet encoding from `pico_lib`
1. Implement Audio Clock Regeneration (N/CTS)
1. Implement Audio Sample Packets
1. Implement Audio InfoFrame
1. Connect to I2S capture pipeline

**This is the hardest part** - requires understanding HDMI audio packet structure.

### Phase 5: Integration and Optimization

**Goal**: Stable 60fps video + 48kHz audio

1. Tune DMA buffer sizes
1. Handle audio/video synchronization
1. Test with various HDMI sinks
1. Optimize memory usage

-----

## Part 7: Code Architecture Comparison

### Current PicoDVI Flow

```c
// Current main.c structure

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);  // IRQ handling
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);  // Never returns, IRQ-driven loop
}

void core1_scanline_callback(uint line_num) {
    // Called ~31.5kHz via IRQ
    // Must return quickly or video corrupts
    bufptr = &g_framebuf[FRAME_WIDTH * scanline];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
}

// Main loop on Core 0
while (true) {
    // Must carefully avoid IRQ conflicts
    wait_for_vsync();
    fill_audio_buffer();  // Only during safe periods!
    capture_frame();
    fill_audio_buffer();  // Again, only when safe!
}
```

### Proposed HSTX Flow

```c
// Proposed HSTX structure

// No Core 1 code needed!

// Pre-computed command buffer (built once at init)
static uint32_t hstx_cmd_buffer[CMD_BUFFER_SIZE];

void init_hstx_dvi(void) {
    // Build command stream with video/audio data islands
    build_frame_commands(hstx_cmd_buffer);
    
    // Setup DMA to feed HSTX
    dma_channel_configure(
        hstx_dma_chan,
        &dma_config,
        &hstx_hw->fifo,      // Destination: HSTX FIFO
        hstx_cmd_buffer,     // Source: Command buffer
        CMD_BUFFER_SIZE,
        true                 // Start immediately
    );
    
    // HSTX runs autonomously from here
}

// Main loop - much simpler!
while (true) {
    wait_for_vsync();
    capture_frame_to_buffer();
    
    // Update audio portion of command buffer
    update_audio_packets(current_audio_samples);
    
    // That's it! No IRQ coordination needed.
}
```

### Key Simplification

|Aspect           |PicoDVI                 |HSTX             |
|-----------------|------------------------|-----------------|
|IRQ handlers     |Complex, timing-critical|None             |
|Core 1 code      |Dedicated to DVI        |Not needed       |
|Scanline callback|Must be fast            |Not needed       |
|Audio timing     |Vblank-restricted       |Any time         |
|Queue management |Multi-queue coordination|Single DMA buffer|

-----

## Part 8: Risk Assessment

### Low Risk

|Item             |Why Low Risk                        |
|-----------------|------------------------------------|
|HSTX video output|Proven in multiple projects         |
|Hardware TMDS    |Built into silicon, well-documented |
|DMA feeding      |Standard RP2350 DMA, well-understood|

### Medium Risk

|Item               |Mitigation                                 |
|-------------------|-------------------------------------------|
|Pin remapping      |Requires new PCB, but straightforward      |
|AVI InfoFrame      |Code exists in hsdaoh, needs porting       |
|Library integration|MichaelBell’s dvhstx is actively maintained|

### High Risk

|Item                   |Challenge                                 |
|-----------------------|------------------------------------------|
|Audio packet porting   |Code exists but in C++, needs careful port|
|Audio sync (N/CTS)     |Requires precise timing calculation       |
|HDMI sink compatibility|Some TVs picky about audio packets        |
|Development time       |Significant effort to implement           |

-----

## Part 9: Alternative Approaches

### Option A: HSTX with Full Audio (Recommended)

**Effort**: High (4-6 weeks)
**Benefit**: Dramatic simplification, freed resources
**Risk**: Medium-High

### Option B: HSTX Video Only, Keep PicoDVI Audio Path

**Effort**: Medium (2-3 weeks)
**Benefit**: Simpler video output
**Risk**: Medium
**Downside**: Still need complex audio coordination

### Option C: External HDMI Transmitter (IT6615E)

**Effort**: Medium (3-4 weeks)
**Benefit**: Offload all HDMI complexity to dedicated chip
**Risk**: Low-Medium
**Downside**: Another chip, I2C configuration, cost

From your `ITE_IT6615E.md` doc, this chip handles:

- HDMI 2.0b with HDCP
- I2S audio input directly
- All InfoFrame generation
- No software TMDS needed

### Option D: Stay with PicoDVI

**Effort**: None
**Benefit**: Already working
**Risk**: None
**Downside**: Current limitations remain (252MHz, timing fragility)

-----

## Part 10: Recommendation

### For Maximum Benefit: Option A (Full HSTX)

If you’re willing to invest the development time, HSTX offers the cleanest long-term architecture:

1. **Freed Core 1** - Could use for advanced audio processing, USB, etc.
1. **No overclocking** - More reliable, less heat
1. **Simpler timing** - No IRQ juggling
1. **All PIOs free** - Future expansion possibilities

### For Fastest Results: Option C (IT6615E)

If you want HDMI audio without the software complexity:

1. IT6615E handles all HDMI encoding
1. Feed it parallel RGB + I2S audio
1. Configure via I2C
1. Let it generate all packets

This is how professional products do it - dedicated silicon.

### My Suggestion

Given your documented pain points with PicoDVI timing, I’d recommend:

1. **Short term**: Continue with current PicoDVI (it works)
1. **Medium term**: Prototype HSTX video output on a breadboard
1. **Long term**: Design new PCB with HSTX pinout + evaluate IT6615E

The HSTX path is the most elegant for a DIY project, but the IT6615E might be more practical if you want “just works” audio.

-----

## Appendix A: HSTX Register Reference

Key HSTX registers (from RP2350 datasheet):

|Register         |Offset|Description                     |
|-----------------|------|--------------------------------|
|HSTX_CTRL        |0x00  |Main control (enable, clock div)|
|HSTX_BIT_ORDER   |0x04  |Lane bit ordering               |
|HSTX_EXPAND_SHIFT|0x08  |Command expander config         |
|HSTX_EXPAND_TMDS |0x0C  |TMDS encoding config            |
|HSTX_FIFO        |0x10  |Write data/commands here        |

## Appendix B: HDMI Audio Packet Reference

Audio Sample Packet (Type 0x02):

```
Header: [0x02] [B3:B0=layout, B7:B4=sample_present]
Subpacket 0: [sample_L_7:0] [sample_L_15:8] [sample_R_7:0] [sample_R_15:8] [0] [0] [0]
Subpacket 1-3: (additional samples or padding)
```

Audio Clock Regeneration (Type 0x01):

```
Header: [0x01] [0x00]
Subpacket 0: [CTS_19:16] [CTS_15:8] [CTS_7:0] [N_19:16] [N_15:8] [N_7:0] [0]
```

For 48kHz @ 25.2MHz pixel clock:

- N = 6144
- CTS = 25200

## Appendix C: Useful Code References

|Project             |URL                              |Relevant Code            |
|--------------------|---------------------------------|-------------------------|
|hsdaoh-rp2350       |github.com/steve-m/hsdaoh-rp2350 |HSTX + data islands      |
|dvhstx              |github.com/MichaelBell/dvhstx    |Clean HSTX DVI library   |
|pico_lib            |github.com/shuichitakano/pico_lib|HDMI audio (C++)         |
|PicoDVI             |github.com/Wren6991/PicoDVI      |Current implementation   |
|dvi_out_hstx_encoder|pico-examples                    |Official HSTX DVI example|
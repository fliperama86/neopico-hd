# TMDS, HDMI and DVI Digital Signaling for Dummies

A plain-English guide to understanding digital video signals.

## The Big Picture

```
┌─────────┐    3 data lanes + 1 clock    ┌─────────┐
│  Source │ ═══════════════════════════> │ Display │
│  (Pico) │   differential pairs (±)     │  (TV)   │
└─────────┘                              └─────────┘
```

Your video source sends pixels as serialized bits over 4 differential pairs:
- **D0**: Blue + HSync + VSync
- **D1**: Green
- **D2**: Red
- **CLK**: Pixel clock (so display knows when to sample)

## What is TMDS?

**Transition-Minimized Differential Signaling**

TMDS is the encoding scheme used by both DVI and HDMI. It converts 8-bit color values into 10-bit symbols.

### Why 10 bits instead of 8?

1. **DC Balance**: Prevents charge buildup in the cable
2. **Transition Minimization**: Reduces electromagnetic interference
3. **Control Codes**: Extra symbols for sync signals

```
8-bit pixel ──► TMDS Encoder ──► 10-bit symbol ──► Serializer ──► Wire
   0x7F              │              0x17E              │
                     └── Magic encoding ──────────────┘
```

## Differential Signaling (The ± Pairs)

Each signal uses TWO wires carrying opposite voltages:

```
D0+ ───────╱╲╱╲╱╲───────  (signal)
D0- ───────╲╱╲╱╲╱───────  (inverted signal)
```

The receiver looks at the **difference** between them. This cancels out noise that affects both wires equally.

That's why pinouts show pairs: `D0+/D0-`, `CLK+/CLK-`, etc.

## The Speed Math

For any resolution:

```
Pixel Clock × 10 bits = TMDS Bit Rate (per lane)
```

| Resolution | Pixel Clock | TMDS per Lane | Example |
|------------|-------------|---------------|---------|
| 480p60 | 25.2 MHz | 252 Mbps | RP2350 HSTX ✓ |
| 720p60 | 74.25 MHz | 742.5 Mbps | Needs dedicated chip |
| 1080p60 | 148.5 MHz | 1.485 Gbps | Needs dedicated chip |

**Critical insight**: You cannot split one lane across multiple wires. Each lane must independently hit its bit rate.

## DVI vs HDMI

They use the **same TMDS signaling**. The differences:

| Feature | DVI | HDMI |
|---------|-----|------|
| Video | ✓ | ✓ |
| Audio | ✗ | ✓ (via Data Islands) |
| Connector | Big white plug | Smaller, many variants |
| Copy Protection | Optional | HDCP common |

An HDMI display will happily accept a DVI signal (no audio, but video works fine).

## Data Islands (HDMI Audio)

HDMI sneaks audio into the horizontal/vertical blanking periods:

```
│◄── Active Video ──►│◄─ Blanking ─►│◄── Active Video ──►│
│    (pixels)        │ DATA ISLAND  │    (pixels)        │
│                    │ (audio here) │                    │
```

During blanking, instead of sending control symbols, HDMI sends **TERC4-encoded** packets containing:
- Audio samples
- InfoFrames (metadata)

This is why HDMI audio is complex - you need precise timing to insert packets during blanking.

## Control Periods vs Active Video

TMDS has two modes:

**Active Video**: Sending pixel data (8b→10b TMDS encoded)

**Control Period**: Sending sync signals using special 10-bit codes:

| Symbol | D0 bits | Meaning |
|--------|---------|---------|
| CTRL0 | 0b1101010100 | HSync=0, VSync=0 |
| CTRL1 | 0b0010101011 | HSync=1, VSync=0 |
| CTRL2 | 0b0101010100 | HSync=0, VSync=1 |
| CTRL3 | 0b1010101011 | HSync=1, VSync=1 |

These symbols are designed to be easily distinguishable from video data.

## Putting It Together: One Scanline

```
│◄────────────── One Horizontal Line ──────────────────►│

┌────────┬────────┬───────────┬─────────────────────────┐
│ Front  │ Sync   │ Back      │ Active Pixels           │
│ Porch  │ Pulse  │ Porch     │ (visible image)         │
├────────┼────────┼───────────┼─────────────────────────┤
│ CTRL   │ CTRL   │ CTRL      │ TMDS-encoded pixels     │
│ symbols│ symbols│ symbols   │ (8b→10b each)           │
└────────┴────────┴───────────┴─────────────────────────┘
     ▲                              ▲
     │                              │
  Control Period               Active Video
  (sync timing)                (actual image)
```

## Why This Matters for RP2350

The HSTX peripheral can:
- ✓ Hardware TMDS encode 8-bit values
- ✓ Output pre-encoded control symbols
- ✓ Run at up to 300 Mbps per lane
- ✗ Cannot reach 720p speeds (742.5 Mbps needed)

For higher resolutions, you need a dedicated HDMI transmitter IC that handles TMDS internally.

## Glossary

| Term | Meaning |
|------|---------|
| **TMDS** | Transition-Minimized Differential Signaling |
| **Lane** | One color channel (D0=Blue, D1=Green, D2=Red) |
| **Differential Pair** | Two wires carrying inverted signals (+/-) |
| **Pixel Clock** | Rate at which pixels are sent |
| **Bit Clock** | Pixel clock × 10 (TMDS symbol rate) |
| **Data Island** | Audio/metadata packets during blanking |
| **TERC4** | 4-bit to 10-bit encoding for data islands |
| **InfoFrame** | Metadata packet (resolution, audio format, etc.) |
| **Blanking** | Non-visible portion of video signal (sync timing) |

## Further Reading

- [DVI Specification](https://glenwing.github.io/docs/DVI-1.0.pdf)
- [HDMI 1.4 Overview](https://www.hdmi.org/spec/hdmi1_4)
- [RP2350 Datasheet - HSTX Chapter](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)

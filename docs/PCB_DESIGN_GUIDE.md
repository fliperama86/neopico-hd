# NeoPico-HD PCB Design Guide

Reference design for KiCad, targeting JLCPCB manufacturing.

## Design Goals

1. **Isolate video and audio I/O banks** to prevent switching noise coupling
2. **Proper power distribution** with per-domain filtering
3. **Signal integrity** for high-speed DVI and I2S signals
4. **Manufacturable** with standard JLCPCB capabilities (4-layer recommended)

---

## Block Diagram

```
                            ┌─────────────────────────────────────────────────────────┐
                            │                    NeoPico-HD PCB                        │
                            │                                                          │
   MVS VIDEO INPUT          │    ┌─────────────────────────────────────────────┐      │
   ══════════════           │    │            RP2350B MODULE                   │      │
                            │    │         (WeAct Studio Board)                │      │
  ┌──────────────┐          │    │                                             │      │
  │ 2x10 Header  │          │    │  ┌─────────┐          ┌─────────┐          │      │
  │              │          │    │  │ BANK 0  │          │ BANK 1  │          │      │
  │ PCLK ────────┼──────────┼────┼─►│GPIO 0-29│          │GPIO30-47│◄─────────┼──────┼──── I2S INPUT
  │ R0-R4 ───────┼──────────┼────┼─►│         │          │         │          │      │    (BCK,DAT,WS)
  │ G0-G4 ───────┼──────────┼────┼─►│  VIDEO  │          │  AUDIO  │          │      │
  │ B0-B4 ───────┼──────────┼────┼─►│ CAPTURE │          │ CAPTURE │          │      │
  │ CSYNC ───────┼──────────┼────┼─►│         │          │         │          │      │
  │              │          │    │  └────┬────┘          └────┬────┘          │      │
  │ GND ─────────┼────┐     │    │       │                    │               │      │
  │ 5V ──────────┼──┐ │     │    │       │    ┌──────────┐    │               │      │     HDMI OUTPUT
  └──────────────┘  │ │     │    │       └───►│  CORE 0  │◄───┘               │      │     ═══════════
                    │ │     │    │            │  CORE 1  │                    │      │
                    │ │     │    │            │          │────────────────────┼──────┼───► HDMI Conn
                    │ │     │    │            │  DVI OUT │  GPIO 25-32        │      │     (Active)
                    │ │     │    │            └──────────┘                    │      │
                    │ │     │    │                                             │      │
                    │ │     │    └─────────────────────────────────────────────┘      │
                    │ │     │                                                          │
                    │ │     │         POWER DISTRIBUTION                               │
                    │ │     │    ┌────────────────────────────────────────────────┐   │
                    │ │     │    │                                                │   │
                    │ │     │    │   5V ──┬──[FB1]──┬──[FB2]──► 5V_VIDEO          │   │
                    │ │     │    │        │         │                             │   │
                    │ │     │    │        │         └──[FB3]──► 5V_AUDIO          │   │
                    │ │     │    │        │                                       │   │
                    │ │     └────┼────────┴─────────────────────► GND_STAR        │   │
                    │ │          │                                                │   │
                    │ └──────────┼────► 5V_IN                                     │   │
                    │            │                                                │   │
                    └────────────┼────► GND_IN                                    │   │
                                 │                                                │   │
                                 └────────────────────────────────────────────────┘   │
                                                                                       │
                            └─────────────────────────────────────────────────────────┘
```

---

## Recommended Layer Stackup (4-Layer)

```
┌─────────────────────────────────────────────────────────────┐
│  Layer 1 (Top)      - Signal + Components                   │
│                       Route video signals here              │
├─────────────────────────────────────────────────────────────┤
│  Layer 2 (Inner 1)  - GND PLANE (solid, unbroken)          │
│                       This is critical for noise immunity   │
├─────────────────────────────────────────────────────────────┤
│  Layer 3 (Inner 2)  - POWER PLANE (3V3 + 5V split)         │
│                       Split between video/audio domains     │
├─────────────────────────────────────────────────────────────┤
│  Layer 4 (Bottom)   - Signal + Components                   │
│                       Route audio + DVI signals here        │
└─────────────────────────────────────────────────────────────┘

JLCPCB 4-layer stackup (JLC04161H-7628):
- Total thickness: 1.6mm
- Copper: 1oz outer, 0.5oz inner
- Dielectric: FR-4
```

---

## Component Placement (Top View)

```
    ┌────────────────────────────────────────────────────────────────────────────┐
    │                                                                            │
    │   ┌─────────┐                                              ┌─────────┐    │
    │   │ C_BULK  │    VIDEO DOMAIN                              │ C_BULK  │    │
    │   │  100µF  │    ════════════                              │  100µF  │    │
    │   └─────────┘                                              └─────────┘    │
    │        │                                                        │         │
    │   ┌────┴────┐                                              ┌────┴────┐    │
    │   │  FB1    │                                              │  FB3    │    │
    │   │ Ferrite │                                              │ Ferrite │    │
    │   └────┬────┘                                              └────┬────┘    │
    │        │                                                        │         │
    │   ═════╪════════════════════════════════════════════════════════╪═════    │
    │        │              ACTIVE HDMI MODULE                        │         │
    │        │         ┌─────────────────────────┐                    │         │
    │        │         │    ┌───────────────┐    │                    │         │
    │        │         │    │               │    │         AUDIO DOMAIN         │
    │ ┌──────┴──────┐  │    │               │    │         ════════════         │
    │ │             │  │    │   RP2350B     │    │                    │         │
    │ │   VIDEO     │  │    │   MODULE      │    │              ┌─────┴─────┐   │
    │ │  CONNECTOR  │  │    │               │    │              │   AUDIO   │   │
    │ │   2x10      │  │    │  (WeAct or    │    │              │ CONNECTOR │   │
    │ │             │  │    │   similar)    │    │              │   1x4     │   │
    │ │  R4 R3 R2 R1│R0│    │               │    │              │           │   │
    │ │  G4 G3 G2 G1│G0│    │               │    │              │ BCK DAT WS│GND│
    │ │  B4 B3 B2 B1│B0│    │               │    │              │           │   │
    │ │  NC CS PC NC│5V│    │               │    │              └───────────┘   │
    │ │  GND  ...  GND│    │               │    │                    │         │
    │ └───────────────┘  │    │               │    │              ┌─────┴─────┐   │
    │        │           │    └───────────────┘    │              │ 0.1µ+10µ  │   │
    │   ┌────┴────┐      │                         │              │ Decoupl   │   │
    │   │0.1µ+10µ │      │    GPIO 25-32 ──────────┼─────►HDMI    └───────────┘   │
    │   │Decoupl  │      │                         │                              │
    │   └─────────┘      └─────────────────────────┘                              │
    │                                                                             │
    │   ┌─────────────────────────────────────────────────────────────────────┐  │
    │   │                        5V POWER INPUT                                │  │
    │   │   [+5V]  [GND]  [GND]  [GND]                                        │  │
    │   │     │      │      │      │     ◄── Multiple GND pins for low Z      │  │
    │   └─────┴──────┴──────┴──────┴──────────────────────────────────────────┘  │
    │                                                                             │
    └─────────────────────────────────────────────────────────────────────────────┘
```

---

## GPIO Assignment (RP2350B)

### Bank 0 - Video Domain (GPIO 0-29)
```
Signal      GPIO    Notes
──────────────────────────────────────────
PCLK        0       6MHz pixel clock input
G4          1       Green MSB
G3          2
G2          3
G1          4
G0          5       Green LSB
B0          6       Blue LSB
B1          7
B2          8
B3          9
B4          10      Blue MSB
R0          11      Red LSB
R1          12
R2          13
R3          14
R4          15      Red MSB
CSYNC       22      Directly to PIO

DVI_D0+     25      \
DVI_D0-     26       \
DVI_D1+     27        \  DVI differential
DVI_D1-     28        /  pairs
DVI_D2+     29       /
```

### Bank 1 - Audio Domain (GPIO 30-47)
```
Signal      GPIO    Notes
──────────────────────────────────────────
DVI_D2-     30      DVI pair (straddles banks)
DVI_CLK+    31      DVI clock
DVI_CLK-    32      DVI clock
BCK         33      I2S bit clock - DIRECTLY TO PIO
DAT         34      I2S data
WS          35      I2S word select (L/R)
```

**Note**: DVI signals span both banks but that's acceptable - they're outputs, not inputs affected by video switching noise.

---

## Power Distribution Schematic

```
                                    ┌──────────────────────────────────────────┐
                                    │           VIDEO POWER DOMAIN             │
                                    │                                          │
        5V_IN ──┬──────────────────►├──[FB1 220Ω@100MHz]──┬──► 5V_VIDEO        │
                │                   │                      │                    │
                │                   │                 ┌────┴────┐               │
                │                   │                 │ C: 100µF│               │
                │                   │                 │ (Bulk)  │               │
                │                   │                 └────┬────┘               │
                │                   │                      │                    │
                │                   │    Per-IC Decoupling │                    │
                │                   │    ┌─────────────────┼─────────────────┐  │
                │                   │    │                 │                 │  │
                │                   │    │  ┌──┴──┐   ┌──┴──┐   ┌──┴──┐    │  │
                │                   │    │  │0.1µF│   │0.1µF│   │0.1µF│    │  │
                │                   │    │  └──┬──┘   └──┬──┘   └──┬──┘    │  │
                │                   │    │     │         │         │        │  │
                │                   │    │   [VCC]     [VCC]     [VCC]      │  │
                │                   │    │   U1        U2        U3         │  │
                │                   │    │  (74LVC)   (74LVC)   (Buffer)    │  │
                │                   │    └─────────────────────────────────┘  │
                │                   └──────────────────────────────────────────┘
                │
                │                   ┌──────────────────────────────────────────┐
                │                   │           AUDIO POWER DOMAIN             │
                │                   │                                          │
                ├──────────────────►├──[FB2 220Ω@100MHz]──┬──► 5V_AUDIO        │
                │                   │                      │                    │
                │                   │                 ┌────┴────┐               │
                │                   │                 │ C: 100µF│               │
                │                   │                 │ (Bulk)  │               │
                │                   │                 └────┬────┘               │
                │                   │                      │                    │
                │                   │         ┌──┴──┐   ┌──┴──┐                │
                │                   │         │10µF │   │0.1µF│                │
                │                   │         └──┬──┘   └──┬──┘                │
                │                   │            └────┬────┘                    │
                │                   │                 │                         │
                │                   │           To Audio Input                  │
                │                   │           Connector VCC                   │
                │                   └──────────────────────────────────────────┘
                │
                │                   ┌──────────────────────────────────────────┐
                │                   │           RP2350B MODULE POWER           │
                └──────────────────►├──[FB3 220Ω@100MHz]──┬──► VSYS (Module)   │
                                    │                      │                    │
                                    │                 ┌────┴────┐               │
                                    │                 │ C: 100µF│               │
                                    │                 │ (Bulk)  │               │
                                    │                 └────┬────┘               │
                                    │                      │                    │
                                    │    Module has        │                    │
                                    │    onboard 3V3       │                    │
                                    │    regulator         │                    │
                                    └──────────────────────────────────────────┘

GND_IN ─────────────────────────────────────────────────────────────────────────►
                                    │
                              SOLID GND PLANE
                            (Layer 2, unbroken)
```

---

## Decoupling Capacitor Placement

```
CRITICAL: Place decoupling caps as close as possible to IC power pins.
Via directly to ground plane beneath cap.

    CORRECT                          WRONG
    ════════                         ═════

      VCC trace                        VCC trace
         │                                │
    ┌────┴────┐                     ┌─────┴─────────────┐
    │   0.1µF │◄── Short trace      │                   │
    └────┬────┘    to VCC pin       │    Long trace     │
         │                          │         │         │
        ═╧═ Via to GND plane        │    ┌────┴────┐    │
                                    │    │  0.1µF  │    │
      ┌─────┐                       │    └────┬────┘    │
      │ IC  │                       │         │         │
      │     │                       │        ═╧═        │
      └─────┘                       │                   │
                                    │    ┌─────┐        │
                                    │    │ IC  │        │
                                    │    └─────┘        │
                                    └───────────────────┘

Decoupling strategy per power pin:
- 0.1µF ceramic (high frequency) - MUST be within 3mm of pin
- 10µF ceramic (mid frequency) - within 10mm
- 100µF electrolytic (bulk) - per power domain
```

---

## Signal Routing Guidelines

### Video Signals (PCLK, RGB, CSYNC)
```
┌─────────────────────────────────────────────────────────────────┐
│ • Route on TOP layer                                            │
│ • Keep traces parallel, matched length not critical (6MHz)      │
│ • Trace width: 0.2mm minimum (8mil)                            │
│ • Spacing: 0.2mm minimum between traces                         │
│ • Ground plane directly beneath (Layer 2)                       │
│ • Keep away from audio signals (>5mm separation)                │
└─────────────────────────────────────────────────────────────────┘
```

### Audio Signals (BCK, DAT, WS)
```
┌─────────────────────────────────────────────────────────────────┐
│ • Route on BOTTOM layer (opposite side from video)              │
│ • BCK is most critical - shortest possible trace                │
│ • Trace width: 0.2mm minimum                                    │
│ • Add ground guard traces on both sides if space permits:       │
│                                                                 │
│     GND ═══════════════════════════════                        │
│     BCK ───────────────────────────────                        │
│     GND ═══════════════════════════════                        │
│                                                                 │
│ • Consider 33Ω series resistor on BCK for impedance matching    │
└─────────────────────────────────────────────────────────────────┘
```

### DVI Signals (Differential Pairs)
```
┌─────────────────────────────────────────────────────────────────┐
│ • Route as differential pairs with 100Ω differential impedance  │
│ • Use KiCad's differential pair routing tool                    │
│ • Trace width: ~0.15mm, spacing: ~0.2mm (calculator dependent)  │
│ • Match lengths within each pair (±0.5mm)                       │
│ • Keep pairs separated from each other by 3x trace width        │
│ • Route on TOP layer, direct to HDMI connector                  │
│ • Minimize vias - if needed, use via pairs                      │
│                                                                 │
│   D2+ ════╦══════════════════════════════════════╗              │
│   D2- ════╩══════════════════════════════════════╝              │
│           ↑                                                     │
│      Tight coupling                                             │
└─────────────────────────────────────────────────────────────────┘
```

---

## Connector Pinouts

### Video Input (2x10 Pin Header, 2.54mm)
```
Active HDMI Module facing right:

          ┌─────────────────────┐
    R4  1 │ ●                 ● │ 2  R3
    R2  3 │ ●                 ● │ 4  R1
    R0  5 │ ●                 ● │ 6  G4
    G3  7 │ ●                 ● │ 8  G2
    G1  9 │ ●                 ● │ 10 G0
   B4  11 │ ●                 ● │ 12 B3
   B2  13 │ ●                 ● │ 14 B1
   B0  15 │ ●                 ● │ 16 CSYNC
  GND  17 │ ●                 ● │ 18 PCLK
  GND  19 │ ●                 ● │ 20 5V
          └─────────────────────┘

Wire colors (suggested, matching MVS edge):
- Red signals: Red wires
- Green signals: Green wires
- Blue signals: Blue wires
- PCLK: Yellow
- CSYNC: White
- 5V: Red (thick)
- GND: Black (thick)
```

### Audio Input (1x5 Pin Header, 2.54mm)
```
    ┌─────────────────┐
    │ ● ● ● ● ●       │
    │ 1 2 3 4 5       │
    └─────────────────┘
      │ │ │ │ │
      │ │ │ │ └── GND
      │ │ │ └──── 5V (optional, for external buffer)
      │ │ └────── WS  (GPIO 35)
      │ └──────── DAT (GPIO 34)
      └────────── BCK (GPIO 33)
```

### Power Input (Screw Terminal or Barrel Jack)
```
    ┌─────────┐
    │ +  -  - │  ◄── Dual GND for lower impedance
    │ 5V G  G │
    └─────────┘
```

---

## Bill of Materials (Key Components)

| Ref | Value | Package | LCSC Part# | Notes |
|-----|-------|---------|------------|-------|
| FB1,FB2,FB3 | 220Ω@100MHz | 0603 | C1015 | MPZ1608S221A or equiv |
| C1,C2,C3 | 100µF/10V | 0805 or larger | C15008 | Bulk, per domain |
| C4-C10 | 10µF/10V | 0603 | C19702 | Ceramic X5R |
| C11-C20 | 0.1µF/16V | 0402 | C1525 | Ceramic, decoupling |
| J1 | 2x10 Header | 2.54mm | - | Video input |
| J2 | 1x5 Header | 2.54mm | - | Audio input |
| J3 | HDMI Type A | - | - | Or use active module |
| R1-R3 | 33Ω | 0402 | C25105 | Optional series termination |

---

## KiCad Design Rules (JLCPCB Compatible)

```
Minimum trace width:     0.127mm (5mil) - use 0.2mm for signals
Minimum spacing:         0.127mm (5mil) - use 0.2mm for safety
Minimum via drill:       0.3mm
Minimum via diameter:    0.6mm (0.3mm drill + 0.15mm annular ring x2)
Minimum hole size:       0.3mm
Copper to edge:          0.3mm minimum

Recommended trace widths:
- Power traces (5V, 3V3):  0.5mm - 1.0mm
- Signal traces:           0.2mm - 0.25mm
- DVI differential:        Per impedance calculator (~0.15mm)
```

---

## Design Checklist

### Schematic
- [ ] All power pins have decoupling caps
- [ ] Ferrite beads between power domains
- [ ] GPIO assignments match Bank 0/Bank 1 separation
- [ ] ERC passes with no errors

### Layout
- [ ] Solid GND plane on Layer 2 (no splits under signals)
- [ ] Power plane split for video/audio domains
- [ ] Decoupling caps within 3mm of IC power pins
- [ ] Video signals routed on top layer
- [ ] Audio signals routed on bottom layer
- [ ] DVI pairs routed as differential with length matching
- [ ] No traces crossing plane splits
- [ ] Via stitching around board edges

### Manufacturing
- [ ] DRC passes for JLCPCB rules
- [ ] Gerbers generated (F.Cu, B.Cu, In1.Cu, In2.Cu, F.SilkS, B.SilkS, F.Mask, B.Mask, Edge.Cuts, PTH, NPTH)
- [ ] Pick-and-place file if using assembly service
- [ ] BOM matches LCSC part numbers

---

## Ground Plane Visualization

```
Layer 2 (GND) - KEEP SOLID, NO CUTS

    ┌─────────────────────────────────────────────────────────────┐
    │█████████████████████████████████████████████████████████████│
    │█████████████████████████████████████████████████████████████│
    │█████████████████████████████████████████████████████████████│
    │███████████████████████████████████████████████████████  ████│
    │███████████████████████████████████████████████████████  ████│◄─ Small
    │███████████████████████████████████████████████████████  ████│   clearance
    │███████████████████████████████████████████████████████  ████│   for HDMI
    │█████████████████████████████████████████████████████████████│   only
    │█████████████████████████████████████████████████████████████│
    │█████████████████████████████████████████████████████████████│
    │█████████████████████████████████████████████████████████████│
    │█████████████████████████████████████████████████████████████│
    └─────────────────────────────────────────────────────────────┘

    ███ = Copper fill (ground)

    DO NOT split ground plane between video/audio!
    Split only the POWER plane.
```

---

## Power Plane Split (Layer 3)

```
Layer 3 (Power) - SPLIT BY DOMAIN

    ┌─────────────────────────────────────────────────────────────┐
    │                              │                              │
    │                              │                              │
    │        5V_VIDEO              │         5V_AUDIO             │
    │                              │                              │
    │     ████████████████         │        ████████████          │
    │     ████████████████         │        ████████████          │
    │     ████████████████    ◄────┼────►   ████████████          │
    │     ████████████████   Gap   │  Gap   ████████████          │
    │     ████████████████  (2mm)  │ (2mm)  ████████████          │
    │                              │                              │
    │──────────────────────────────┼──────────────────────────────│
    │                              │                              │
    │              3V3 (from module, local decoupling)            │
    │                              │                              │
    └─────────────────────────────────────────────────────────────┘

    Power domains are isolated by 2mm gaps.
    Ferrite beads bridge the gaps at single points.
```

---

## References

- cps2_digiav hardware design: `reference/cps2_digiav/pcb/`
- RP2350 datasheet: GPIO banks and power domains
- JLCPCB capabilities: https://jlcpcb.com/capabilities/pcb-capabilities
- Saturn PCB Toolkit: For impedance calculations

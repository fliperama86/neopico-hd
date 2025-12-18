# Neo Geo MVS MV1C Digital Video Specification

Technical specification for the digital video signals on the Neo Geo MVS MV1C arcade board.

## Signal Overview

The MVS MV1C generates 15-bit RGB video digitally, clocked at 6 MHz with composite sync (CSYNC). Signals are digital and can be tapped before the R2R DAC for direct digital capture.

### Video Signals

| Signal | Width | Description |
|--------|-------|-------------|
| R[4:0] | 5 bits | Red channel (R4 is MSB) |
| G[4:0] | 5 bits | Green channel (G4 is MSB) |
| B[4:0] | 5 bits | Blue channel (B4 is MSB) |
| PCLK | 1 bit | Pixel clock, 6 MHz |
| CSYNC | 1 bit | Composite sync, active low |
| DARK | 1 bit | Brightness control (active = dark pixel) |
| SHADOW | 1 bit | Dimming control (active = RGB values halved) |

**Color depth**: 15-bit (32,768 colors)
**Data valid**: RGB sampled on PCLK rising edge

### Additional Signals

| Signal | Frequency | Description |
|--------|-----------|-------------|
| C1 | 12 MHz | Master clock (24 MHz / 2) |
| C2/PCLK | 6 MHz | Pixel clock (24 MHz / 4) |

## Timing Parameters

### Frame Timing

| Parameter | Value |
|-----------|-------|
| Frame rate | 59.19 Hz (MVS) / 59.60 Hz (AES) |
| Total pixels per frame | 101,376 (384 x 264) |
| Active resolution | 320 x 224 |

### Horizontal Timing (per line)

| Parameter | Pixels | Time |
|-----------|--------|------|
| Total | 384 | 64 µs |
| Active | 320 | 53.3 µs |
| Sync (HSYNC) | 29 | 4.8 µs |
| Back porch | 28 | 4.7 µs |
| Front porch | 7 | 1.2 µs |

**HSYNC frequency**: ~15.625 kHz

### Vertical Timing (per frame)

| Parameter | Lines | Time |
|-----------|-------|------|
| Total | 264 | 16.9 ms |
| Active | 224 | 14.3 ms |
| Sync (VSYNC) | 3 | 0.19 ms |
| Back porch | 21 | 1.34 ms |
| Front porch | 16 | 1.02 ms |

### Timing Diagram

```
Horizontal Line:
├── HSYNC (29 px) ──┼── Back Porch (28 px) ──┼── Active (320 px) ──┼── Front Porch (7 px) ──┤
                    │                         │                      │
CSYNC: ___LOW_______│________HIGH____________│______HIGH___________│______HIGH_____________│

Vertical Frame:
├── VSYNC (3 lines) ──┼── Back Porch (21 lines) ──┼── Active (224 lines) ──┼── Front Porch (16 lines) ──┤
                      │                            │                         │
Active starts:        │                            Line 24                   │
```

## CSYNC Structure

CSYNC combines horizontal and vertical sync with equalization pulses:

| Period | CSYNC Behavior |
|--------|----------------|
| Normal line | Low for 29 pixels (HSYNC), high for 355 pixels |
| VSYNC | Rapid half-line pulses (equalization) |

### Equalization Pulses

During VSYNC, CSYNC contains 18 equalization pulses (~177 pixels each, half a normal line):

- **Detection**: Count pixels between CSYNC edges
  - Full line: ~355 pixels
  - Equalization: ~177 pixels
- **Threshold**: Use >288 pixels (3/4 line) to distinguish HSYNC from equalization
- **Position**: Lines 248-260 in frame

## SHADOW/DARK Processing

| Signal | Action |
|--------|--------|
| SHADOW active | Divide RGB values by 2 (right shift by 1) |
| DARK active | Indicates dark/shadow pixel |

Example SHADOW processing:
```
RGB[4:0] = {0, R[4:1]} when SHADOW=1
```

## Tap Points (MV1C Board)

### RGB (from R2R DAC resistors, LS273 side)

| Bit | Red | Green | Blue |
|-----|-----|-------|------|
| 0 (LSB) | R53 | R69 | R54 |
| 1 | R55 | R70 | R56 |
| 2 | R57 | R72 | R58 |
| 3 | R59 | R74 | R60 |
| 4 (MSB) | R61 | R75 | R62 |

### Clock and Sync

| Signal | Location |
|--------|----------|
| C1 (12 MHz) | R47 |
| C2/PCLK (6 MHz) | PC23 pin 11 |
| CSYNC | R51 (bottom side) |

### Control

| Signal | Location |
|--------|----------|
| DARK | PC22 pin 1 |
| SHADOW | PC22 pin 9 |

### Power

| Signal | Location |
|--------|----------|
| 5V | TP2 |
| GND | C6 |

## Capture Considerations

1. **Sample on PCLK rising edge** during active region only
2. **Filter CSYNC** to separate HSYNC from equalization pulses
3. **Detect VSYNC** by counting equalization pulse sequences (18 pulses)
4. **Handle SHADOW** by shifting RGB right if shadow effects are desired
5. **Ground properly** to avoid noise from digital switching

## References

- cps2_digiav project (board/neogeo/ documentation)
- Neo Geo MVS hardware schematics
- NeoGeo Dev Wiki

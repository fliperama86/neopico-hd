# How SHADOW and DARK Work on Neo Geo

## Firmware Status

NeoPico-HD captures RGB555 plus the per-pixel SHADOW and DARK signals.

- `NEOPICO_ENABLE_DARK_SHADOW=OFF` is still the default. It converts all 32,768 RGB555 colors exactly to RGB565 and ignores the two effect flags.
- `NEOPICO_ENABLE_DARK_SHADOW=ON` enables four independent states: normal, SHADOW, DARK, and DARK+SHADOW.
- `NEOPICO_MVS_EFFECT_MODEL=MISTER` selects the default digital-reference model.
- `NEOPICO_MVS_EFFECT_MODEL=MAME` selects MAME's analog resistor-network model.

The effect model is still selected at compile time. This split-LUT path produced bottom-screen pixel jitter in hardware testing and remains a separate default-off experiment.

`NEOPICO_MVS_COLOR_MODEL_MENU=ON` is a different feature. Its OSD values, `Digital` and `Analog`, select normal-pixel conversion only. It keeps both normal-color LUTs and previews by switching one active pointer at input VSYNC. SELECT restores the committed model; START/MENU persists the preview without a firmware reboot. Both choices ignore the captured SHADOW/DARK signals, and the build system rejects enabling the menu and effect experiment together.

## Captured Pixel Layout

The PIO capture word preserves all source bits:

```text
[18:DARK][17:SHADOW][16:12:R][11:7:G][6:2:B][1:PCLK][0:CSYNC]
```

After the configured wiring correction, each color component is a value from 0 through 31. The two effect signals do not replace RGB bits. They select how the three RGB555 components are converted.

## MiSTer Digital Model

The MiSTer profile is pinned to [Neo Geo commit `2325e6c`](https://github.com/MiSTer-devel/NeoGeo_MiSTer/blob/2325e6c4303dc9a3fd554b18d9833e992ccd444f/neogeo.sv#L2205-L2222). For each channel:

```text
value6 = (value5 << 1) | (value5 >> 4)
if DARK:   value6 = max(value6 - 1, 0)
value8 = (value6 << 2) | ((value6 >> 3) & 3)
if SHADOW: value8 = value8 >> 1
```

DARK and SHADOW are independent. DARK changes `value6` first, then SHADOW halves the expanded result. White endpoints before RGB565 packing are:

| State | RGB888 component |
| :--- | ---: |
| Normal | 255 |
| DARK | 251 |
| SHADOW | 127 |
| DARK+SHADOW | 125 |

This profile is recommended when the goal is deterministic preservation of the digital RGB555 image and effect bits.

## MAME Analog Model

The MAME profile is pinned to [MAME commit `e47c0f3`](https://github.com/mamedev/mame/blob/e47c0f33c5be3ee286ff65bed13458c2920340d2/src/mame/neogeo/neogeo_v.cpp#L23-L64). It models the Neo Geo DAC as five channel resistors of 3900, 2200, 1000, 470, and 220 ohms. It applies an 8200-ohm DARK pulldown, a 150-ohm SHADOW pulldown, or their modeled parallel combination.

Firmware stores the exact 128 precomputed channel results, so no floating-point math runs on the RP2350. White endpoints before RGB565 packing are:

| State | RGB888 component |
| :--- | ---: |
| Normal | 255 |
| DARK | 251 |
| SHADOW | 142 |
| DARK+SHADOW | 141 |

MAME intentionally applies its non-ideal resistor curve to normal colors too. It is useful as an analog-emulation reference, but it is not a measurement of the specific MV1C board, DAC loading, video amplifier, display chain, or calibration target.

## Compact Four-State LUT

A flat four-state RGB565 LUT would require 262,144 bytes. NeoPico-HD splits each pixel into an R/G index and a B index:

```text
pixel = rg_table[state][raw_rg] | b_table[state][raw_b]
```

The production table contains:

- 4 x 1,024 R/G entries
- 4 x 32 B entries
- 4,224 total 16-bit entries
- 8,448 bytes total

The Cortex-M33 pixel loop performs two halfword table reads plus OR. It has no data-dependent effect branch. MiSTer and MAME use identical runtime lookup code; only boot-time table generation differs.

## RGB565 Transport Limit

Each selected model produces an 8-bit component, then firmware packs it as:

```text
R5 = R8 >> 3
G6 = G8 >> 2
B5 = B8 >> 3
```

The current HSTX path expands those fields by zero-filling their low bits. This introduces at most 7, 3, and 7 code values of transport error for red, green, and blue. It also means two distinct modeled states can occasionally produce the same RGB565 code even though their 8-bit model values differ.

The exhaustive host test checks every one of the 32,768 RGB555 values in all four effect states for both profiles. It proves formula and table fidelity. It does not replace measurement or visual validation on the target MV1C and HDMI sink.

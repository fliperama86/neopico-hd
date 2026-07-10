/**
 * NeoPico-HD SNES / SuperPico Pin Configuration
 *
 * Target-specific pins for SHVC/SNES digital video and audio capture.
 */

#ifndef SNES_PINS_H
#define SNES_PINS_H

// =============================================================================
// SNES Video Input Pins - GP27-44 CONTIGUOUS LAYOUT
// =============================================================================
// Captures RGB555 + VBLANK + HBLANK from the SNES PPU2.
//
// 18-pin capture window: GP27 (LSB) through GP44 (MSB).
//
// Pin mapping (LSB to MSB in captured word):
//   Bit 0:      GP27 (VBLANK) - PPU2 Pin 26
//   Bit 1:      GP28 (PCLK)   - PPU2 Pin 27
//   Bits 2-6:   GP29-33 (Blue B4-B0, contiguous)
//   Bits 7-11:  GP34-38 (Green G4-G0, contiguous)
//   Bits 12-16: GP39-43 (Red R4-R0, contiguous)
//   Bit 17:     GP44 (HBLANK) - PPU2 Pin 25
//
// With PIO GPIOBASE=16, pin index N = GP(N+16). So IN_BASE=11 -> GP27.

#define PIN_SNES_VBLANK 27
#define PIN_SNES_PCLK 28
#define PIN_SNES_BASE 27

#define PIN_SNES_B4 29
#define PIN_SNES_B3 30
#define PIN_SNES_B2 31
#define PIN_SNES_B1 32
#define PIN_SNES_B0 33

#define PIN_SNES_G4 34
#define PIN_SNES_G3 35
#define PIN_SNES_G2 36
#define PIN_SNES_G1 37
#define PIN_SNES_G0 38

#define PIN_SNES_R4 39
#define PIN_SNES_R3 40
#define PIN_SNES_R2 41
#define PIN_SNES_R1 42
#define PIN_SNES_R0 43

#define PIN_SNES_HBLANK 44

#define SNES_CAPTURE_PIN_LAST PIN_SNES_HBLANK
#define SNES_CAPTURE_BITS 18

// =============================================================================
// SNES S-DSP Audio Input Pins (DAT=GP22, WS=GP23, BCK=GP24)
// =============================================================================
// These match the existing NeoPico audio capture GPIOs, but the source is the
// SNES S-DSP digital interface:
//   DSP44 SDATA -> GPIO 22
//   DSP43 LRCK  -> GPIO 23
//   DSP42 BCLK  -> GPIO 24

#define PIN_I2S_DAT 22
#define PIN_I2S_WS 23
#define PIN_I2S_BCK 24

// Existing OSD buttons on the carrier.
#define PIN_OSD_BTN_MENU 25
#define PIN_OSD_BTN_BACK 26

#endif // SNES_PINS_H

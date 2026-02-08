/**
 * NeoPico-HD MVS Pin Configuration
 *
 * This file contains all MVS capture, I2S audio, and OSD control pins.
 * These are specific to the NeoPico-HD firmware and hardware layout.
 */

#ifndef MVS_PINS_H
#define MVS_PINS_H

// =============================================================================
// MVS Video Input Pins - GP27-44 CONTIGUOUS LAYOUT
// =============================================================================
// Captures RGB555 + SHADOW from Neo Geo MVS (this layout has no DARK pin).
//
// 18-pin capture window: GP27 (LSB) through GP44 (MSB).
// PIO captures with 'in pins, 18' from IN_BASE = GP27.
//
// Pin mapping (LSB to MSB in captured word):
//   Bit 0:      GP27 (CSYNC)
//   Bit 1:      GP28 (PCLK)
//   Bits 2-6:   GP29-33 (Blue B4-B0, contiguous)
//   Bits 7-11:  GP34-38 (Green G4-G0, contiguous)
//   Bits 12-16: GP39-43 (Red R4-R0, contiguous)
//   Bit 17:     GP44 (SHADOW)
//
// With PIO GPIOBASE=16, pin index N = GP(N+16). So IN_BASE=11 → GP27.

#define PIN_MVS_CSYNC 27 // Composite sync (position 0 in capture word)
#define PIN_MVS_PCLK 28  // Pixel clock (6 MHz, position 1)
#define PIN_MVS_BASE 27  // Base pin - capture GP27-44 (18 pins), CSYNC first

// Blue channel (B4-B0) - CONTIGUOUS at bits 2-6
#define PIN_MVS_B4 29
#define PIN_MVS_B3 30
#define PIN_MVS_B2 31
#define PIN_MVS_B1 32
#define PIN_MVS_B0 33

// Green channel (G4-G0) - CONTIGUOUS at bits 7-11
#define PIN_MVS_G4 34
#define PIN_MVS_G3 35
#define PIN_MVS_G2 36
#define PIN_MVS_G1 37
#define PIN_MVS_G0 38

// Red channel (R4-R0) - CONTIGUOUS at bits 12-16
#define PIN_MVS_R4 39
#define PIN_MVS_R3 40
#define PIN_MVS_R2 41
#define PIN_MVS_R1 42
#define PIN_MVS_R0 43

// Special effects
#define PIN_MVS_SHADOW 44 // Bit 17 (no DARK pin in this hardware layout)

// =============================================================================
// I2S Audio Input Pins (DAT=GP22, WS=GP23, BCK=GP24)
// =============================================================================
#define PIN_I2S_DAT 22 // I2S data
#define PIN_I2S_WS 23  // I2S word select
#define PIN_I2S_BCK 24 // I2S bit clock

// =============================================================================
// OSD Control (Direct Buttons)
// =============================================================================
// Custom PCB (WeAct RP2350B): SW_MENU=GP25, SW_BACK=GP26. Change if different.
#define PIN_OSD_BTN_MENU 3 // Menu / Select Button
#define PIN_OSD_BTN_BACK 4 // Back / Cycle Button

#endif // MVS_PINS_H

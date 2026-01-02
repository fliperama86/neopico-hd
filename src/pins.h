/**
 * NeoPico-HD Pin Configuration
 *
 * Single source of truth for all GPIO pin assignments
 * Target: RP2350B (WeAct Studio RP2350B Core)
 */

#ifndef PINS_H
#define PINS_H

// =============================================================================
// MVS Video Input Pins - GP25-42 CONTIGUOUS LAYOUT
// =============================================================================
// Captures RGB555 + DARK + SHADOW from Neo Geo MVS
//
// GP25-42: 18-pin capture window (CONTIGUOUS bit fields for performance)
// GP43:    CSYNC (outside capture window, read separately)
//
// OPTIMIZED Pin mapping (connect with dupont wires):
//   PCLK:   GP25 (position 0 - CRITICAL!)
//   Red:    R0=GP26, R1=GP27, R2=GP28, R3=GP29, R4=GP30 (5 bits, contiguous)
//   Green:  G0=GP31, G1=GP32, G2=GP33, G3=GP34, G4=GP35 (5 bits, contiguous)
//   Blue:   B0=GP36, B1=GP37, B2=GP38, B3=GP39, B4=GP40 (5 bits, contiguous)
//   DARK:   GP41 (1 bit)
//   SHADOW: GP42 (1 bit)
//   CSYNC:  GP43 (outside 18-pin window, separate PIO SM)
//
// PIO captures GP25-42 (18 consecutive pins) using 'in pins, 18'
// Fast extraction: Simple shift+mask for each contiguous field (no LUT!)

#define PIN_MVS_PCLK  25   // Pixel clock (6 MHz, position 0)
#define PIN_MVS_CSYNC 43   // Composite sync (outside capture window)
#define PIN_MVS_BASE  25   // Base pin - capture GP25-42 (18 pins, PCLK first)

// Red channel (R0-R4) - CONTIGUOUS at bits 1-5
#define PIN_MVS_R0    26
#define PIN_MVS_R1    27
#define PIN_MVS_R2    28
#define PIN_MVS_R3    29
#define PIN_MVS_R4    30

// Green channel (G0-G4) - CONTIGUOUS at bits 6-10
#define PIN_MVS_G0    31
#define PIN_MVS_G1    32
#define PIN_MVS_G2    33
#define PIN_MVS_G3    34
#define PIN_MVS_G4    35

// Blue channel (B0-B4) - CONTIGUOUS at bits 11-15
#define PIN_MVS_B0    36
#define PIN_MVS_B1    37
#define PIN_MVS_B2    38
#define PIN_MVS_B3    39
#define PIN_MVS_B4    40

// Special effects (bits 16-17)
#define PIN_MVS_DARK     41   // Bit 16
#define PIN_MVS_SHADOW   42   // Bit 17

// =============================================================================
// DVI/HDMI Output Pins (GPIO 12-19)
// =============================================================================
// Differential pairs for TMDS video output
//
// GPIO 12-13: DVI Clock (CLK-/CLK+)
// GPIO 14-15: DVI D0 (D0-/D0+)
// GPIO 16-17: DVI D1 (D1-/D1+)
// GPIO 18-19: DVI D2 (D2-/D2+)
//
// Note: GP(n)=negative, GP(n+1)=positive

#define PIN_DVI_CLK   12   // Clock pair base (12=CLK-, 13=CLK+)
#define PIN_DVI_D0    14   // Data 0 pair base (14=D0-, 15=D0+)
#define PIN_DVI_D1    16   // Data 1 pair base (16=D1-, 17=D1+)
#define PIN_DVI_D2    18   // Data 2 pair base (18=D2-, 19=D2+)

// =============================================================================
// I2S Audio Input Pins (GPIO 0-2)
// =============================================================================
// Captures digital audio from MVS
//
// GPIO 0: I2S DAT (Data)
// GPIO 1: I2S WS  (Word Select / LRCK)
// GPIO 2: I2S BCK (Bit Clock)

#define PIN_I2S_DAT   0    // I2S data
#define PIN_I2S_WS    1    // I2S word select
#define PIN_I2S_BCK   2    // I2S bit clock

// =============================================================================
// Pin Usage Summary
// =============================================================================
/*
 * GPIO 12-19: DVI/HDMI output (TMDS differential pairs) - RESERVED
 * GPIO 20-24: Available for future expansion (5 pins)
 * GPIO 25-42: MVS video capture - 18 pins:
 *             - GP25: PCLK (position 0)
 *             - GP26-30: Red (R0-R4, contiguous)
 *             - GP31-35: Green (G0-G4, contiguous)
 *             - GP36-40: Blue (B0-B4, contiguous)
 *             - GP41: DARK
 *             - GP42: SHADOW
 * GPIO 43:    MVS CSYNC (outside capture window)
 * GPIO 44-47: Available for future expansion (4 pins)
 *
 * PIO Usage:
 * - PIO0: DVI serialization (video output) / Future: HSTX
 * - PIO1: MVS capture (video input) with GPIOBASE=16
 * - PIO2: I2S capture (audio input) - future
 */

#endif // PINS_H

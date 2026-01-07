/**
 * NeoPico-HD Pin Configuration
 *
 * Single source of truth for all GPIO pin assignments
 * Target: RP2350B (WeAct Studio RP2350B Core)
 */

#ifndef PINS_H
#define PINS_H

// =============================================================================
// MVS Video Input Pins - GP27-44 CONTIGUOUS LAYOUT
// =============================================================================
// Captures RGB555 + DARK + SHADOW from Neo Geo MVS
//
// GP27-44: 18-pin capture window (CONTIGUOUS bit fields for performance)
// GP45:    CSYNC (outside capture window, read separately)
//
// OPTIMIZED Pin mapping (Shift +2):
//   PCLK:   GP27 (position 0 - CRITICAL!)
//   Red:    R0=GP28, R1=GP29, R2=GP30, R3=GP31, R4=GP32 (5 bits, contiguous)
//   Green:  G0=GP33, G1=GP34, G2=GP35, G3=GP36, G4=GP37 (5 bits, contiguous)
//   Blue:   B0=GP38, B1=GP39, B2=GP40, B3=GP41, B4=GP42 (5 bits, contiguous)
//   DARK:   GP43 (1 bit)
//   SHADOW: GP44 (1 bit)
//   CSYNC:  GP45 (outside 18-pin window, separate PIO SM)
//
// PIO captures GP27-44 (18 consecutive pins) using 'in pins, 18'

#define PIN_MVS_PCLK 27  // Pixel clock (6 MHz, position 0)
#define PIN_MVS_CSYNC 45 // Composite sync (outside capture window)
#define PIN_MVS_BASE 27  // Base pin - capture GP27-44 (18 pins, PCLK first)

// Red channel (R0-R4) - CONTIGUOUS at bits 1-5
#define PIN_MVS_R0 28
#define PIN_MVS_R1 29
#define PIN_MVS_R2 30
#define PIN_MVS_R3 31
#define PIN_MVS_R4 32

// Green channel (G0-G4) - CONTIGUOUS at bits 6-10
#define PIN_MVS_G0 33
#define PIN_MVS_G1 34
#define PIN_MVS_G2 35
#define PIN_MVS_G3 36
#define PIN_MVS_G4 37

// Blue channel (B0-B4) - CONTIGUOUS at bits 11-15
#define PIN_MVS_B0 38
#define PIN_MVS_B1 39
#define PIN_MVS_B2 40
#define PIN_MVS_B3 41
#define PIN_MVS_B4 42

// Special effects (bits 16-17)
#define PIN_MVS_DARK 43   // Bit 16
#define PIN_MVS_SHADOW 44 // Bit 17

// =============================================================================
// DVI/HDMI Output Pins (GPIO 12-19)
// =============================================================================
#define PIN_DVI_CLK 12 // Clock pair base (12=CLK-, 13=CLK+)
#define PIN_DVI_D0 14  // Data 0 pair base (14=D0-, 15=D0+)
#define PIN_DVI_D1 16  // Data 1 pair base (16=D1-, 17=D1+)
#define PIN_DVI_D2 18  // Data 2 pair base (18=D2-, 19=D2+)

// =============================================================================
// I2S Audio Input Pins (GPIO 0-2)
// =============================================================================
#define PIN_I2S_DAT 0 // I2S data
#define PIN_I2S_WS 1  // I2S word select
#define PIN_I2S_BCK 2 // I2S bit clock

// =============================================================================
// OSD Control (Direct Buttons & Shift Register)
// =============================================================================
// Option A: Direct Buttons (For physical buttons on the case)
#define PIN_OSD_BTN_MENU 3 // Menu / Select Button
#define PIN_OSD_BTN_BACK 4 // Back / Cycle Button

// Option B: Shift Register (For FUTURE tapping Neo Geo Joystick)
// #define PIN_OSD_LD 5  // Parallel Load (PL)
// #define PIN_OSD_CLK 6 // Clock (CP)
// #define PIN_OSD_DAT 7 // Serial Data (Q7)

#endif // PINS_H

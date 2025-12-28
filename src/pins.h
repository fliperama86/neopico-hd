/**
 * NeoPico-HD Pin Configuration
 *
 * Single source of truth for all GPIO pin assignments
 * Target: RP2350B (WeAct Studio RP2350B Core)
 */

#ifndef PINS_H
#define PINS_H

// =============================================================================
// MVS Video Input Pins (GPIO 0-15, 22)
// =============================================================================
// Captures RGB555 video from Neo Geo MVS
//
// GPIO 0:     PCLK (Pixel Clock, 6 MHz)
// GPIO 1-5:   G4-G0 (Green, reversed bit order)
// GPIO 6-10:  B0-B4 (Blue)
// GPIO 11-15: R0-R4 (Red)
// GPIO 22:    CSYNC (Composite Sync)

#define PIN_MVS_BASE  0    // Base pin for 16-bit capture (PCLK + RGB data)
#define PIN_MVS_PCLK  0    // Pixel clock
#define PIN_MVS_CSYNC 22   // Composite sync

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
// I2S Audio Input Pins (GPIO 36-38)
// =============================================================================
// Captures digital audio from MVS
//
// GPIO 36: I2S DAT (Data)
// GPIO 37: I2S WS  (Word Select / LRCK)
// GPIO 38: I2S BCK (Bit Clock)

#define PIN_I2S_DAT   36   // I2S data
#define PIN_I2S_WS    37   // I2S word select
#define PIN_I2S_BCK   38   // I2S bit clock

// =============================================================================
// Pin Usage Summary
// =============================================================================
/*
 * GPIO 0-15:  MVS video capture (PCLK + RGB555)
 * GPIO 12-19: DVI/HDMI output (overlaps with MVS, but different function)
 * GPIO 22:    MVS sync input
 * GPIO 36-38: I2S audio input
 *
 * PIO Usage:
 * - PIO0: DVI serialization (video output)
 * - PIO1: MVS capture (video input)
 * - PIO2: I2S capture (audio input)
 */

#endif // PINS_H

#ifndef NEOPICO_CONFIG_H
#define NEOPICO_CONFIG_H

#include "dvi_serialiser.h"

// =============================================================================
// MVS Pin Configuration
// =============================================================================
// GPIO 0:     PCLK (C2)
// GPIO 1-5:   G4-G0 (Green, reversed bit order)
// GPIO 6-10:  B0-B4 (Blue)
// GPIO 11-15: R0-R4 (Red)
// GPIO 22:    CSYNC

#define PIN_BASE  0    // Base pin for 16-bit capture (PCLK + RGB data)
#define PIN_PCLK  0    // Pixel clock
#define PIN_CSYNC 22   // Composite sync

// Lookup table to reverse 5-bit green value (G4G3G2G1G0 -> G0G1G2G3G4)
static const uint8_t green_reverse_lut[32] = {
    0x00, 0x10, 0x08, 0x18, 0x04, 0x14, 0x0C, 0x1C,
    0x02, 0x12, 0x0A, 0x1A, 0x06, 0x16, 0x0E, 0x1E,
    0x01, 0x11, 0x09, 0x19, 0x05, 0x15, 0x0D, 0x1D,
    0x03, 0x13, 0x0B, 0x1B, 0x07, 0x17, 0x0F, 0x1F
};

// =============================================================================
// DVI Pin Configuration
// =============================================================================
// GPIO 25-26: DVI Clock (CLKN/CLKP)
// GPIO 27-28: DVI D0 (D0N/D0P)
// GPIO 29-30: DVI D1 (D1N/D1P)
// GPIO 31-32: DVI D2 (D2N/D2P)

static const struct dvi_serialiser_cfg neopico_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {27, 29, 31},   // D0=GP27-28, D1=GP29-30, D2=GP31-32
    .pins_clk = 25,              // Clock=GP25-26
    .invert_diffpairs = true
};

#endif // NEOPICO_CONFIG_H

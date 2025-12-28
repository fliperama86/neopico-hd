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
// GPIO 12-13: DVI Clock (CLK-/CLK+)
// GPIO 14-15: DVI D0 (D0-/D0+)
// GPIO 16-17: DVI D1 (D1-/D1+)
// GPIO 18-19: DVI D2 (D2-/D2+)
//
// Requirements:
//   - Add DVI_USE_PIO_CLOCK=1 compile flag

static const struct dvi_serialiser_cfg neopico_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {14, 16, 18},   // D0=GP14-15, D1=GP16-17, D2=GP18-19
    .pins_clk = 12,              // Clock=GP12-13
    .invert_diffpairs = true     // GP(n)=-, GP(n+1)=+
};

// GPIO setup function - no longer needed for GPIO 12-19 (was required for GPIO 32+ access)
static inline void neopico_dvi_gpio_setup(void) {
    // No setup required for GPIO 12-19 (default PIO GPIO base works)
}

#endif // NEOPICO_CONFIG_H

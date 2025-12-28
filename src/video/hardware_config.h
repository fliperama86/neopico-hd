#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "dvi_serialiser.h"
#include "../pins.h"

// =============================================================================
// MVS Pin Aliases (for compatibility)
// =============================================================================
#define PIN_BASE  PIN_MVS_BASE
#define PIN_PCLK  PIN_MVS_PCLK
#define PIN_CSYNC PIN_MVS_CSYNC

// Lookup table to reverse 5-bit green value (G4G3G2G1G0 -> G0G1G2G3G4)
static const uint8_t green_reverse_lut[32] = {
    0x00, 0x10, 0x08, 0x18, 0x04, 0x14, 0x0C, 0x1C,
    0x02, 0x12, 0x0A, 0x1A, 0x06, 0x16, 0x0E, 0x1E,
    0x01, 0x11, 0x09, 0x19, 0x05, 0x15, 0x0D, 0x1D,
    0x03, 0x13, 0x0B, 0x1B, 0x07, 0x17, 0x0F, 0x1F
};

// =============================================================================
// DVI Configuration
// =============================================================================

static const struct dvi_serialiser_cfg neopico_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {PIN_DVI_D0, PIN_DVI_D1, PIN_DVI_D2},
    .pins_clk = PIN_DVI_CLK,
    .invert_diffpairs = true     // GP(n)=-, GP(n+1)=+
};

// GPIO setup function - no longer needed for GPIO 12-19 (was required for GPIO 32+ access)
static inline void neopico_dvi_gpio_setup(void) {
    // No setup required for GPIO 12-19 (default PIO GPIO base works)
}

#endif // HARDWARE_CONFIG_H

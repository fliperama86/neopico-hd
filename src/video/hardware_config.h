#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#ifndef HSTX_LAB_BUILD
#include "dvi_serialiser.h"
#endif
#include "../pins.h"

// =============================================================================
// MVS Pin Aliases (for compatibility)
// =============================================================================
#define PIN_BASE  PIN_MVS_BASE
#define PIN_PCLK  PIN_MVS_PCLK
#define PIN_CSYNC PIN_MVS_CSYNC

// =============================================================================
// GP25-42 Contiguous Bit Field Extraction (FAST!)
// =============================================================================
// PIO reads GP25-42 (18 pins), with PCLK at position 0 and CONTIGUOUS RGB fields
//
// 18-bit capture layout (RGB order):
//   Bit 0:      GP25 (PCLK - ignored)
//   Bits 1-5:   GP26-30 (Red R0-R4, contiguous)
//   Bits 6-10:  GP31-35 (Green G0-G4, contiguous)
//   Bits 11-15: GP36-40 (Blue B0-B4, contiguous)
//   Bit 16:     GP41 (DARK - not used yet)
//   Bit 17:     GP42 (SHADOW - not used yet)
//
// This matches the working Bank 0 architecture: contiguous fields = fast extraction!
// RGB555 format: RRRRR GGGGG BBBBB (bits 14-10: R, 9-5: G, 4-0: B)

static inline uint16_t extract_rgb555_contiguous(uint32_t gpio_data) {
    // BABY STEP #2: Revert to original positions - SDK should fix IN_BASE
    // If IN_BASE is correctly set to GP25, these positions will be right:
    uint8_t r = (gpio_data >> 1) & 0x1F;   // Bits 1-5: Red (GP26-30)
    uint8_t g = (gpio_data >> 6) & 0x1F;   // Bits 6-10: Green (GP31-35)
    uint8_t b = (gpio_data >> 11) & 0x1F;  // Bits 11-15: Blue (GP36-40)

    // Combine into RGB555: RRRRR GGGGG BBBBB
    return (r << 10) | (g << 5) | b;
}

// =============================================================================
// DVI Configuration (PicoDVI only, not used by HSTX Lab)
// =============================================================================

#ifndef HSTX_LAB_BUILD
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
#endif // HSTX_LAB_BUILD

#endif // HARDWARE_CONFIG_H

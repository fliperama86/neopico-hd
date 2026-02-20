#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include <stdint.h>

#include "mvs_pins.h"

// =============================================================================
// GP27-44 Contiguous Bit Field Extraction
// =============================================================================
// PIO reads GP27-44 (18 pins) with IN_BASE = GP27. LSB first in capture word.
//
// 19-bit capture layout:
//   Bit 0:      GP27 (CSYNC)
//   Bit 1:      GP28 (PCLK)
//   Bits 2-6:   GP29-33 (Blue B4-B0, contiguous)
//   Bits 7-11:  GP34-38 (Green G4-G0, contiguous)
//   Bits 12-16: GP39-43 (Red R4-R0, contiguous)
//   Bit 17:     GP44 (SHADOW)
//   Bit 18:     GP45 (DARK)
//
// RGB555 format: RRRRR GGGGG BBBBB (bits 12-16: R, 7-11: G, 2-6: B)

static inline uint16_t extract_rgb555_contiguous(uint32_t gpio_data)
{
    uint8_t r = (gpio_data >> 12) & 0x1F; // Bits 12-16: Red (GP39-43)
    uint8_t g = (gpio_data >> 7) & 0x1F;  // Bits 7-11: Green (GP34-38)
    uint8_t b = (gpio_data >> 2) & 0x1F;  // Bits 2-6: Blue (GP29-33)
    return (r << 10) | (g << 5) | b;
}

#endif // HARDWARE_CONFIG_H

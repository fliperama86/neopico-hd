#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "mvs_pins.h"
#include <stdint.h>

// =============================================================================
// GP27-44 Contiguous Bit Field Extraction (FAST!)
// =============================================================================
// PIO reads GP27-44 (18 pins), with PCLK at position 0 and CONTIGUOUS RGB
// fields
//
// 18-bit capture layout (RGB order):
//   Bit 0:      GP27 (PCLK - ignored)
//   Bits 1-5:   GP28-32 (Red R0-R4, contiguous)
//   Bits 6-10:  GP33-37 (Green G0-G4, contiguous)
//   Bits 11-15: GP38-42 (Blue B0-B4, contiguous)
//   Bit 16:     GP43 (DARK)
//   Bit 17:     GP44 (SHADOW)
//
// This matches the working Bank 1 architecture: contiguous fields = fast
// extraction! RGB555 format: RRRRR GGGGG BBBBB (bits 14-10: R, 9-5: G, 4-0: B)

static inline uint16_t extract_rgb555_contiguous(uint32_t gpio_data) {
  // If IN_BASE is correctly set to GP27, these positions will be right:
  uint8_t r = (gpio_data >> 1) & 0x1F;  // Bits 1-5: Red (GP28-32)
  uint8_t g = (gpio_data >> 6) & 0x1F;  // Bits 6-10: Green (GP33-37)
  uint8_t b = (gpio_data >> 11) & 0x1F; // Bits 11-15: Blue (GP38-42)

  // Combine into RGB555: RRRRR GGGGG BBBBB
  return (r << 10) | (g << 5) | b;
}

#endif // HARDWARE_CONFIG_H

/**
 * Audio Pipeline - Common Types
 *
 * Shared type definitions for all audio modules.
 * Note: Uses "ap_" prefix to avoid conflicts with PicoDVI's audio types.
 */

#ifndef AUDIO_COMMON_H
#define AUDIO_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#include "pico_hdmi/hstx_packet.h"

// Sample rate conversion modes
typedef enum {
  SRC_MODE_NONE = 0, // Passthrough (no conversion, plays fast)
  SRC_MODE_DROP,     // Bresenham sample dropping
  SRC_MODE_LINEAR,   // Linear interpolation
  SRC_MODE_COUNT     // Number of modes (for cycling)
} src_mode_t;

// Get human-readable name for SRC mode
static inline const char *src_mode_name(src_mode_t mode) {
  switch (mode) {
  case SRC_MODE_NONE:
    return "NONE";
  case SRC_MODE_DROP:
    return "DROP";
  case SRC_MODE_LINEAR:
    return "LINEAR";
  default:
    return "?";
  }
}

#endif // AUDIO_COMMON_H

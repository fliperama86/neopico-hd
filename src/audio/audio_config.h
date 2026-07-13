/**
 * Audio Subsystem Configuration
 * Shared settings for audio capture, processing, and output
 */

#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include "capture_profile.h"

// =============================================================================
// Audio Buffer Configuration
// =============================================================================

// HSTX audio buffer size (samples)
// Larger buffer handles irregular fill timing from video frame processing
#define AUDIO_BUFFER_SIZE 1024

// Sample rates
#if NEOPICO_AUDIO_PCM1802
// PCM1802 ADC with 12.288 MHz crystal: 12288000 / 256 = 48000 Hz exactly
#define AUDIO_INPUT_RATE 48000
// The PCM1802 clock is independent of the MVS video-derived audio clock.
// Keep the DI queue feedback servo close to its nominal 48 kHz rate.
#define AUDIO_INPUT_RATE_MIN 47000
#define AUDIO_INPUT_RATE_MAX 49000
#else
#define AUDIO_INPUT_RATE CAPTURE_AUDIO_INPUT_RATE
#define AUDIO_INPUT_RATE_MIN CAPTURE_AUDIO_RATE_MIN
#define AUDIO_INPUT_RATE_MAX CAPTURE_AUDIO_RATE_MAX
#endif
#define AUDIO_OUTPUT_RATE 48000 // HSTX audio output rate

// =============================================================================
// Pin Configuration (see pins.h for actual GPIO assignments)
// =============================================================================
// YM2610 digital audio interface (MV1C board)
// - Format: Right-justified 16-bit linear PCM
// - Sample rate: ~55.5kHz
//
// IMPORTANT: Pin order is constrained by PIO `wait pin N` instruction!
// - DAT must be at lowest GPIO (in_base for `in pins`)
// - WS and BCK must be consecutive above DAT (accessible via `wait pin N`)
//
// See mvs_pins.h for actual GPIO numbers:
//   PIN_I2S_DAT (GPIO 22) - Serial data / OPO
//   PIN_I2S_WS  (GPIO 23) - Word select / SH1
//   PIN_I2S_BCK (GPIO 24) - Bit clock / øS

#endif // AUDIO_CONFIG_H

/**
 * Audio Subsystem Configuration
 * Shared settings for audio capture, processing, and output
 */

#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

// =============================================================================
// Audio Buffer Configuration
// =============================================================================

// HSTX audio buffer size (samples)
// Larger buffer handles irregular fill timing from video frame processing
#define AUDIO_BUFFER_SIZE 1024

// Sample rates
#define AUDIO_INPUT_RATE 55555  // MVS I2S input rate (approximately)
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
// See pins.h for actual GPIO numbers:
//   PIN_I2S_DAT (GPIO 0) - Serial data / OPO
//   PIN_I2S_WS  (GPIO 1) - Word select / SH1
//   PIN_I2S_BCK (GPIO 2) - Bit clock / øS

#endif // AUDIO_CONFIG_H

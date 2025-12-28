#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

// =============================================================================
// MVS Audio Capture Pin Configuration
// =============================================================================
// YM2610 digital audio interface (protoboard setup)
// MV1C board outputs 16-bit linear PCM (via NEO-YSA2 -> BU9480F DAC)
// Format: Right-justified, ~55.5kHz sample rate
//
// IMPORTANT: Pin order is constrained by PIO `wait pin N` instruction!
// - DAT must be at lowest GPIO (in_base for `in pins`)
// - WS and BCK must be above DAT (accessible via `wait pin N`)
//
// GP36: DAT (serial data / OPO) - Bank 1 (in_base for sampling)
// GP37: WS  (word select / SH1) - Bank 1 (wait pin 1)
// GP38: BCK (bit clock / øS)    - Bank 1 (wait pin 2)

#define AUDIO_PIN_DAT  36
#define AUDIO_PIN_WS   37
#define AUDIO_PIN_BCK  38

#endif // AUDIO_CONFIG_H

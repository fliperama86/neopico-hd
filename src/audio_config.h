#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

// =============================================================================
// MVS Audio Capture Pin Configuration
// =============================================================================
// YM2610 digital audio interface (protoboard setup)
// MV1C board outputs 16-bit linear PCM (via NEO-YSA2 -> BU9480F DAC)
// Format: Right-justified, ~55.5kHz sample rate
//
// GP21: BCK (bit clock / øS)
// GP23: DAT (serial data / OPO)
// GP24: WS  (word select / SH1)

#define AUDIO_PIN_BCK  21
#define AUDIO_PIN_DAT  23
#define AUDIO_PIN_WS   24

#endif // AUDIO_CONFIG_H

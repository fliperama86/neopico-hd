#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

// =============================================================================
// MVS Audio Capture Pin Configuration
// =============================================================================
// YM2610 digital audio interface (directly from chip, before YM3016 DAC)
//
// GP21: BCK (bit clock / øS)
// GP23: DAT (serial data / OPO)
// GP24: WS  (word select / SH1)

#define AUDIO_PIN_BCK  21
#define AUDIO_PIN_DAT  23
#define AUDIO_PIN_WS   24

#endif // AUDIO_CONFIG_H

#ifndef AUDIO_SOURCE_H
#define AUDIO_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#define NEOPICO_AUDIO_MODE_DIGITAL 0
#define NEOPICO_AUDIO_MODE_PCM1802 1
#define NEOPICO_AUDIO_MODE_SELECTABLE 2

#ifndef NEOPICO_AUDIO_MODE
#define NEOPICO_AUDIO_MODE NEOPICO_AUDIO_MODE_DIGITAL
#endif

typedef enum {
    AUDIO_SOURCE_MV1C_DIGITAL = 0,
    AUDIO_SOURCE_PCM1802_I2S = 1,
} audio_source_t;

static inline bool audio_source_is_valid(uint8_t source)
{
    return source <= (uint8_t)AUDIO_SOURCE_PCM1802_I2S;
}

#endif // AUDIO_SOURCE_H

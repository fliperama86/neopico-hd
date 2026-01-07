#ifndef AUDIO_SUBSYSTEM_H
#define AUDIO_SUBSYSTEM_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize the audio subsystem (pipeline, buffers, etc.)
 */
void audio_subsystem_init(void);

/**
 * Start audio capture and processing.
 */
void audio_subsystem_start(void);

/**
 * Stop audio capture.
 */
void audio_subsystem_stop(void);

#endif // AUDIO_SUBSYSTEM_H

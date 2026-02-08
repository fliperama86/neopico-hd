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

/**
 * Mute/unmute HDMI audio output (push silence when muted).
 * Core 0 sets muted on timeout, unmuted when video is locked (CPS2_DIGAV-style).
 */
void audio_subsystem_set_muted(bool muted);

/**
 * Background task for audio subsystem (call from Core 1 only).
 */
void audio_subsystem_background_task(void);

/**
 * Pre-fill DI queue with silence packets.
 * Call from Core 0 before launching Core 1.
 */
void audio_subsystem_prefill_di_queue(void);

/**
 * Core 0 audio poll — BCK-driven state machine.
 * Validates BCK, warms up, then processes audio continuously.
 */
void audio_subsystem_core0_poll(void);

#endif // AUDIO_SUBSYSTEM_H

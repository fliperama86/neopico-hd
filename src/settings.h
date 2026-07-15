#ifndef NEOPICO_SETTINGS_H
#define NEOPICO_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define NEOPICO_SETTINGS_AUDIO_SOURCE_VALID 0xA5U
#define NEOPICO_SETTINGS_COLOR_MODEL_VALID 0xC7U

// Flash-backed persistent settings. Survives power-off (unlike the watchdog
// scratch used for warm reboots). Stored in the last 4 KB flash sector as a
// magic+version+CRC record and written only on explicit setting changes. Most
// settings write immediately before reboot; the live Colors selector queues a
// Core 0 write after a completed input frame. Keep the payload small; the
// reserved bytes give room to add fields without a format/version bump.
typedef struct {
    uint8_t resolution;         // video_pipeline_reboot_mode_t (0=480p, 1=240p, 2=720p)
    uint8_t audio_source;       // audio_source_t; used only when the marker below is valid
    uint8_t audio_source_valid; // NEOPICO_SETTINGS_AUDIO_SOURCE_VALID after explicit selection
    uint8_t color_model;        // mvs_color_model_t; used only when the marker below is valid
    uint8_t color_model_valid;  // NEOPICO_SETTINGS_COLOR_MODEL_VALID after explicit selection
    uint8_t reserved[27];       // future settings; zero-initialized
} neopico_settings_t;

_Static_assert(sizeof(neopico_settings_t) == 32, "settings payload format must remain 32 bytes");

// Load persisted settings into *out. Returns true if a valid record was found;
// false (and *out filled with defaults) if flash is blank/corrupt/old-version.
bool settings_load(neopico_settings_t *out);

// Persist settings to flash (blocking erase+program, ~tens of ms). Runs from
// RAM. Callers must ensure the other core cannot access flash while XIP is
// suspended. The CRC and page image are prepared before the flash operation.
void settings_save(const neopico_settings_t *s);

#if NEOPICO_MVS_COLOR_MODEL_MENU
// Queue one settings record from Core 1 without touching flash. Core 0 calls
// settings_service_pending_save() after completing a captured frame, allowing
// Core 1 HDMI interrupts to continue while XIP is temporarily unavailable.
bool settings_request_save(const neopico_settings_t *s);
bool settings_service_pending_save(void);
bool settings_save_pending(void);
#endif

// Restore and persist recovery defaults: 480p, MV1C Digital audio, and the
// Digital MVS color model when its selector is compiled.
void settings_factory_reset(void);

#endif // NEOPICO_SETTINGS_H

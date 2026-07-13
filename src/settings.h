#ifndef NEOPICO_SETTINGS_H
#define NEOPICO_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define NEOPICO_SETTINGS_AUDIO_SOURCE_VALID 0xA5U

// Flash-backed persistent settings. Survives power-off (unlike the watchdog
// scratch used for warm reboots). Stored in the last 4 KB flash sector as a
// magic+version+CRC record; written rarely (only on a setting change, at the
// reboot point). Keep the payload small; the reserved bytes give room to add
// fields without a format/version bump.
typedef struct {
    uint8_t resolution;         // video_pipeline_reboot_mode_t (0=480p, 1=240p, 2=720p)
    uint8_t audio_source;       // audio_source_t; used only when the marker below is valid
    uint8_t audio_source_valid; // NEOPICO_SETTINGS_AUDIO_SOURCE_VALID after explicit selection
    uint8_t reserved[29];       // future settings; zero-initialized
} neopico_settings_t;

_Static_assert(sizeof(neopico_settings_t) == 32, "settings payload format must remain 32 bytes");

// Load persisted settings into *out. Returns true if a valid record was found;
// false (and *out filled with defaults) if flash is blank/corrupt/old-version.
bool settings_load(neopico_settings_t *out);

// Persist settings to flash (blocking erase+program, ~tens of ms). Runs from
// RAM; safe to call immediately before a reboot. The CRC is computed before the
// flash op so no flash-resident code runs while XIP is suspended.
void settings_save(const neopico_settings_t *s);

// Restore the recovery defaults and persist them: 480p plus an explicit
// MV1C Digital audio selection.
void settings_factory_reset(void);

#endif // NEOPICO_SETTINGS_H

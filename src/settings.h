#ifndef NEOPICO_SETTINGS_H
#define NEOPICO_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

// Flash-backed persistent settings. Survives power-off (unlike the watchdog
// scratch used for warm reboots). Stored in the last 4 KB flash sector as a
// magic+version+CRC record; written rarely (only on a setting change, at the
// reboot point). Keep the payload small; the reserved bytes give room to add
// fields without a format/version bump.
typedef struct {
    uint8_t resolution;   // video_pipeline_reboot_mode_t (0=480p, 1=240p, 2=720p)
    uint8_t reserved[31]; // future settings; zero-initialized
} neopico_settings_t;

// Load persisted settings into *out. Returns true if a valid record was found;
// false (and *out filled with defaults) if flash is blank/corrupt/old-version.
bool settings_load(neopico_settings_t *out);

// Persist settings to flash (blocking erase+program, ~tens of ms). Runs from
// RAM; safe to call immediately before a reboot. The CRC is computed before the
// flash op so no flash-resident code runs while XIP is suspended.
void settings_save(const neopico_settings_t *s);

#endif // NEOPICO_SETTINGS_H

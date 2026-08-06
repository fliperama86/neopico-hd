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
    uint8_t resolution;         // video_pipeline_reboot_mode_t; existing values 0=480p, 1=240p, 2=1280x720
    uint8_t audio_source;       // audio_source_t; used only when the marker below is valid
    uint8_t audio_source_valid; // NEOPICO_SETTINGS_AUDIO_SOURCE_VALID after explicit selection
    uint8_t color_model;        // mvs_color_model_t; used only when the marker below is valid
    uint8_t color_model_valid;  // NEOPICO_SETTINGS_COLOR_MODEL_VALID after explicit selection
    // 0 = off (default), nonzero = on. Unlike audio_source/color_model, off
    // needs no separate "_valid" marker: it is both the desired default AND
    // the natural zero value, so an old settings image (whose reserved byte
    // here was always zero-initialized) already reads as "off" with no
    // ambiguity -- see settings_load()'s payload_size check, which keeps
    // accepting old images because sizeof(neopico_settings_t) is unchanged.
    uint8_t genlock_enabled;
    // Batched Video-screen Apply safety net (see menu_diag_experiment.c): the
    // PENDING flag and the revert resolution do NOT live here. They live in
    // watchdog scratch (video_pipeline_request_reboot_mode_pending() /
    // video_pipeline_take_pending_confirmation()) because that state is only
    // ever needed across a single warm reboot, and being volatile-by-design
    // means the post-Apply Keep action costs nothing: there is no flash
    // record to clear, so Keep never touches flash (measured on hardware: a
    // blocking Core 1 flash write in that path loses HDMI sync). Only the
    // revert genlock bit is kept here, because genlock has no scratch
    // carrier -- it is applied at boot by reading this persisted settings
    // record, not from scratch like resolution -- so its revert value must
    // survive in flash for Revert/timeout to read it back. Zero is both the
    // natural default AND "revert to OFF", so an old settings image (whose
    // reserved byte here was always zero-initialized) already reads
    // unambiguously with no version bump needed. Known accepted tradeoff:
    // because the pending flag is volatile, power-cycling mid-countdown keeps
    // the new mode instead of reverting; the factory-reset button chord is
    // the recovery path for that case.
    uint8_t pending_revert_genlock; // 0/1 genlock_enabled to revert to
    // Same batched-Apply safety net as pending_revert_genlock above, but for
    // Colors: the Video screen's rule is that changing anything on that
    // screen and then rebooting via Apply must Apply-or-revert as a single
    // unit, so a resolution/refresh revert must also roll back a Colors
    // change made in the same Apply. Colors has no scratch carrier (like
    // genlock, it is applied at boot by reading this persisted record, not
    // from scratch), so its revert value must live in flash too. Declared
    // unconditionally -- like color_model/color_model_valid above -- even
    // though only the NEOPICO_MVS_COLOR_MODEL_MENU build ever reads or
    // writes it, because this struct is a persisted flash format that must
    // be byte-identical across build configurations.
    uint8_t pending_revert_color; // mvs_color_model_t to revert color_model to
    // Scanlines: LIVE setting (menu_diag_experiment.c), applied immediately
    // via video_pipeline_set_scanline_level() and queued to flash through the
    // same deferred settings_request_save()/settings_service_pending_save()
    // path Colors uses -- no batched Apply, no revert carrier, since it is
    // exempt from the Video screen's Apply/Cancel transaction. video_pipeline_
    // scanline_level_t values are 0 (OFF) through 4 (100%); like genlock_
    // enabled above, 0 needs no separate "_valid" marker because it is both
    // the natural zero value and the desired default, so an old settings
    // image (whose reserved byte here was always zero-initialized) already
    // reads as "OFF" with no ambiguity. Declared unconditionally: this struct
    // is a persisted flash format that must be byte-identical across build
    // configurations.
    uint8_t scanline_level;
    uint8_t reserved[23]; // future settings; zero-initialized
} neopico_settings_t;

_Static_assert(sizeof(neopico_settings_t) == 32, "settings payload format must remain 32 bytes");

// Load persisted settings into *out. Returns true if a valid record was found;
// false (and *out filled with defaults) if flash is blank/corrupt/old-version.
bool settings_load(neopico_settings_t *out);

// Persist settings to flash (blocking erase+program, ~tens of ms). Runs from
// RAM. Callers must ensure the other core cannot access flash while XIP is
// suspended. The CRC and page image are prepared before the flash operation.
void settings_save(const neopico_settings_t *s);

// Queue one settings record from Core 1 without touching flash. Core 0 calls
// settings_service_pending_save() after completing a captured frame, allowing
// Core 1 HDMI interrupts to continue while XIP is temporarily unavailable.
// Unconditional: both the Colors selector (NEOPICO_MVS_COLOR_MODEL_MENU) and
// the always-available Scanlines level use this same deferred-save queue.
bool settings_request_save(const neopico_settings_t *s);
bool settings_service_pending_save(void);
bool settings_save_pending(void);

// Restore and persist recovery defaults: 480p, MV1C Digital audio, and the
// Digital MVS color model when its selector is compiled.
void settings_factory_reset(void);

#endif // NEOPICO_SETTINGS_H

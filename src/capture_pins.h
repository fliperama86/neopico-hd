#ifndef CAPTURE_PINS_H
#define CAPTURE_PINS_H

#include "capture_profile.h"

#if NEOPICO_CAPTURE_TARGET == NEOPICO_CAPTURE_TARGET_MVS
#include "mvs_pins.h"
#elif NEOPICO_CAPTURE_TARGET == NEOPICO_CAPTURE_TARGET_SNES
#include "snes_pins.h"
#else
#error "Unsupported NEOPICO_CAPTURE_TARGET"
#endif

// Controller taps used as additional OSD inputs (always enabled; see
// AGENTS.md / OSD_IMPLEMENTATION.md). Board/tuning constants, not build
// variants -- GP0/1/2/3 are unused by either capture target's pin map.
#define NEOPICO_OSD_CONTROLLER_MENU_PIN 0
#define NEOPICO_OSD_CONTROLLER_BACK_PIN 1
#define NEOPICO_OSD_CONTROLLER_UP_PIN 3
#define NEOPICO_OSD_CONTROLLER_DOWN_PIN 2
#define NEOPICO_OSD_CONTROLLER_DEBOUNCE_MS 20

#endif // CAPTURE_PINS_H

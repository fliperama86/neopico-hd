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

#endif // CAPTURE_PINS_H

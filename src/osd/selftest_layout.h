#ifndef SELFTEST_LAYOUT_H
#define SELFTEST_LAYOUT_H

#include <stdint.h>

// Draws the static self-test screen layout and initializes icon states.
void selftest_layout_reset(void);

// Bounded dynamic updates for layout-only mode (call at low rate).
void selftest_layout_update(uint32_t frame_count);

#endif // SELFTEST_LAYOUT_H

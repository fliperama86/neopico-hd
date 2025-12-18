/**
 * Audio Ring Buffer Implementation
 */

#include "audio_buffer.h"
#include <string.h>

void ap_ring_init(ap_ring_t *ring) {
    memset(ring->samples, 0, sizeof(ring->samples));
    ring->write_idx = 0;
    ring->read_idx = 0;
}

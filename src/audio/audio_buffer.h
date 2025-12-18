/**
 * Audio Pipeline - Ring Buffer
 *
 * Lock-free single-producer single-consumer ring buffer for audio samples.
 * Designed for DMA producer (capture) and software consumer (processing).
 */

#ifndef AUDIO_BUFFER_H
#define AUDIO_BUFFER_H

#include "audio_common.h"

// Buffer size must be power of 2
#define AP_RING_SIZE 512
#define AP_RING_MASK (AP_RING_SIZE - 1)

typedef struct {
    ap_sample_t samples[AP_RING_SIZE];
    volatile uint32_t write_idx;  // Written by producer (DMA/interrupt)
    volatile uint32_t read_idx;   // Written by consumer (processing)
} ap_ring_t;

// Initialize ring buffer
void ap_ring_init(ap_ring_t *ring);

// Get number of samples available to read
static inline uint32_t ap_ring_available(ap_ring_t *ring) {
    return (ring->write_idx - ring->read_idx) & AP_RING_MASK;
}

// Get free space for writing
static inline uint32_t ap_ring_free(ap_ring_t *ring) {
    return AP_RING_SIZE - 1 - ap_ring_available(ring);
}

// Read one sample (caller must check available first)
static inline ap_sample_t ap_ring_read(ap_ring_t *ring) {
    ap_sample_t s = ring->samples[ring->read_idx & AP_RING_MASK];
    ring->read_idx++;
    return s;
}

// Write one sample (caller must check free first)
static inline void ap_ring_write(ap_ring_t *ring, ap_sample_t s) {
    ring->samples[ring->write_idx & AP_RING_MASK] = s;
    ring->write_idx++;
}

// Get pointer to write location (for DMA)
static inline ap_sample_t* ap_ring_write_ptr(ap_ring_t *ring) {
    return &ring->samples[ring->write_idx & AP_RING_MASK];
}

// Advance write pointer by n (after DMA completes)
static inline void ap_ring_write_advance(ap_ring_t *ring, uint32_t n) {
    ring->write_idx += n;
}

#endif // AUDIO_BUFFER_H

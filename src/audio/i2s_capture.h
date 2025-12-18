/**
 * Audio Pipeline - I2S DMA Capture
 *
 * Captures I2S audio from Neo Geo MV1C using PIO + DMA.
 * NEO-YSA2 outputs 16-bit linear PCM at ~55.5kHz.
 */

#ifndef I2S_CAPTURE_H
#define I2S_CAPTURE_H

#include "audio_common.h"
#include "audio_buffer.h"
#include "hardware/pio.h"

// I2S capture configuration
typedef struct
{
    uint pin_bck; // Bit clock pin
    uint pin_dat; // Data pin
    uint pin_ws;  // Word select (LRCK) pin
    PIO pio;      // PIO instance to use
    uint sm;      // State machine to use
} i2s_capture_config_t;

// I2S capture state
typedef struct
{
    i2s_capture_config_t config;
    ap_ring_t *ring; // Output ring buffer
    volatile uint32_t samples_captured;
    volatile uint32_t overflows;
    bool running;

    // DMA state
    int dma_chan;
    uint32_t *dma_buffer;    // Local buffer for DMA to write to (raw PIO words)
    uint32_t dma_buffer_idx; // Current read position in dma_buffer

    // For sample rate measurement
    uint32_t last_sample_count;
    uint64_t last_measure_time;
    uint32_t measured_rate;
} i2s_capture_t;

// Initialize I2S capture (does not start yet)
bool i2s_capture_init(i2s_capture_t *cap, const i2s_capture_config_t *config, ap_ring_t *ring);

// Start capturing
void i2s_capture_start(i2s_capture_t *cap);

// Stop capturing
void i2s_capture_stop(i2s_capture_t *cap);

// Poll for new samples - call this frequently from main loop
// Returns number of samples captured this call
uint32_t i2s_capture_poll(i2s_capture_t *cap);

// Get measured sample rate (updated by poll)
uint32_t i2s_capture_get_sample_rate(i2s_capture_t *cap);

#endif // I2S_CAPTURE_H

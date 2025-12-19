/**
 * I2S Capture Implementation (Polling Mode)
 *
 * Uses PIO to capture I2S data, polled from main loop.
 * Simpler and more reliable than DMA - avoids interrupt conflicts with DVI.
 */

#include "i2s_capture.h"
#include "i2s_capture.pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// DMA buffer must be large enough to hold samples between polls
// At 55.5 kHz and 60 fps: ~1850 words/frame. Use 4096 for ~2 frames of headroom.
#define I2S_DMA_BUFFER_SIZE 4096
#define I2S_DMA_BUFFER_MASK (I2S_DMA_BUFFER_SIZE - 1)

// Aligned buffer for DMA ring wrapping (4096 words = 16384 bytes)
static uint32_t g_dma_buffer[I2S_DMA_BUFFER_SIZE] __attribute__((aligned(16384)));

bool i2s_capture_init(i2s_capture_t *cap, const i2s_capture_config_t *config, ap_ring_t *ring)
{
    cap->config = *config;
    cap->ring = ring;
    cap->samples_captured = 0;
    cap->overflows = 0;
    cap->running = false;
    cap->last_sample_count = 0;
    cap->last_measure_time = 0;
    cap->measured_rate = 0;

    // Initialize DMA state
    cap->dma_buffer = g_dma_buffer;
    cap->dma_buffer_idx = 0;
    cap->dma_chan = dma_claim_unused_channel(true);

    // Initialize GPIO pins as inputs BEFORE PIO takes over
    gpio_init(config->pin_bck);
    gpio_init(config->pin_dat);
    gpio_init(config->pin_ws);
    gpio_set_dir(config->pin_bck, GPIO_IN);
    gpio_set_dir(config->pin_dat, GPIO_IN);
    gpio_set_dir(config->pin_ws, GPIO_IN);

    // CRITICAL: Set gpio_base BEFORE adding the PIO program!
    pio_set_gpio_base(config->pio, 16);

    // Add PIO program
    uint offset = pio_add_program(config->pio, &i2s_capture_program);

    // Initialize PIO state machine
    i2s_capture_program_init(config->pio, config->sm, offset);

    // Configure DMA
    dma_channel_config c = dma_channel_get_default_config(cap->dma_chan);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(config->pio, config->sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

    // Enable ring wrapping for destination (2^14 bytes = 16384 bytes = 4096 words)
    channel_config_set_ring(&c, true, 14);

    dma_channel_configure(
        cap->dma_chan,
        &c,
        cap->dma_buffer,               // Destination
        &config->pio->rxf[config->sm], // Source
        0xFFFFFFFF,                    // Count (run "forever")
        false                          // Don't start yet
    );

    return true;
}

void i2s_capture_start(i2s_capture_t *cap)
{
    if (cap->running)
        return;

    // Reset counters
    cap->samples_captured = 0;
    cap->overflows = 0;
    cap->last_sample_count = 0;
    cap->last_measure_time = time_us_64();
    cap->dma_buffer_idx = 0;

    // Clear any stale data in PIO FIFO
    while (!pio_sm_is_rx_fifo_empty(cap->config.pio, cap->config.sm))
    {
        pio_sm_get(cap->config.pio, cap->config.sm);
    }

    // Clear DMA buffer to be safe
    memset(cap->dma_buffer, 0, I2S_DMA_BUFFER_SIZE * sizeof(uint32_t));

    // Start DMA
    dma_channel_set_write_addr(cap->dma_chan, cap->dma_buffer, true);

    // Enable PIO state machine
    pio_sm_set_enabled(cap->config.pio, cap->config.sm, true);

    cap->running = true;
}

void i2s_capture_stop(i2s_capture_t *cap)
{
    if (!cap->running)
        return;

    // Stop DMA
    dma_channel_abort(cap->dma_chan);

    // Disable PIO state machine
    pio_sm_set_enabled(cap->config.pio, cap->config.sm, false);

    cap->running = false;
}

uint32_t i2s_capture_poll(i2s_capture_t *cap)
{
    if (!cap->running)
        return 0;

    uint32_t count = 0;

    // Get current DMA write position from the WRITE_ADDR register
    uint32_t write_ptr = dma_hw->ch[cap->dma_chan].write_addr;
    uint32_t write_idx = (write_ptr - (uint32_t)cap->dma_buffer) / sizeof(uint32_t);

    // Read all samples written by DMA since last poll
    // Each sample is 2 words (Left then Right)
    while (cap->dma_buffer_idx != write_idx)
    {
        // Check if we have at least 2 words (one stereo sample)
        uint32_t next_idx = (cap->dma_buffer_idx + 1) & I2S_DMA_BUFFER_MASK;
        if (next_idx == write_idx)
            break;

        uint32_t raw_l = cap->dma_buffer[cap->dma_buffer_idx];
        uint32_t raw_r = cap->dma_buffer[next_idx];

        // Move pointer forward by 2
        cap->dma_buffer_idx = (next_idx + 1) & I2S_DMA_BUFFER_MASK;

        // NEO-YSA2 (MV1C) outputs 24-bit frames in Right-Justified format (MODE=1).
        // The 16-bit PCM data is in the lower 16 bits of the 24-bit window.
        ap_sample_t sample;
        sample.left = (int16_t)(raw_l & 0xFFFF);
        sample.right = (int16_t)(raw_r & 0xFFFF);

        if (ap_ring_free(cap->ring) > 0)
        {
            ap_ring_write(cap->ring, sample);
            cap->samples_captured++;
            count++;
        }
        else
        {
            cap->overflows++;
        }
    }

    // Update sample rate measurement every 500ms
    uint64_t now = time_us_64();
    uint64_t elapsed = now - cap->last_measure_time;
    if (elapsed >= 500000)
    {
        uint32_t samples_since = cap->samples_captured - cap->last_sample_count;
        cap->measured_rate = (uint32_t)((uint64_t)samples_since * 1000000 / elapsed);
        cap->last_sample_count = cap->samples_captured;
        cap->last_measure_time = now;
    }

    return count;
}

uint32_t i2s_capture_get_sample_rate(i2s_capture_t *cap)
{
    return cap->measured_rate;
}

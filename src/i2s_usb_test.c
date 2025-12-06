/**
 * I2S USB Audio Test - MVS Digital Audio Capture to USB
 *
 * Captures I2S digital audio from MVS and streams to USB for playback on Mac.
 * No DVI/HDMI output - just USB streaming for testing the digital audio chain.
 *
 * Hardware:
 *   GPIO 22: I2S BCK (bit clock)
 *   GPIO 23: I2S DAT (data)
 *   GPIO 24: I2S WS (word select / LRCLK)
 *
 * Protocol (same as audio_test.c):
 *   Header: 0xAA 0x55 0x55 0xAA (4 bytes)
 *   Sample count: uint16_t little-endian (2 bytes) - number of STEREO samples
 *   Samples: interleaved L/R 16-bit signed samples (4 bytes per stereo sample)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "tusb.h"

#include "i2s_audio.pio.h"

// =============================================================================
// Pin Configuration
// =============================================================================

#define I2S_PIN_BCK 22  // Bit clock
#define I2S_PIN_DAT 23  // Data
#define I2S_PIN_WS  24  // Word select (LRCLK)

// =============================================================================
// Audio Configuration
// =============================================================================

// MVS YM2610 outputs ~55.5 kHz sample rate
// We'll report 48kHz for playback (slight pitch adjustment is acceptable for testing)
#define SAMPLE_RATE 55555

// Buffer size - smaller for lower latency and less USB pressure
#define SAMPLES_PER_BUFFER 128
#define BUFFER_SIZE_WORDS (SAMPLES_PER_BUFFER * 2)  // L+R samples as 32-bit words from PIO

// Sync header for USB protocol - more unique pattern
#define SYNC_BYTE_0 0xDE
#define SYNC_BYTE_1 0xAD
#define SYNC_BYTE_2 0xBE
#define SYNC_BYTE_3 0xEF

// =============================================================================
// Buffers
// =============================================================================

// Double-buffered DMA capture
// PIO pushes 32-bit words (lower 16 bits contain sample)
static uint32_t i2s_buffer_a[BUFFER_SIZE_WORDS];
static uint32_t i2s_buffer_b[BUFFER_SIZE_WORDS];
static volatile uint8_t current_capture_buffer = 0;
static volatile bool buffer_ready = false;

// Output buffer for USB (interleaved stereo 16-bit)
static int16_t usb_buffer[SAMPLES_PER_BUFFER * 2];

static PIO i2s_pio = pio0;
static uint i2s_sm = 0;
static int dma_chan;

// =============================================================================
// DMA Handler
// =============================================================================

static void i2s_dma_handler(void) {
    // Clear interrupt
    dma_hw->ints0 = 1u << dma_chan;

    // Mark buffer as ready
    buffer_ready = true;

    // Swap buffers and restart DMA
    current_capture_buffer = !current_capture_buffer;
    uint32_t *next_buffer = current_capture_buffer ? i2s_buffer_b : i2s_buffer_a;

    dma_channel_set_write_addr(dma_chan, next_buffer, false);
    dma_channel_set_trans_count(dma_chan, BUFFER_SIZE_WORDS, true);
}

// =============================================================================
// I2S Setup
// =============================================================================

static void setup_i2s(void) {
    // Configure I2S pins as inputs first
    gpio_init(I2S_PIN_DAT);
    gpio_init(I2S_PIN_WS);
    gpio_init(I2S_PIN_BCK);
    gpio_set_dir(I2S_PIN_DAT, GPIO_IN);
    gpio_set_dir(I2S_PIN_WS, GPIO_IN);
    gpio_set_dir(I2S_PIN_BCK, GPIO_IN);

    // Load PIO program
    uint offset = pio_add_program(i2s_pio, &i2s_rx_program);
    i2s_rx_program_init(i2s_pio, i2s_sm, offset, I2S_PIN_DAT, I2S_PIN_WS, I2S_PIN_BCK);
}

static void setup_dma(void) {
    // Claim DMA channel
    dma_chan = dma_claim_unused_channel(true);

    // Configure DMA to read from PIO FIFO
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);   // Always read from PIO FIFO
    channel_config_set_write_increment(&cfg, true);   // Write to buffer
    channel_config_set_dreq(&cfg, pio_get_dreq(i2s_pio, i2s_sm, false));  // PIO RX FIFO triggers DMA

    dma_channel_configure(
        dma_chan,
        &cfg,
        i2s_buffer_a,                // Initial write address
        &i2s_pio->rxf[i2s_sm],       // Read from PIO RX FIFO
        BUFFER_SIZE_WORDS,           // Transfer count
        false                        // Don't start yet
    );

    // Enable DMA interrupt
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, i2s_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

// =============================================================================
// USB Transmission
// =============================================================================

static void process_and_send_audio(uint32_t *raw_buffer) {
    if (!tud_cdc_connected()) {
        return;
    }

    // With 16-bit autopush, each 32-bit FIFO word contains one 16-bit sample in lower bits
    for (int i = 0; i < SAMPLES_PER_BUFFER; i++) {
        usb_buffer[i * 2] = (int16_t)(raw_buffer[i * 2] & 0xFFFF);       // Left
        usb_buffer[i * 2 + 1] = (int16_t)(raw_buffer[i * 2 + 1] & 0xFFFF); // Right
    }

    // Send sync header + sample count
    uint8_t header[6] = {
        SYNC_BYTE_0,
        SYNC_BYTE_1,
        SYNC_BYTE_2,
        SYNC_BYTE_3,
        (SAMPLES_PER_BUFFER) & 0xFF,
        (SAMPLES_PER_BUFFER >> 8) & 0xFF
    };

    // Send header
    uint32_t header_remaining = sizeof(header);
    uint8_t *header_ptr = header;
    while (header_remaining > 0) {
        uint32_t available = tud_cdc_write_available();
        if (available > 0) {
            uint32_t chunk = (header_remaining < available) ? header_remaining : available;
            uint32_t written = tud_cdc_write(header_ptr, chunk);
            header_ptr += written;
            header_remaining -= written;
        }
        tud_task();
    }

    // Send stereo samples
    uint8_t *ptr = (uint8_t *)usb_buffer;
    uint32_t remaining = SAMPLES_PER_BUFFER * 2 * sizeof(int16_t);  // Stereo, 16-bit

    while (remaining > 0) {
        uint32_t available = tud_cdc_write_available();
        if (available > 0) {
            uint32_t chunk = (remaining < available) ? remaining : available;
            uint32_t written = tud_cdc_write(ptr, chunk);
            ptr += written;
            remaining -= written;
        }
        tud_task();
    }

    tud_cdc_write_flush();
}

// =============================================================================
// LED Functions
// =============================================================================

static inline void led_init(void) {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

static inline void led_toggle(void) {
    gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
}

static inline void led_on(void) {
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

static inline void led_off(void) {
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    // Initialize stdio for USB
    stdio_init_all();
    led_init();

    // Startup blink pattern
    for (int i = 0; i < 10; i++) {
        led_toggle();
        sleep_ms(100);
    }

    printf("\n");
    printf("========================================\n");
    printf("  I2S USB Audio Test - MVS Digital\n");
    printf("========================================\n");
    printf("I2S Pins: DAT=%d, WS=%d, BCK=%d\n", I2S_PIN_DAT, I2S_PIN_WS, I2S_PIN_BCK);
    printf("Sample Rate: %d Hz\n", SAMPLE_RATE);
    printf("Buffer Size: %d stereo samples\n", SAMPLES_PER_BUFFER);
    printf("\n");

    // Quick GPIO test before PIO takes over
    printf("GPIO signal check (5 samples):\n");
    for (int i = 0; i < 5; i++) {
        printf("  BCK=%d DAT=%d WS=%d\n",
               gpio_get(I2S_PIN_BCK), gpio_get(I2S_PIN_DAT), gpio_get(I2S_PIN_WS));
        sleep_ms(50);
    }
    printf("\n");

    // Setup I2S and DMA
    printf("Initializing I2S PIO...\n");
    setup_i2s();
    printf("Initializing DMA...\n");
    setup_dma();

    printf("Starting I2S capture in 2 seconds...\n");
    printf("Run: python i2s_audio_test.py\n\n");
    stdio_flush();
    sleep_ms(2000);  // Wait for printf to finish before streaming

    // Start DMA
    dma_channel_start(dma_chan);

    led_on();

    uint32_t packet_count = 0;
    bool has_nonzero = false;

    // NO printf in main loop - it corrupts the USB audio stream!
    while (true) {
        tud_task();

        if (buffer_ready) {
            buffer_ready = false;

            // Process the buffer that was just filled
            uint32_t *ready_buffer = current_capture_buffer ? i2s_buffer_a : i2s_buffer_b;

            // Check if we have any non-zero data (for LED indicator)
            has_nonzero = false;
            for (int i = 0; i < 16; i++) {
                if ((ready_buffer[i] & 0xFFFF) != 0) {
                    has_nonzero = true;
                    break;
                }
            }

            process_and_send_audio(ready_buffer);

            packet_count++;

            // LED: fast blink if data, slow blink if zeros
            if (has_nonzero) {
                if (packet_count % 50 == 0) led_toggle();
            } else {
                if (packet_count % 500 == 0) led_toggle();
            }
        }
    }

    return 0;
}

/**
 * YM2610 Audio USB Capture
 *
 * Captures raw audio data from YM2610 serial interface via PIO+DMA
 * and streams to USB. No DVI, no processing - just raw capture.
 *
 * Pin configuration:
 *   GPIO 21: BCK (bit clock)
 *   GPIO 23: DAT (serial data)
 *   GPIO 24: WS (word select)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "ym2610_audio.pio.h"

// =============================================================================
// Pin Configuration
// =============================================================================
#define PIN_BCK  2
#define PIN_DAT  3
#define PIN_WS   4

// =============================================================================
// Buffer Configuration
// =============================================================================
// Each sample is 32 bits (16-bit L + 16-bit R from two PIO reads)
// At 55.5kHz, 1 second = 55500 samples = 222KB
// Use double buffering: capture to one while sending other
#define SAMPLES_PER_BUFFER 4096  // ~74ms at 55kHz
#define BUFFER_SIZE_WORDS (SAMPLES_PER_BUFFER * 2)  // 2 words per stereo sample (L, R)

static uint32_t audio_buf_a[BUFFER_SIZE_WORDS];
static uint32_t audio_buf_b[BUFFER_SIZE_WORDS];
static volatile uint8_t current_capture_buf = 0;  // 0=A, 1=B
static volatile bool buffer_ready = false;
static volatile uint32_t buffers_captured = 0;

// DMA channels
static int dma_chan;
static PIO audio_pio;
static uint audio_sm;
static uint pio_offset;

// =============================================================================
// DMA IRQ Handler - called when buffer is full
// =============================================================================
static void dma_irq_handler(void) {
    // Clear interrupt
    dma_hw->ints0 = 1u << dma_chan;

    // Mark buffer as ready
    buffer_ready = true;
    buffers_captured++;

    // Switch to other buffer and restart DMA
    current_capture_buf = !current_capture_buf;
    uint32_t *next_buf = current_capture_buf ? audio_buf_b : audio_buf_a;

    dma_channel_set_write_addr(dma_chan, next_buf, false);
    dma_channel_set_trans_count(dma_chan, BUFFER_SIZE_WORDS, true);  // Auto-start
}

// =============================================================================
// Initialize Audio Capture
// =============================================================================
static void init_audio_capture(void) {
    // Configure audio pins as inputs
    gpio_init(PIN_BCK);
    gpio_init(PIN_DAT);
    gpio_init(PIN_WS);
    gpio_set_dir(PIN_BCK, GPIO_IN);
    gpio_set_dir(PIN_DAT, GPIO_IN);
    gpio_set_dir(PIN_WS, GPIO_IN);

    // Initialize PIO
    audio_pio = pio1;
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    pio_offset = pio_add_program(audio_pio, &ym2610_rx_program);
    ym2610_rx_program_init(audio_pio, audio_sm, pio_offset, PIN_DAT);

    // Initialize DMA
    dma_chan = dma_claim_unused_channel(true);

    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(audio_pio, audio_sm, false));

    dma_channel_configure(
        dma_chan,
        &cfg,
        audio_buf_a,                        // Initial write address
        &audio_pio->rxf[audio_sm],          // Read from PIO FIFO
        BUFFER_SIZE_WORDS,                  // Transfer count
        false                               // Don't start yet
    );

    // Enable DMA IRQ
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

// =============================================================================
// Start Capture
// =============================================================================
static void start_capture(void) {
    // Clear buffers
    memset(audio_buf_a, 0, sizeof(audio_buf_a));
    memset(audio_buf_b, 0, sizeof(audio_buf_b));

    current_capture_buf = 0;
    buffer_ready = false;
    buffers_captured = 0;

    // Reset PIO
    pio_sm_set_enabled(audio_pio, audio_sm, false);
    pio_sm_clear_fifos(audio_pio, audio_sm);
    pio_sm_restart(audio_pio, audio_sm);
    pio_sm_exec(audio_pio, audio_sm, pio_encode_jmp(pio_offset));

    // Start DMA
    dma_channel_set_write_addr(dma_chan, audio_buf_a, false);
    dma_channel_set_trans_count(dma_chan, BUFFER_SIZE_WORDS, true);

    // Enable PIO
    pio_sm_set_enabled(audio_pio, audio_sm, true);
}

// =============================================================================
// USB Send Buffer (uses stdio which handles USB CDC)
// =============================================================================
static void send_buffer_usb(uint32_t *buf, uint32_t word_count) {
    // Send raw binary data via putchar (which goes through USB CDC)
    uint8_t *ptr = (uint8_t *)buf;
    uint32_t bytes = word_count * 4;

    for (uint32_t i = 0; i < bytes; i++) {
        putchar_raw(ptr[i]);
    }
    stdio_flush();
}

// =============================================================================
// Main
// =============================================================================
int main() {
    // Basic init
    stdio_init_all();

    // LED for status
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Wait for USB enumeration
    for (int i = 0; i < 30; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, i & 1);
        sleep_ms(100);
    }
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Initialize audio capture
    init_audio_capture();

    // Wait for USB connection and 'S' command to start
    printf("\n\n");
    printf("=== YM2610 Audio USB Capture ===\n");
    printf("Pins: BCK=%d, DAT=%d, WS=%d\n", PIN_BCK, PIN_DAT, PIN_WS);
    printf("Buffer: %d samples (%d bytes)\n", SAMPLES_PER_BUFFER, BUFFER_SIZE_WORDS * 4);
    printf("\n");
    printf("Commands:\n");
    printf("  S - Start streaming (binary: 4 bytes header + raw data)\n");
    printf("  D - Dump 2000 samples as hex (for analysis)\n");
    printf("  T - Test: show raw PIO values\n");
    printf("\n");
    printf("Ready.\n");

    bool streaming = false;

    while (true) {
        // Check for commands (non-blocking with timeout)
        int c = getchar_timeout_us(1000);  // 1ms timeout

        if (c != PICO_ERROR_TIMEOUT) {
            char cmd = (char)c;

            if (cmd == 'S' || cmd == 's') {
                // Start binary streaming - no printf to avoid mixing with binary data
                // Just send the header directly after a flush
                stdio_flush();
                sleep_ms(200);

                // Send header as raw bytes
                putchar_raw('A');
                putchar_raw('U');
                putchar_raw('D');
                putchar_raw('O');
                stdio_flush();

                streaming = true;
                start_capture();
            }
            else if (cmd == 'X' || cmd == 'x') {
                // Stop streaming
                streaming = false;
                pio_sm_set_enabled(audio_pio, audio_sm, false);
                dma_channel_abort(dma_chan);
                printf("\nStopped.\n");
            }
            else if (cmd == 'D' || cmd == 'd') {
                // Dump mode - capture and print as hex
                printf("Capturing 2000 samples...\n");
                sleep_ms(100);

                start_capture();

                // Wait for first buffer
                while (!buffer_ready) {
                    tight_loop_contents();
                }

                // Stop capture
                pio_sm_set_enabled(audio_pio, audio_sm, false);
                dma_channel_abort(dma_chan);

                // Print samples
                uint32_t *buf = (current_capture_buf == 0) ? audio_buf_b : audio_buf_a;

                printf("RAW_DUMP_START\n");
                for (int i = 0; i < 2000 && i < SAMPLES_PER_BUFFER; i++) {
                    uint16_t l = buf[i * 2] & 0xFFFF;
                    uint16_t r = buf[i * 2 + 1] & 0xFFFF;
                    printf("%04X,%04X\n", l, r);
                }
                printf("RAW_DUMP_END\n");

                buffer_ready = false;
            }
            else if (cmd == 'T' || cmd == 't') {
                // Test mode - show a few raw values
                printf("Reading 20 raw PIO values...\n");

                // Enable PIO briefly
                pio_sm_set_enabled(audio_pio, audio_sm, false);
                pio_sm_clear_fifos(audio_pio, audio_sm);
                pio_sm_restart(audio_pio, audio_sm);
                pio_sm_exec(audio_pio, audio_sm, pio_encode_jmp(pio_offset));
                pio_sm_set_enabled(audio_pio, audio_sm, true);

                for (int i = 0; i < 20; i++) {
                    // Wait for data with timeout
                    int timeout = 100000;
                    while (pio_sm_is_rx_fifo_empty(audio_pio, audio_sm) && timeout > 0) {
                        timeout--;
                    }

                    if (timeout <= 0) {
                        printf("  %d: TIMEOUT\n", i);
                    } else {
                        uint32_t val = pio_sm_get(audio_pio, audio_sm);
                        printf("  %d: 0x%08X (16-bit: 0x%04X)\n", i, val, val & 0xFFFF);
                    }
                }

                pio_sm_set_enabled(audio_pio, audio_sm, false);
            }
        }

        // If streaming, send completed buffers
        if (streaming && buffer_ready) {
            // Get the buffer that was just filled (opposite of current capture buffer)
            uint32_t *send_buf = (current_capture_buf == 0) ? audio_buf_b : audio_buf_a;

            send_buffer_usb(send_buf, BUFFER_SIZE_WORDS);
            buffer_ready = false;

            // Blink LED
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }
    }

    return 0;
}

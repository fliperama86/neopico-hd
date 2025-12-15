#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "mvs_capture.pio.h"

// =============================================================================
// Pin Configuration
// =============================================================================
// GPIO 0:     PCLK (C2)
// GPIO 1-5:   G4-G0 (Green, reversed bit order)
// GPIO 6-10:  B0-B4 (Blue)
// GPIO 11-15: R0-R4 (Red)
// GPIO 22:    CSYNC

#define PIN_BASE 0    // Base pin for 16-bit capture (PCLK + RGB data)
#define PIN_PCLK 0    // Pixel clock
#define PIN_CSYNC 22  // Composite sync

// Lookup table to reverse 5-bit green value (G4G3G2G1G0 -> G0G1G2G3G4)
static const uint8_t green_reverse_lut[32] = {
    0x00, 0x10, 0x08, 0x18, 0x04, 0x14, 0x0C, 0x1C,
    0x02, 0x12, 0x0A, 0x1A, 0x06, 0x16, 0x0E, 0x1E,
    0x01, 0x11, 0x09, 0x19, 0x05, 0x15, 0x0D, 0x1D,
    0x03, 0x13, 0x0B, 0x1B, 0x07, 0x17, 0x0F, 0x1F
};

// =============================================================================
// MVS Timing Constants
// =============================================================================

#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE_START 27 // Reduced from 57 to account for CSYNC sync

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 224

// =============================================================================
// Buffer Configuration
// =============================================================================

// Pico 2 has 520KB RAM - plenty of room!
// Full frame needs: (224 + 22 offset + 10 margin) × 384 × 16 / 32 = 49,152 words
// Frame buffer: 320 × 224 = 71,680 bytes (~72KB)
// Available: ~520KB - 72KB - 20KB stack = 428KB = 107,000 words
#define RAW_BUFFER_WORDS 50000 // ~200KB - plenty of margin for Pico 2

// RGB565 frame buffer (2 bytes per pixel)
#define FRAME_SIZE_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 2)

#define SYNC_WORD_1 0xAA55
#define SYNC_WORD_2 0x55AA

static uint32_t raw_buffer[RAW_BUFFER_WORDS];

// Double buffering - capture to one while sending the other
static uint16_t frame_buffer_a[FRAME_WIDTH * FRAME_HEIGHT]; // RGB565
static uint16_t frame_buffer_b[FRAME_WIDTH * FRAME_HEIGHT]; // RGB565
static uint8_t current_buffer = 0; // 0 = A, 1 = B

static int dma_chan;

static volatile bool frame_captured = false;
static uint32_t frame_count = 0;
static uint32_t capture_offset_lines = 16; // Reduced to fit full active area in buffer

// =============================================================================
// Debug LED Functions
// =============================================================================

static inline void led_init(void)
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

static inline void led_on(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

static inline void led_off(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

// Blink pattern: N short blinks, then pause
// Use to identify which stage we reached
static void led_blink_code(int code)
{
    for (int i = 0; i < code; i++)
    {
        led_on();
        sleep_ms(150);
        led_off();
        sleep_ms(150);
    }
    sleep_ms(500);
}

// Rapid blink = stuck/error
static void led_error_loop(int code)
{
    while (true)
    {
        led_blink_code(code);
        sleep_ms(1000);
    }
}

// =============================================================================
// VSYNC Detection State Machine
// =============================================================================

typedef enum
{
    SYNC_WAIT_FIRST_VSYNC,
    SYNC_WAIT_SECOND_VSYNC,
    SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC,
    SYNC_READY_TO_CAPTURE
} sync_state_t;

static inline void drain_sync_fifo(PIO pio, uint sm)
{
    while (!pio_sm_is_rx_fifo_empty(pio, sm))
    {
        pio_sm_get(pio, sm);
    }
}

// Wait for frame boundary with timeout - returns false if no signal
static bool wait_for_frame_start_timeout(PIO pio, uint sm_sync, uint32_t timeout_ms)
{
    sync_state_t state = SYNC_WAIT_FIRST_VSYNC;
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);

    while (state != SYNC_READY_TO_CAPTURE)
    {
        // Check timeout
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false;
        }

        // Non-blocking FIFO check
        if (pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        switch (state)
        {
        case SYNC_WAIT_FIRST_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else if (equ_count >= 8)
            {
                state = SYNC_WAIT_SECOND_VSYNC;
                equ_count = 0;
            }
            else
            {
                equ_count = 0;
            }
            break;

        case SYNC_WAIT_SECOND_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else if (equ_count >= 8)
            {
                drain_sync_fifo(pio, sm_sync);
                state = SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC;
                equ_count = 0;
            }
            else
            {
                equ_count = 0;
            }
            break;

        case SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else
            {
                state = SYNC_READY_TO_CAPTURE;
            }
            break;

        case SYNC_READY_TO_CAPTURE:
            break;
        }
    }
    return true;
}

static bool wait_for_vsync_timeout(PIO pio, uint sm_sync, uint32_t timeout_ms)
{
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);

    while (true)
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (is_short_pulse)
        {
            equ_count++;
        }
        else
        {
            if (equ_count >= 8)
            {
                return true;
            }
            equ_count = 0;
        }
    }
}

// Wait for VSYNC, then wait for first stable HSYNC - more deterministic
static bool wait_for_vsync_and_hsync(PIO pio, uint sm_sync, uint32_t timeout_ms)
{
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    bool in_vsync = false;

    while (true)
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (!in_vsync)
        {
            // Looking for VSYNC (8+ short pulses)
            if (is_short_pulse)
            {
                equ_count++;
            }
            else
            {
                if (equ_count >= 8)
                {
                    in_vsync = true;
                    equ_count = 0;
                    // Drain any queued pulses to get closer to real-time
                    drain_sync_fifo(pio, sm_sync);
                }
                else
                {
                    equ_count = 0;
                }
            }
        }
        else
        {
            // In VSYNC region, wait for first normal HSYNC
            if (is_short_pulse)
            {
                equ_count++; // Still in equalizing pulses
            }
            else
            {
                // First normal HSYNC after VSYNC - this is our trigger point!
                return true;
            }
        }
    }
}

// =============================================================================
// Frame Processing
// =============================================================================

// Extract pixels from raw capture buffer
// Input: 16 bits per pixel:
//   GPIO 0:     PCLK (ignored)
//   GPIO 1-5:   G4-G0 (Green, reversed - use LUT)
//   GPIO 6-10:  B0-B4 (Blue)
//   GPIO 11-15: R0-R4 (Red)
// Output: RGB565 (RRRRRGGGGGGBBBBB)
static void process_frame(uint32_t *raw_buf, uint16_t *frame_buf, uint32_t words_captured)
{
    uint32_t raw_bit_idx = capture_offset_lines * NEO_H_TOTAL * 16;
    uint32_t out_idx = 0;

    for (uint32_t line = 0; line < FRAME_HEIGHT; line++)
    {
        raw_bit_idx += NEO_H_ACTIVE_START * 16;

        for (uint32_t x = 0; x < FRAME_WIDTH; x++)
        {
            uint32_t word_idx = raw_bit_idx / 32;
            uint32_t bit_idx = raw_bit_idx % 32;

            uint16_t pixel = 0;
            if (word_idx < words_captured)
            {
                uint32_t raw_val = raw_buf[word_idx] >> bit_idx;
                if (bit_idx > 16 && (word_idx + 1) < words_captured)
                {
                    raw_val |= raw_buf[word_idx + 1] << (32 - bit_idx);
                }

                // Extract RGB555 from GPIO 1-15 (bit 0 is PCLK, ignored)
                // GPIO 1-5:   G4-G0 (reversed, use LUT)
                // GPIO 6-10:  B0-B4
                // GPIO 11-15: R0-R4
                uint8_t g_raw = (raw_val >> 1) & 0x1F;  // bits 1-5 (reversed green)
                uint8_t b5 = (raw_val >> 6) & 0x1F;     // bits 6-10
                uint8_t r5 = (raw_val >> 11) & 0x1F;    // bits 11-15
                uint8_t g5 = green_reverse_lut[g_raw];  // reverse green bits

                // Convert to RGB565: RRRRR GGGGGG BBBBB
                // Expand G from 5 to 6 bits: g6 = (g5 << 1) | (g5 >> 4)
                uint8_t g6 = (g5 << 1) | (g5 >> 4);
                pixel = (r5 << 11) | (g6 << 5) | b5;
            }

            frame_buf[out_idx++] = pixel;
            raw_bit_idx += 16;
        }

        raw_bit_idx += (NEO_H_TOTAL - NEO_H_ACTIVE_START - FRAME_WIDTH) * 16;
    }
}

// =============================================================================
// USB Transmission
// =============================================================================

// Use TinyUSB directly for better throughput (stdio_usb uses it internally)
#include "tusb.h"

static bool send_frame_usb(uint16_t *frame_buf)
{
    if (!tud_cdc_connected())
    {
        return false;
    }

    // Send header
    uint8_t header[4] = {
        SYNC_WORD_1 & 0xFF,
        (SYNC_WORD_1 >> 8) & 0xFF,
        SYNC_WORD_2 & 0xFF,
        (SYNC_WORD_2 >> 8) & 0xFF};

    tud_cdc_write(header, 4);

    // Send frame in chunks (cast to bytes)
    uint8_t *ptr = (uint8_t *)frame_buf;
    uint32_t remaining = FRAME_SIZE_BYTES;

    while (remaining > 0)
    {
        uint32_t available = tud_cdc_write_available();
        if (available > 0)
        {
            uint32_t chunk = (remaining < available) ? remaining : available;
            uint32_t written = tud_cdc_write(ptr, chunk);
            ptr += written;
            remaining -= written;
        }
        tud_task(); // Process USB
    }

    tud_cdc_write_flush();
    return true;
}

// =============================================================================
// DMA Configuration
// =============================================================================

static void setup_dma(PIO pio, uint sm_pixel)
{
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm_pixel, false));
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);

    dma_channel_configure(
        dma_chan,
        &cfg,
        raw_buffer,
        &pio->rxf[sm_pixel],
        RAW_BUFFER_WORDS,
        false);
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    // Stage 1: Basic init
    stdio_init_all();
    led_init();

    // Blink 1: We're alive
    led_blink_code(1);

    // Stage 2: USB init (handled by stdio_init_all)
    // Give USB time to enumerate
    for (int i = 0; i < 20; i++)
    {
        sleep_ms(100);
        led_on();
        sleep_ms(50);
        led_off();
    }

    // Blink 2: USB initialized
    led_blink_code(2);

    // Stage 3: PIO setup
    PIO pio = pio0;

    uint offset_sync = pio_add_program(pio, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio, true);
    mvs_sync_4a_program_init(pio, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    uint offset_pixel = pio_add_program(pio, &mvs_pixel_capture_program);
    uint sm_pixel = pio_claim_unused_sm(pio, true);
    mvs_pixel_capture_program_init(pio, sm_pixel, offset_pixel, PIN_BASE, PIN_CSYNC, PIN_PCLK);

    // Blink 3: PIO initialized
    led_blink_code(3);

    // Stage 4: DMA setup
    dma_chan = dma_claim_unused_channel(true);
    setup_dma(pio, sm_pixel);

    // Blink 4: DMA initialized
    led_blink_code(4);

    // Stage 5: Wait for sync signal (with timeout)
    // Try for 5 seconds, if no signal, enter error mode
    led_on(); // LED on while waiting for sync

    bool got_sync = wait_for_frame_start_timeout(pio, sm_sync, 5000);

    if (!got_sync)
    {
        // Error: No sync signal detected
        // Blink 5 repeatedly = stuck waiting for VSYNC
        led_error_loop(5);
    }

    // Blink 6: Got sync, starting capture
    led_blink_code(6);

    // Track if we have a frame ready to send
    bool have_frame_to_send = false;
    uint16_t *send_buf = NULL;

    // Main capture loop
    while (true)
    {
        // Toggle LED every 15 frames
        gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 15) & 1);

        // Send PREVIOUS frame while waiting for sync (overlapped!)
        if (have_frame_to_send)
        {
            send_frame_usb(send_buf);
        }

        // Wait for VSYNC then first HSYNC
        bool got_sync = wait_for_vsync_and_hsync(pio, sm_sync, 100);

        if (!got_sync)
        {
            continue;
        }

        // Start capture
        dma_channel_set_write_addr(dma_chan, raw_buffer, false);
        dma_channel_set_trans_count(dma_chan, RAW_BUFFER_WORDS, false);

        pio_sm_set_enabled(pio, sm_pixel, false);
        pio_sm_clear_fifos(pio, sm_pixel);
        pio_sm_restart(pio, sm_pixel);
        pio_sm_exec(pio, sm_pixel, pio_encode_jmp(offset_pixel));
        pio_sm_set_enabled(pio, sm_pixel, true);

        dma_channel_start(dma_chan);
        pio_sm_exec(pio, sm_sync, pio_encode_irq_set(false, 4));

        // Wait for frame end
        bool got_vsync = wait_for_vsync_timeout(pio, sm_sync, 100);

        if (!got_vsync)
        {
            dma_channel_abort(dma_chan);
            continue;
        }

        dma_channel_abort(dma_chan);
        uint32_t words_captured = RAW_BUFFER_WORDS - dma_channel_hw_addr(dma_chan)->transfer_count;

        // Get current buffer pointer
        uint16_t *capture_buf = current_buffer ? frame_buffer_b : frame_buffer_a;

        process_frame(raw_buffer, capture_buf, words_captured);

        // Set up this frame to be sent on NEXT iteration (while we capture)
        send_buf = capture_buf;
        have_frame_to_send = true;

        // Swap buffers for next frame
        current_buffer = !current_buffer;

        frame_count++;
    }

    return 0;
}
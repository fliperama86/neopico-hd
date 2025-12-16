/**
 * PIO-based Audio Capture - debug version to find bit alignment
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "audio_capture.pio.h"
#include "audio_config.h"

#define SAMPLE_RATE     55500
#define BUFFER_SAMPLES  1024

// Now 32-bit to hold full 24-bit samples
static uint32_t audio_buffer[2][BUFFER_SAMPLES * 2];
static volatile int active_buffer = 0;

static PIO pio;
static uint sm;
static int dma_chan;

void setup_dma_32(uint32_t *buffer, size_t count) {
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(dma_chan, &c, buffer, &pio->rxf[sm], count, true);
}

int main() {
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 30; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, i & 1);
        sleep_ms(100);
    }
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    pio = pio0;
    uint offset = pio_add_program(pio, &ym2610_capture_program);
    sm = pio_claim_unused_sm(pio, true);
    ym2610_capture_program_init(pio, sm, offset, AUDIO_PIN_DAT);

    dma_chan = dma_claim_unused_channel(true);

    printf("READY\n");
    stdio_flush();

    // Wait for any character to start
    while (getchar_timeout_us(100000) == PICO_ERROR_TIMEOUT) {
        gpio_xor_mask(1 << PICO_DEFAULT_LED_PIN);
    }

    printf("STREAM_START\n");
    stdio_flush();

    static int16_t stream_buf[BUFFER_SAMPLES * 2];
    setup_dma_32(audio_buffer[0], BUFFER_SAMPLES * 2);

    while (true) {
        dma_channel_wait_for_finish_blocking(dma_chan);

        int completed = active_buffer;
        active_buffer = 1 - active_buffer;

        setup_dma_32(audio_buffer[active_buffer], BUFFER_SAMPLES * 2);

        // 24 bits in bits 23-0, take upper 16 bits (bits 23-8)
        for (int i = 0; i < BUFFER_SAMPLES * 2; i++) {
            stream_buf[i] = (int16_t)(audio_buffer[completed][i] >> 8);
        }

        fwrite(stream_buf, sizeof(int16_t), BUFFER_SAMPLES * 2, stdout);
        fflush(stdout);

        gpio_xor_mask(1 << PICO_DEFAULT_LED_PIN);
    }

    return 0;
}

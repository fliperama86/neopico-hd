/**
 * NeoPico-HD standalone PCM1802 USB capture diagnostic.
 *
 * This target intentionally initializes no video, HSTX, HDMI, SRC, filters, or
 * volume processing. It captures the PCM1802's standard-I2S DOUT as signed
 * 24-bit stereo samples and forwards either all 24 bits or the upper 16 bits
 * in framed packets over a TinyUSB CDC port.
 *
 * The host must complete an INFO -> HELLO -> START -> STARTED handshake before
 * capture begins. Every stream packet carries a session ID, sequence number,
 * source-frame index, cumulative DMA-drop count, and CRC32 so a receiver can
 * attach at any time without accepting stale or partial data.
 */

#include "pico/stdlib.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp/board_api.h"
#include "i2s_capture.pio.h"
#include "tusb.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The PCM1802 USB capture protocol requires a little-endian target"
#endif

// PCM1802 standard-I2S wiring used by the production NeoPico-HD firmware.
#define PCM1802_PIN_DOUT 22u
#define PCM1802_PIN_LRCK 23u
#define PCM1802_PIN_BCK 24u

#define PCM1802_SAMPLE_RATE 48000u
#define PCM1802_CHANNELS 2u

#ifndef NEOPICO_PCM1802_USB_BITS_PER_SAMPLE
#define NEOPICO_PCM1802_USB_BITS_PER_SAMPLE 24
#endif

#if NEOPICO_PCM1802_USB_BITS_PER_SAMPLE != 16 && NEOPICO_PCM1802_USB_BITS_PER_SAMPLE != 24
#error "NEOPICO_PCM1802_USB_BITS_PER_SAMPLE must be 16 or 24"
#endif

#define PCM1802_BITS_PER_SAMPLE ((uint32_t)NEOPICO_PCM1802_USB_BITS_PER_SAMPLE)
#define PCM1802_BYTES_PER_SAMPLE (PCM1802_BITS_PER_SAMPLE / 8u)

// Capture always remains 24-bit. The optional 16-bit transport discards only
// the least-significant byte while packets are staged for USB.
#define AUDIO_FRAMES_PER_PACKET 128u
#define AUDIO_WORDS_PER_PACKET (AUDIO_FRAMES_PER_PACKET * PCM1802_CHANNELS)
#define AUDIO_BYTES_PER_FRAME (PCM1802_CHANNELS * PCM1802_BYTES_PER_SAMPLE)
#define AUDIO_PAYLOAD_BYTES (AUDIO_FRAMES_PER_PACKET * AUDIO_BYTES_PER_FRAME)

#if NEOPICO_PCM1802_USB_BITS_PER_SAMPLE == 16
_Static_assert(AUDIO_PAYLOAD_BYTES == 512u, "Unexpected 16-bit audio payload size");
#else
_Static_assert(AUDIO_PAYLOAD_BYTES == 768u, "Unexpected 24-bit audio payload size");
#endif

// The DMA ring holds about 85 ms at 48 kHz. Keep one packet of separation
// between the reader and writer so DMA cannot overwrite a packet as it is
// copied into the USB staging buffer.
#define DMA_RING_WORDS 8192u
#define DMA_RING_MASK (DMA_RING_WORDS - 1u)
#define DMA_RING_BYTES (DMA_RING_WORDS * sizeof(uint32_t))
#define DMA_SAFE_BACKLOG_WORDS (DMA_RING_WORDS - AUDIO_WORDS_PER_PACKET)

#define PROTOCOL_VERSION 1u
#define COMMAND_MAGIC "NPCMCMD1"
#define STREAM_MAGIC "NPCMDAT1"

enum command_type {
    COMMAND_INFO = 1,
    COMMAND_START = 2,
    COMMAND_STOP = 3,
};

enum packet_type {
    PACKET_HELLO = 1,
    PACKET_STARTED = 2,
    PACKET_AUDIO = 3,
    PACKET_STOPPED = 4,
    PACKET_ERROR = 5,
};

typedef struct __attribute__((packed)) {
    uint8_t magic[8];
    uint8_t version;
    uint8_t command;
    uint16_t packet_bytes;
    uint32_t session_id;
    uint32_t crc32;
} command_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[8];
    uint8_t version;
    uint8_t type;
    uint16_t header_bytes;
    uint32_t session_id;
    uint32_t sequence;
    uint32_t sample_rate;
    uint64_t first_frame;
    uint16_t frame_count;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t payload_bytes;
    uint64_t dropped_frames;
} stream_header_t;

_Static_assert(sizeof(command_packet_t) == 20u, "Unexpected command packet layout");
_Static_assert(sizeof(stream_header_t) == 48u, "Unexpected stream header layout");

#define STREAM_PACKET_MAX_BYTES (sizeof(stream_header_t) + AUDIO_PAYLOAD_BYTES + sizeof(uint32_t))

static uint32_t crc32_table[256];

static uint32_t dma_ring[DMA_RING_WORDS] __attribute__((aligned(DMA_RING_BYTES)));
static PIO capture_pio = pio2;
static const uint capture_sm = 0;
static uint capture_program_offset;
static int capture_dma_channel;
static volatile uint32_t capture_dma_wraps;
static bool capture_running;
static uint64_t capture_read_words;
static uint64_t capture_dropped_frames;

static uint8_t tx_packet[STREAM_PACKET_MAX_BYTES] __attribute__((aligned(4)));
static uint32_t tx_packet_bytes;
static uint32_t tx_packet_offset;

static uint8_t command_buffer[sizeof(command_packet_t)];
static uint32_t command_buffer_used;

static bool session_armed;
static bool streaming;
static uint32_t active_session_id;
static uint32_t packet_sequence;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t value = i;
        for (uint32_t bit = 0; bit < 8u; ++bit) {
            value = (value >> 1u) ^ ((value & 1u) ? 0xEDB88320u : 0u);
        }
        crc32_table[i] = value;
    }
}

static uint32_t crc32_calculate(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void __isr capture_dma_irq0_handler(void)
{
    const uint32_t mask = 1u << (uint)capture_dma_channel;
    if (dma_hw->ints0 & mask) {
        dma_hw->ints0 = mask;
        capture_dma_wraps++;
    }
}

static void capture_init(void)
{
    gpio_init(PCM1802_PIN_DOUT);
    gpio_init(PCM1802_PIN_LRCK);
    gpio_init(PCM1802_PIN_BCK);
    gpio_set_dir(PCM1802_PIN_DOUT, GPIO_IN);
    gpio_set_dir(PCM1802_PIN_LRCK, GPIO_IN);
    gpio_set_dir(PCM1802_PIN_BCK, GPIO_IN);
    gpio_disable_pulls(PCM1802_PIN_DOUT);
    gpio_disable_pulls(PCM1802_PIN_LRCK);
    gpio_disable_pulls(PCM1802_PIN_BCK);
    gpio_set_input_hysteresis_enabled(PCM1802_PIN_DOUT, true);
    gpio_set_input_hysteresis_enabled(PCM1802_PIN_LRCK, true);
    gpio_set_input_hysteresis_enabled(PCM1802_PIN_BCK, true);

    pio_set_gpio_base(capture_pio, 0);
    capture_program_offset = pio_add_program(capture_pio, &i2s_capture_pcm1802_program);
    i2s_capture_pcm1802_program_init(capture_pio, capture_sm, capture_program_offset, PCM1802_PIN_DOUT,
                                     PCM1802_PIN_LRCK, PCM1802_PIN_BCK);

    capture_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config((uint)capture_dma_channel);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_dreq(&config, pio_get_dreq(capture_pio, capture_sm, false));
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_ring(&config, true, 15u); // 2^15 bytes = 8192 words.
    channel_config_set_high_priority(&config, true);

    dma_channel_configure((uint)capture_dma_channel, &config, dma_ring, &capture_pio->rxf[capture_sm],
                          dma_encode_transfer_count_with_self_trigger(DMA_RING_WORDS), false);

    dma_channel_set_irq0_enabled((uint)capture_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, capture_dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

static void capture_stop(void)
{
    if (!capture_running) {
        return;
    }

    pio_sm_set_enabled(capture_pio, capture_sm, false);
    dma_channel_abort((uint)capture_dma_channel);
    dma_hw->ints0 = 1u << (uint)capture_dma_channel;
    capture_running = false;
}

static void capture_start(void)
{
    capture_stop();

    pio_sm_set_enabled(capture_pio, capture_sm, false);
    pio_sm_restart(capture_pio, capture_sm);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_exec(capture_pio, capture_sm, pio_encode_jmp(capture_program_offset));

    memset(dma_ring, 0, sizeof(dma_ring));
    dma_channel_abort((uint)capture_dma_channel);
    dma_hw->ints0 = 1u << (uint)capture_dma_channel;

    uint32_t irq_state = save_and_disable_interrupts();
    capture_dma_wraps = 0;
    restore_interrupts(irq_state);

    capture_read_words = 0;
    capture_dropped_frames = 0;

    dma_channel_set_write_addr((uint)capture_dma_channel, dma_ring, false);
    dma_channel_set_transfer_count((uint)capture_dma_channel,
                                   dma_encode_transfer_count_with_self_trigger(DMA_RING_WORDS), false);
    dma_channel_start((uint)capture_dma_channel);
    pio_sm_set_enabled(capture_pio, capture_sm, true);
    capture_running = true;
}

static uint64_t capture_words_written(void)
{
    const uint32_t dma_mask = 1u << (uint)capture_dma_channel;
    const uintptr_t ring_base = (uintptr_t)dma_ring;

    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t wraps;
    uint32_t pending_before;
    uint32_t pending_after;
    uintptr_t write_address;

    // DMA continues while CPU interrupts are masked. Retry if it crosses the
    // ring boundary during the snapshot so the wrap count and write address
    // always describe the same DMA lap.
    do {
        wraps = capture_dma_wraps;
        pending_before = dma_hw->ints0 & dma_mask;
        write_address = (uintptr_t)dma_hw->ch[capture_dma_channel].write_addr;
        pending_after = dma_hw->ints0 & dma_mask;
    } while (pending_before != pending_after);

    if (pending_before) {
        // The self-triggering DMA has wrapped, but its IRQ has not yet run.
        wraps++;
    }
    restore_interrupts(irq_state);

    const uint32_t write_index = (uint32_t)((write_address - ring_base) / sizeof(uint32_t)) & DMA_RING_MASK;
    return (uint64_t)wraps * DMA_RING_WORDS + write_index;
}

static uint64_t capture_available_words(void)
{
    const uint64_t words_written = capture_words_written();
    if (words_written < capture_read_words) {
        // Defensive recovery for a capture restart or unexpected DMA reset.
        capture_read_words = words_written & ~1ull;
    }

    uint64_t available = words_written - capture_read_words;
    if (available > DMA_SAFE_BACKLOG_WORDS) {
        uint64_t lost_words = available - DMA_SAFE_BACKLOG_WORDS;
        lost_words = (lost_words + 1u) & ~1ull; // Preserve left/right pairing.
        capture_read_words += lost_words;
        capture_dropped_frames += lost_words / PCM1802_CHANNELS;
        available = words_written - capture_read_words;
    }

    return available & ~1ull;
}

static void usb_discard_pending_tx(void)
{
    tx_packet_bytes = 0;
    tx_packet_offset = 0;
    if (tud_cdc_connected()) {
        tud_cdc_write_clear();
    }
}

static void packet_finalize(uint32_t bytes_without_crc)
{
    const uint32_t crc = crc32_calculate(tx_packet, bytes_without_crc);
    memcpy(&tx_packet[bytes_without_crc], &crc, sizeof(crc));
    tx_packet_bytes = bytes_without_crc + sizeof(crc);
    tx_packet_offset = 0;
}

static void packet_begin(uint8_t type, uint64_t first_frame, uint16_t frame_count, uint32_t payload_bytes)
{
    stream_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, STREAM_MAGIC, sizeof(header.magic));
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.header_bytes = sizeof(header);
    header.session_id = active_session_id;
    header.sequence = packet_sequence++;
    header.sample_rate = PCM1802_SAMPLE_RATE;
    header.first_frame = first_frame;
    header.frame_count = frame_count;
    header.channels = PCM1802_CHANNELS;
    header.bits_per_sample = PCM1802_BITS_PER_SAMPLE;
    header.payload_bytes = payload_bytes;
    header.dropped_frames = capture_dropped_frames;
    memcpy(tx_packet, &header, sizeof(header));
}

static void queue_control_packet(uint8_t type)
{
    packet_begin(type, capture_read_words / PCM1802_CHANNELS, 0, 0);
    packet_finalize(sizeof(stream_header_t));
}

static void pack_sample(uint8_t *destination, uint32_t raw_sample)
{
#if NEOPICO_PCM1802_USB_BITS_PER_SAMPLE == 24
    destination[0] = (uint8_t)(raw_sample & 0xFFu);
    destination[1] = (uint8_t)((raw_sample >> 8u) & 0xFFu);
    destination[2] = (uint8_t)((raw_sample >> 16u) & 0xFFu);
#else
    // Keep the most-significant 16 bits of the signed 24-bit PCM sample.
    destination[0] = (uint8_t)((raw_sample >> 8u) & 0xFFu);
    destination[1] = (uint8_t)((raw_sample >> 16u) & 0xFFu);
#endif
}

static bool queue_audio_packet(void)
{
    if (!capture_running || capture_available_words() < AUDIO_WORDS_PER_PACKET) {
        return false;
    }

    const uint64_t first_frame = capture_read_words / PCM1802_CHANNELS;
    packet_begin(PACKET_AUDIO, first_frame, AUDIO_FRAMES_PER_PACKET, AUDIO_PAYLOAD_BYTES);

    uint8_t *payload = &tx_packet[sizeof(stream_header_t)];
    __dmb(); // Observe completed DMA writes before copying the ring.
    for (uint32_t frame = 0; frame < AUDIO_FRAMES_PER_PACKET; ++frame) {
        const uint64_t word_offset = capture_read_words + (uint64_t)frame * PCM1802_CHANNELS;
        const uint32_t left = dma_ring[(uint32_t)word_offset & DMA_RING_MASK];
        const uint32_t right = dma_ring[((uint32_t)word_offset + 1u) & DMA_RING_MASK];
        pack_sample(&payload[frame * AUDIO_BYTES_PER_FRAME], left);
        pack_sample(&payload[frame * AUDIO_BYTES_PER_FRAME + PCM1802_BYTES_PER_SAMPLE], right);
    }

    capture_read_words += AUDIO_WORDS_PER_PACKET;
    packet_finalize(sizeof(stream_header_t) + AUDIO_PAYLOAD_BYTES);
    return true;
}

static void usb_tx_task(void)
{
    if (tx_packet_bytes == 0 || !tud_cdc_connected()) {
        return;
    }

    const uint32_t available = tud_cdc_write_available();
    if (available == 0) {
        return;
    }

    uint32_t remaining = tx_packet_bytes - tx_packet_offset;
    if (remaining > available) {
        remaining = available;
    }

    const uint32_t written = tud_cdc_write(&tx_packet[tx_packet_offset], remaining);
    tx_packet_offset += written;
    tud_cdc_write_flush();

    if (tx_packet_offset == tx_packet_bytes) {
        tx_packet_bytes = 0;
        tx_packet_offset = 0;
    }
}

static void reset_session(void)
{
    capture_stop();
    session_armed = false;
    streaming = false;
    active_session_id = 0;
    packet_sequence = 0;
    command_buffer_used = 0;
    usb_discard_pending_tx();
}

static void handle_command(const command_packet_t *command)
{
    if (command->command == COMMAND_INFO) {
        capture_stop();
        usb_discard_pending_tx();
        active_session_id = command->session_id;
        packet_sequence = 0;
        session_armed = true;
        streaming = false;
        capture_read_words = 0;
        capture_dropped_frames = 0;
        queue_control_packet(PACKET_HELLO);
        return;
    }

    if (command->command == COMMAND_START) {
        if (!session_armed || command->session_id != active_session_id) {
            capture_stop();
            usb_discard_pending_tx();
            active_session_id = command->session_id;
            packet_sequence = 0;
            session_armed = false;
            streaming = false;
            queue_control_packet(PACKET_ERROR);
            return;
        }

        usb_discard_pending_tx();
        packet_sequence = 0;
        capture_start();
        streaming = true;
        queue_control_packet(PACKET_STARTED);
        return;
    }

    if (command->command == COMMAND_STOP && command->session_id == active_session_id) {
        capture_stop();
        streaming = false;
        usb_discard_pending_tx();
        queue_control_packet(PACKET_STOPPED);
    }
}

static void command_consume_byte(uint8_t byte)
{
    if (command_buffer_used < sizeof(command_buffer)) {
        command_buffer[command_buffer_used++] = byte;
    }

    while (command_buffer_used >= sizeof(COMMAND_MAGIC) - 1u &&
           memcmp(command_buffer, COMMAND_MAGIC, sizeof(COMMAND_MAGIC) - 1u) != 0) {
        memmove(command_buffer, &command_buffer[1], --command_buffer_used);
    }

    if (command_buffer_used != sizeof(command_packet_t)) {
        return;
    }

    command_packet_t command;
    memcpy(&command, command_buffer, sizeof(command));
    command_buffer_used = 0;

    if (command.version != PROTOCOL_VERSION || command.packet_bytes != sizeof(command_packet_t)) {
        return;
    }

    const uint32_t crc = crc32_calculate((const uint8_t *)&command, offsetof(command_packet_t, crc32));
    if (crc != command.crc32) {
        return;
    }

    handle_command(&command);
}

static void usb_rx_task(void)
{
    uint8_t bytes[64];
    while (tud_cdc_available() > 0) {
        const uint32_t count = tud_cdc_read(bytes, sizeof(bytes));
        for (uint32_t i = 0; i < count; ++i) {
            command_consume_byte(bytes[i]);
        }
    }
}

int main(void)
{
    board_init();
    crc32_init();
    capture_init();

    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    bool was_connected = false;
    while (true) {
        tud_task();

        const bool connected = tud_cdc_connected();
        if (!connected && was_connected) {
            reset_session();
        }
        was_connected = connected;

        if (!connected) {
            continue;
        }

        usb_rx_task();
        usb_tx_task();

        if (streaming) {
            // Update the overrun guard even while USB still owns a pending
            // packet. Any loss becomes visible in the next packet's counters
            // and first-frame index.
            (void)capture_available_words();
            if (tx_packet_bytes == 0) {
                (void)queue_audio_packet();
            }
        }
    }
}

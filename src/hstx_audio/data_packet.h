/**
 * HDMI Data Packet Encoding for HSTX
 *
 * Adapted from PicoDVI for use with RP2350 HSTX peripheral.
 * Encodes HDMI data packets (audio, InfoFrames) using TERC4 encoding.
 *
 * Key difference from PicoDVI:
 * - HSTX uses 1 symbol per 32-bit word (30 bits: 3 lanes x 10 bits)
 * - PicoDVI uses 2 symbols per 32-bit word (20 bits per lane)
 */

#ifndef HSTX_DATA_PACKET_H
#define HSTX_DATA_PACKET_H

#include <stdint.h>
#include <stdbool.h>

// Data Island timing constants
#define W_GUARDBAND     2    // Guard band: 2 pixel clocks
#define W_PREAMBLE      8    // Preamble: 8 pixel clocks
#define W_DATA_PACKET   32   // Packet data: 32 pixel clocks
#define W_DATA_ISLAND   (W_GUARDBAND + W_DATA_PACKET + W_GUARDBAND)  // Total: 36

// HSTX outputs 1 symbol per word
#define HSTX_DATA_ISLAND_WORDS  W_DATA_ISLAND  // 36 words for HSTX

// Packet structure (same as HDMI spec)
typedef struct {
    uint8_t header[4];        // 3 bytes header + 1 byte BCH parity
    uint8_t subpacket[4][8];  // 4 subpackets, each 7 bytes + 1 byte BCH parity
} data_packet_t;

// Pre-encoded data island for HSTX (36 words)
typedef struct {
    uint32_t words[HSTX_DATA_ISLAND_WORDS];
} hstx_data_island_t;

// Audio sample ring buffer
typedef struct {
    int16_t left;
    int16_t right;
} audio_sample_t;

// ============================================================================
// Packet creation functions
// ============================================================================

/**
 * Initialize a packet to all zeros
 */
void packet_init(data_packet_t *packet);

/**
 * Set up Audio Clock Regeneration (ACR) packet
 * @param packet Output packet
 * @param n N value (e.g., 6144 for 48kHz)
 * @param cts CTS value (cycle time stamp)
 */
void packet_set_acr(data_packet_t *packet, uint32_t n, uint32_t cts);

/**
 * Set up Audio InfoFrame packet
 * @param packet Output packet
 * @param sample_rate Sample rate in Hz (32000, 44100, 48000)
 * @param channels Number of channels (2 for stereo)
 * @param bits_per_sample Bits per sample (16, 20, 24)
 */
void packet_set_audio_infoframe(data_packet_t *packet, uint32_t sample_rate,
                                 uint8_t channels, uint8_t bits_per_sample);

/**
 * Set up AVI (Auxiliary Video Information) InfoFrame
 * @param packet Output packet
 * @param vic Video Identification Code (1 = 640x480 @ 60Hz)
 */
void packet_set_avi_infoframe(data_packet_t *packet, uint8_t vic);

/**
 * Set up Audio Sample packet
 * @param packet Output packet
 * @param samples Array of audio samples (up to 4)
 * @param num_samples Number of samples (1-4)
 * @param frame_count Current audio frame counter (0-191, for channel status)
 * @return Updated frame count
 */
int packet_set_audio_samples(data_packet_t *packet, const audio_sample_t *samples,
                              int num_samples, int frame_count);

/**
 * Set up Null packet (no data, used for padding)
 */
void packet_set_null(data_packet_t *packet);

// ============================================================================
// TERC4 encoding for HSTX
// ============================================================================

/**
 * Encode a data packet to HSTX format
 * @param out Output buffer (36 words)
 * @param packet Input packet
 * @param vsync Current vsync state
 * @param hsync Current hsync state
 */
void hstx_encode_data_island(hstx_data_island_t *out, const data_packet_t *packet,
                              bool vsync, bool hsync);

/**
 * Get pre-encoded null data island for given sync state
 * Useful for padding when no audio data is available
 */
const uint32_t* hstx_get_null_data_island(bool vsync, bool hsync);

/**
 * Debug function to trace bit shuffle and nibble extraction
 */
void debug_encode_byte(uint8_t byte_val);

/**
 * Debug function to decode and print a data island
 */
void debug_print_data_island(const hstx_data_island_t *island);

/**
 * Debug function to dump all 36 words of a data island
 */
void debug_dump_data_island(const hstx_data_island_t *island);

/**
 * Debug function to compare our encoding with hsdaoh-style
 */
void debug_hsdaoh_acr_comparison(uint32_t n, uint32_t cts);

// ============================================================================
// Preamble and guard band symbols
// ============================================================================

// Data Island preamble (8 clocks before guard band)
// Lane 0: CTRL with hsync/vsync, Lanes 1&2: 01 pattern
#define PREAMBLE_DATA_ISLAND_V0H0  (0x354u | (0x0abu << 10) | (0x0abu << 20))
#define PREAMBLE_DATA_ISLAND_V0H1  (0x0abu | (0x0abu << 10) | (0x0abu << 20))
#define PREAMBLE_DATA_ISLAND_V1H0  (0x154u | (0x0abu << 10) | (0x0abu << 20))
#define PREAMBLE_DATA_ISLAND_V1H1  (0x2abu | (0x0abu << 10) | (0x0abu << 20))

// Video preamble (different pattern)
#define PREAMBLE_VIDEO_V1H1        (0x2abu | (0x0abu << 10) | (0x354u << 20))

// Video leading guard band
#define VIDEO_GUARD_BAND           (0x2ccu | (0x133u << 10) | (0x2ccu << 20))

#endif // HSTX_DATA_PACKET_H

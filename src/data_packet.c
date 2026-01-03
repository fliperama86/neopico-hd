/**
 * HDMI Data Packet Encoding for HSTX
 *
 * Adapted from PicoDVI/hsdaoh for RP2350 HSTX peripheral.
 *
 * References:
 * - HDMI 1.4 Specification (Section 5.2 - Data Island)
 * - PicoDVI: github.com/Wren6991/PicoDVI
 * - hsdaoh-rp2350: github.com/steve-m/hsdaoh-rp2350
 */

#include "data_packet.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// TERC4 Symbol Table (4-bit to 10-bit encoding)
// ============================================================================

static const uint16_t TERC4[16] = {
    0b1010011100, // 0
    0b1001100011, // 1
    0b1011100100, // 2
    0b1011100010, // 3
    0b0101110001, // 4
    0b0100011110, // 5
    0b0110001110, // 6
    0b0100111100, // 7
    0b1011001100, // 8
    0b0100111001, // 9
    0b0110011100, // 10
    0b1011000110, // 11
    0b1010001110, // 12
    0b1001110001, // 13
    0b0101100011, // 14
    0b1011000011, // 15
};

// Data Island guard band symbol (same on lanes 1 & 2)
#define GUARD_BAND_SYMBOL 0x133u // 0b0100110011

// ============================================================================
// BCH Encoding (for packet error correction)
// ============================================================================

static const uint8_t bch_table[256] = {
    0x00, 0xd9, 0xb5, 0x6c, 0x6d, 0xb4, 0xd8, 0x01, 0xda, 0x03, 0x6f, 0xb6,
    0xb7, 0x6e, 0x02, 0xdb, 0xb3, 0x6a, 0x06, 0xdf, 0xde, 0x07, 0x6b, 0xb2,
    0x69, 0xb0, 0xdc, 0x05, 0x04, 0xdd, 0xb1, 0x68, 0x61, 0xb8, 0xd4, 0x0d,
    0x0c, 0xd5, 0xb9, 0x60, 0xbb, 0x62, 0x0e, 0xd7, 0xd6, 0x0f, 0x63, 0xba,
    0xd2, 0x0b, 0x67, 0xbe, 0xbf, 0x66, 0x0a, 0xd3, 0x08, 0xd1, 0xbd, 0x64,
    0x65, 0xbc, 0xd0, 0x09, 0xc2, 0x1b, 0x77, 0xae, 0xaf, 0x76, 0x1a, 0xc3,
    0x18, 0xc1, 0xad, 0x74, 0x75, 0xac, 0xc0, 0x19, 0x71, 0xa8, 0xc4, 0x1d,
    0x1c, 0xc5, 0xa9, 0x70, 0xab, 0x72, 0x1e, 0xc7, 0xc6, 0x1f, 0x73, 0xaa,
    0xa3, 0x7a, 0x16, 0xcf, 0xce, 0x17, 0x7b, 0xa2, 0x79, 0xa0, 0xcc, 0x15,
    0x14, 0xcd, 0xa1, 0x78, 0x10, 0xc9, 0xa5, 0x7c, 0x7d, 0xa4, 0xc8, 0x11,
    0xca, 0x13, 0x7f, 0xa6, 0xa7, 0x7e, 0x12, 0xcb, 0x83, 0x5a, 0x36, 0xef,
    0xee, 0x37, 0x5b, 0x82, 0x59, 0x80, 0xec, 0x35, 0x34, 0xed, 0x81, 0x58,
    0x30, 0xe9, 0x85, 0x5c, 0x5d, 0x84, 0xe8, 0x31, 0xea, 0x33, 0x5f, 0x86,
    0x87, 0x5e, 0x32, 0xeb, 0xe2, 0x3b, 0x57, 0x8e, 0x8f, 0x56, 0x3a, 0xe3,
    0x38, 0xe1, 0x8d, 0x54, 0x55, 0x8c, 0xe0, 0x39, 0x51, 0x88, 0xe4, 0x3d,
    0x3c, 0xe5, 0x89, 0x50, 0x8b, 0x52, 0x3e, 0xe7, 0xe6, 0x3f, 0x53, 0x8a,
    0x41, 0x98, 0xf4, 0x2d, 0x2c, 0xf5, 0x99, 0x40, 0x9b, 0x42, 0x2e, 0xf7,
    0xf6, 0x2f, 0x43, 0x9a, 0xf2, 0x2b, 0x47, 0x9e, 0x9f, 0x46, 0x2a, 0xf3,
    0x28, 0xf1, 0x9d, 0x44, 0x45, 0x9c, 0xf0, 0x29, 0x20, 0xf9, 0x95, 0x4c,
    0x4d, 0x94, 0xf8, 0x21, 0xfa, 0x23, 0x4f, 0x96, 0x97, 0x4e, 0x22, 0xfb,
    0x93, 0x4a, 0x26, 0xff, 0xfe, 0x27, 0x4b, 0x92, 0x49, 0x90, 0xfc, 0x25,
    0x24, 0xfd, 0x91, 0x48,
};

// Parity table for audio sample validity
static const uint8_t parity_table[32] = {
    0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69, 0x69, 0x96, 0x96,
    0x69, 0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69, 0x96, 0x69,
    0x69, 0x96, 0x96, 0x69, 0x69, 0x96, 0x69, 0x96, 0x96, 0x69};

static inline bool compute_parity(uint8_t v) {
  return (parity_table[v / 8] >> (v % 8)) & 1;
}

static inline bool compute_parity3(uint8_t a, uint8_t b, uint8_t c) {
  return compute_parity(a) ^ compute_parity(b) ^ compute_parity(c);
}

static uint8_t encode_bch_3(const uint8_t *p) {
  uint8_t v = bch_table[p[0]];
  v = bch_table[p[1] ^ v];
  v = bch_table[p[2] ^ v];
  return v;
}

static uint8_t encode_bch_7(const uint8_t *p) {
  uint8_t v = bch_table[p[0]];
  v = bch_table[p[1] ^ v];
  v = bch_table[p[2] ^ v];
  v = bch_table[p[3] ^ v];
  v = bch_table[p[4] ^ v];
  v = bch_table[p[5] ^ v];
  v = bch_table[p[6] ^ v];
  return v;
}

static void compute_header_parity(data_packet_t *p) {
  p->header[3] = encode_bch_3(p->header);
}

static void compute_subpacket_parity(data_packet_t *p, int idx) {
  p->subpacket[idx][7] = encode_bch_7(p->subpacket[idx]);
}

static void compute_all_parity(data_packet_t *p) {
  compute_header_parity(p);
  for (int i = 0; i < 4; i++) {
    compute_subpacket_parity(p, i);
  }
}

// InfoFrame checksum (different from BCH)
static void compute_infoframe_checksum(data_packet_t *p) {
  int sum = 0;
  // Sum header bytes
  for (int i = 0; i < 3; i++) {
    sum += p->header[i];
  }
  // Sum data bytes (length is in header[2])
  int len = p->header[2] + 1;
  for (int j = 0; j < 4 && len > 0; j++) {
    for (int i = 0; i < 7 && len > 0; i++, len--) {
      sum += p->subpacket[j][i];
    }
  }
  // Checksum goes in first byte of first subpacket
  p->subpacket[0][0] = (uint8_t)(-sum);
}

// ============================================================================
// Packet Creation Functions
// ============================================================================

void packet_init(data_packet_t *packet) {
  memset(packet, 0, sizeof(data_packet_t));
}

void packet_set_null(data_packet_t *packet) {
  packet_init(packet);
  // Null packet: type 0, all zeros
  compute_all_parity(packet);
}

void packet_set_acr(data_packet_t *packet, uint32_t n, uint32_t cts) {
  packet_init(packet);

  // Header: packet type 0x01 (Audio Clock Regeneration)
  packet->header[0] = 0x01;
  packet->header[1] = 0x00;
  packet->header[2] = 0x00;
  compute_header_parity(packet);

  // Subpacket format per HDMI 1.4 spec Table 5-10 (BIG ENDIAN byte order!):
  // SB0: Reserved
  // SB1: CTS[19:16] (high nibble)
  // SB2: CTS[15:8]  (middle byte)
  // SB3: CTS[7:0]   (low byte)
  // SB4: N[19:16]   (high nibble)
  // SB5: N[15:8]    (middle byte)
  // SB6: N[7:0]     (low byte)
  packet->subpacket[0][0] = 0;                  // SB0: Reserved
  packet->subpacket[0][1] = (cts >> 16) & 0x0F; // SB1: CTS[19:16]
  packet->subpacket[0][2] = (cts >> 8) & 0xFF;  // SB2: CTS[15:8]
  packet->subpacket[0][3] = cts & 0xFF;         // SB3: CTS[7:0]
  packet->subpacket[0][4] = (n >> 16) & 0x0F;   // SB4: N[19:16]
  packet->subpacket[0][5] = (n >> 8) & 0xFF;    // SB5: N[15:8]
  packet->subpacket[0][6] = n & 0xFF;           // SB6: N[7:0]
  compute_subpacket_parity(packet, 0);

  // Copy to other subpackets (ACR repeats same data 4 times)
  memcpy(packet->subpacket[1], packet->subpacket[0], 8);
  memcpy(packet->subpacket[2], packet->subpacket[0], 8);
  memcpy(packet->subpacket[3], packet->subpacket[0], 8);
}

void packet_set_audio_infoframe(data_packet_t *packet, uint32_t sample_rate,
                                uint8_t channels, uint8_t bits_per_sample) {
  packet_init(packet);

  // Header: Audio InfoFrame (type 0x84)
  packet->header[0] = 0x84;
  packet->header[1] = 0x01; // Version 1
  packet->header[2] = 0x0A; // Length = 10

  // Coding type and channel count
  uint8_t cc = (channels - 1) & 0x07; // CC: channel count - 1
  uint8_t ct = 0x01;                  // CT: PCM

  // Sample size
  uint8_t ss;
  switch (bits_per_sample) {
  case 16:
    ss = 0x01;
    break;
  case 20:
    ss = 0x02;
    break;
  case 24:
    ss = 0x03;
    break;
  default:
    ss = 0x00;
    break; // Refer to stream header
  }

  // Sample frequency
  uint8_t sf;
  switch (sample_rate) {
  case 32000:
    sf = 0x01;
    break;
  case 44100:
    sf = 0x02;
    break;
  case 48000:
    sf = 0x03;
    break;
  case 88200:
    sf = 0x04;
    break;
  case 96000:
    sf = 0x05;
    break;
  case 176400:
    sf = 0x06;
    break;
  case 192000:
    sf = 0x07;
    break;
  default:
    sf = 0x00;
    break; // Refer to stream header
  }

  // Data bytes (in subpackets, byte 0 is checksum)
  packet->subpacket[0][1] = cc | (ct << 4); // CC + CT
  packet->subpacket[0][2] = ss | (sf << 2); // SS + SF
  packet->subpacket[0][3] = 0x00;           // Format
  packet->subpacket[0][4] = 0x00;           // CA (channel allocation) - FR/FL
  packet->subpacket[0][5] = 0x00;           // LSV=0, DM_INH=0

  compute_infoframe_checksum(packet);
  compute_all_parity(packet);
}

void packet_set_avi_infoframe(data_packet_t *packet, uint8_t vic) {
  packet_init(packet);

  // Header: AVI InfoFrame (type 0x82)
  packet->header[0] = 0x82;
  packet->header[1] = 0x02; // Version 2
  packet->header[2] = 0x0D; // Length = 13

  // Data bytes (simplified for 640x480 RGB)
  // Y1Y0 = 00 (RGB), A0 = 0 (no active format), B1B0 = 00 (no bar info)
  // S1S0 = 00 (no scan info), C1C0 = 00 (no colorimetry)
  // M1M0 = 00 (no aspect ratio), R3-R0 = 1000 (same as picture)
  packet->subpacket[0][1] = 0x00; // Y=RGB, A0=0, B=0, S=0
  packet->subpacket[0][2] = 0x08; // C=0, M=0, R=8 (same as coded frame)
  packet->subpacket[0][3] = 0x00; // ITC=0, EC=0, Q=0 (default range), SC=0
  packet->subpacket[0][4] = vic;  // VIC (Video Identification Code)
  packet->subpacket[0][5] = 0x00; // YQ=0, CN=0, PR=0

  compute_infoframe_checksum(packet);
  compute_all_parity(packet);
}

// Get Channel Status bit for a given frame in the 192-frame block
// Channel status: Simplified to match PicoDVI (constant, no dynamic
// computation)
static bool get_channel_status(int frame_idx) {
  // PicoDVI uses constant vuc=1 (all C-bits = 0) and it works
  // Ignore frame_idx and always return 0 for consistency
  (void)frame_idx;
  return 0;
}

int packet_set_audio_samples(data_packet_t *packet,
                             const audio_sample_t *samples, int num_samples,
                             int frame_count) {
  packet_init(packet);

  // Clamp num_samples to 1-4
  if (num_samples < 1)
    num_samples = 1;
  if (num_samples > 4)
    num_samples = 4;

  // Header: Audio Sample Packet (type 0x02)
  // Layout 0 (2-channel), sample present flags
  uint8_t sample_present = (1 << num_samples) - 1;

  // B bits indicate start of IEC60958 192-sample block
  uint8_t b_flags = 0;
  uint8_t c_bits = 0;

  int temp_frame_count = frame_count;
  for (int i = 0; i < num_samples; i++) {
    if (temp_frame_count == 0) {
      b_flags |= (1 << i);
    }
    if (get_channel_status(temp_frame_count)) {
      c_bits |= (1 << i);
    }
    temp_frame_count = (temp_frame_count + 1) % 192;
  }

  packet->header[0] = 0x02;
  packet->header[1] = sample_present; // SP flags + layout
  packet->header[2] = b_flags << 4;   // B flags
  compute_header_parity(packet);

  // Fill subpackets with audio samples
  for (int i = 0; i < num_samples; i++) {
    uint8_t *d = packet->subpacket[i];
    int16_t left = samples[i].left;
    int16_t right = samples[i].right;

    d[0] = 0x00;
    d[1] = left & 0xFF;
    d[2] = (left >> 8) & 0xFF;
    d[3] = 0x00;
    d[4] = right & 0xFF;
    d[5] = (right >> 8) & 0xFF;

    // Validity, User, Channel Status, Parity bits
    // Validity bit (V): 0 = Valid, 1 = Invalid.
    // User bit (U): 0
    // Channel status bit (C): 0
    // Parity bit (P): Calculated
    const uint8_t vuc_left = 0;  // 0 = Valid
    const uint8_t vuc_right = 0; // 0 = Valid

    bool p_left = compute_parity3(d[1], d[2], vuc_left);
    bool p_right = compute_parity3(d[4], d[5], vuc_right);

    d[6] = (vuc_left) | (p_left << 3) | (vuc_right << 4) | (p_right << 7);
    compute_subpacket_parity(packet, i);
  }

  // Zero remaining subpackets
  for (int i = num_samples; i < 4; i++) {
    memset(packet->subpacket[i], 0, 8);
  }

  return temp_frame_count;
}

// ============================================================================
// TERC4 Encoding for HSTX
// ============================================================================

// Create HSTX word from 3 lane symbols
static inline uint32_t make_hstx_word(uint16_t lane0, uint16_t lane1,
                                      uint16_t lane2) {
  return (lane0 & 0x3FF) | ((lane1 & 0x3FF) << 10) | ((lane2 & 0x3FF) << 20);
}

// Encode header bits to lane 0 TERC4 symbols (includes hsync/vsync)
static void encode_header_to_lane0(const data_packet_t *packet, uint16_t *lane0,
                                   int hv, bool first_packet) {
  // HDMI Spec: Lane 0 TERC4 D[3] is the Data Island flag.
  // It is 1 for all symbols in the Data Island Period EXCEPT the very first
  // symbol of the first packet's header (per some implementations/receivers).
  int hv1 = hv | 0x08;
  if (!first_packet) {
    hv = hv1;
  }

  int idx = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t h = packet->header[i];
    // 8 bits -> 8 TERC4 symbols (1 bit per symbol in bit 2)
    lane0[idx++] = TERC4[((h << 2) & 4) | hv];
    hv = hv1;
    lane0[idx++] = TERC4[((h << 1) & 4) | hv];
    lane0[idx++] = TERC4[(h & 4) | hv];
    lane0[idx++] = TERC4[((h >> 1) & 4) | hv];
    lane0[idx++] = TERC4[((h >> 2) & 4) | hv];
    lane0[idx++] = TERC4[((h >> 3) & 4) | hv];
    lane0[idx++] = TERC4[((h >> 4) & 4) | hv];
    lane0[idx++] = TERC4[((h >> 5) & 4) | hv];
  }
}

// Encode subpackets to lanes 1 and 2
static void encode_subpackets_to_lanes(const data_packet_t *packet,
                                       uint16_t *lane1, uint16_t *lane2) {
  for (int i = 0; i < 8; i++) {
    // Interleave subpacket bytes across lanes
    uint32_t v =
        (packet->subpacket[0][i] << 0) | (packet->subpacket[1][i] << 8) |
        (packet->subpacket[2][i] << 16) | (packet->subpacket[3][i] << 24);

    // Bit shuffle for TERC4 encoding
    uint32_t t = (v ^ (v >> 7)) & 0x00aa00aa;
    v = v ^ t ^ (t << 7);
    t = (v ^ (v >> 14)) & 0x0000cccc;
    v = v ^ t ^ (t << 14);

    // Extract nibbles for TERC4 encoding
    // hsdaoh packs 2 symbols per word: makeTERC4x2Char_2(a, b) = TERC4[a] |
    // (TERC4[b] << 10) Then HSTX conversion unpacks: first output = lower 10
    // bits, second = upper 10 bits So their symbol order per byte is: (v>>0),
    // (v>>16), (v>>4), (v>>20) And for lane 2: (v>>8), (v>>24), (v>>12),
    // (v>>28)
    lane1[i * 4 + 0] = TERC4[(v >> 0) & 0xF];
    lane1[i * 4 + 1] = TERC4[(v >> 16) & 0xF];
    lane1[i * 4 + 2] = TERC4[(v >> 4) & 0xF];
    lane1[i * 4 + 3] = TERC4[(v >> 20) & 0xF];

    lane2[i * 4 + 0] = TERC4[(v >> 8) & 0xF];
    lane2[i * 4 + 1] = TERC4[(v >> 24) & 0xF];
    lane2[i * 4 + 2] = TERC4[(v >> 12) & 0xF];
    lane2[i * 4 + 3] = TERC4[(v >> 28) & 0xF];
  }
}

// Reverse TERC4 lookup for debugging
static int terc4_to_nibble(uint16_t symbol) {
  for (int i = 0; i < 16; i++) {
    if (TERC4[i] == symbol)
      return i;
  }
  return -1; // Invalid symbol
}

// Debug function to verify encoding
void debug_encode_byte(uint8_t byte_val) {
  // Simulate encoding a single byte replicated across 4 subpackets
  uint32_t v = byte_val | (byte_val << 8) | (byte_val << 16) | (byte_val << 24);

  // Bit shuffle
  uint32_t t = (v ^ (v >> 7)) & 0x00aa00aa;
  v = v ^ t ^ (t << 7);
  t = (v ^ (v >> 14)) & 0x0000cccc;
  v = v ^ t ^ (t << 14);

  printf("Byte 0x%02x -> shuffled 0x%08lx\n", byte_val, v);
  printf("  Lane1 nibbles (0,16,4,20): %lx %lx %lx %lx\n", (v >> 0) & 0xF,
         (v >> 16) & 0xF, (v >> 4) & 0xF, (v >> 20) & 0xF);
  printf("  Lane2 nibbles (8,24,12,28): %lx %lx %lx %lx\n", (v >> 8) & 0xF,
         (v >> 24) & 0xF, (v >> 12) & 0xF, (v >> 28) & 0xF);
}

// Decode and print a data island for verification
void debug_print_data_island(const hstx_data_island_t *island) {
  printf("Data island words (guard, packet, guard):\n");
  printf("  Guard[0-1]: %08lx %08lx\n", island->words[0], island->words[1]);

  // Decode packet symbols
  printf("  Packet byte layout:\n");
  for (int byte_idx = 0; byte_idx < 8; byte_idx++) {
    int base_symbol = byte_idx * 4;
    int base_word = base_symbol + 2; // Skip guard band

    // Extract lane1 and lane2 nibbles from 4 consecutive words
    uint8_t lane1_nibbles[4], lane2_nibbles[4];
    for (int i = 0; i < 4; i++) {
      uint32_t w = island->words[base_word + i];
      uint16_t l1 = (w >> 10) & 0x3FF;
      uint16_t l2 = (w >> 20) & 0x3FF;
      lane1_nibbles[i] = terc4_to_nibble(l1);
      lane2_nibbles[i] = terc4_to_nibble(l2);
    }

    printf("    Byte %d (words %d-%d): L1=[%x,%x,%x,%x] L2=[%x,%x,%x,%x]\n",
           byte_idx, base_word, base_word + 3, lane1_nibbles[0],
           lane1_nibbles[1], lane1_nibbles[2], lane1_nibbles[3],
           lane2_nibbles[0], lane2_nibbles[1], lane2_nibbles[2],
           lane2_nibbles[3]);
  }
  printf("  Guard[34-35]: %08lx %08lx\n", island->words[34], island->words[35]);
}

// Print all 36 words of a data island in hex
void debug_dump_data_island(const hstx_data_island_t *island) {
  printf("Full data island dump (36 words):\n");
  for (int i = 0; i < 36; i += 4) {
    printf("  [%2d-%2d]: %08lx %08lx %08lx %08lx\n", i, i + 3, island->words[i],
           island->words[i + 1], island->words[i + 2], island->words[i + 3]);
  }
}

// Simulate what hsdaoh would produce for an ACR packet
void debug_hsdaoh_acr_comparison(uint32_t n, uint32_t cts) {
  printf("\nComparing with hsdaoh-style encoding:\n");

  // hsdaoh packs 2 symbols per word, then converts to HSTX (1 per word)
  // For ACR subpacket byte 1 (CTS[7:0] = 0x57):
  uint8_t cts_byte0 = cts & 0xFF;         // 0x57
  uint8_t cts_byte1 = (cts >> 8) & 0xFF;  // 0x62
  uint8_t cts_byte2 = (cts >> 16) & 0x0F; // 0x00

  printf("CTS bytes: [%02x, %02x, %02x]\n", cts_byte0, cts_byte1, cts_byte2);

  // For each byte, show the bit shuffle result
  // Byte 1 (SB1 = CTS[7:0]):
  uint32_t v =
      cts_byte0 | (cts_byte0 << 8) | (cts_byte0 << 16) | (cts_byte0 << 24);
  uint32_t t = (v ^ (v >> 7)) & 0x00aa00aa;
  v = v ^ t ^ (t << 7);
  t = (v ^ (v >> 14)) & 0x0000cccc;
  v = v ^ t ^ (t << 14);

  printf("CTS[7:0]=0x%02x after shuffle: 0x%08lx\n", cts_byte0, v);

  // hsdaoh packing: makeTERC4x2Char_2((v>>0)&15, (v>>16)&15)
  uint16_t sym0 = TERC4[(v >> 0) & 0xF];
  uint16_t sym1 = TERC4[(v >> 16) & 0xF];
  uint32_t packed01 = sym0 | (sym1 << 10);
  printf("  hsdaoh packed[0,1] = TERC4[%lx] | TERC4[%lx]<<10 = 0x%05lx\n",
         (v >> 0) & 0xF, (v >> 16) & 0xF, packed01);

  uint16_t sym2 = TERC4[(v >> 4) & 0xF];
  uint16_t sym3 = TERC4[(v >> 20) & 0xF];
  uint32_t packed23 = sym2 | (sym3 << 10);
  printf("  hsdaoh packed[2,3] = TERC4[%lx] | TERC4[%lx]<<10 = 0x%05lx\n",
         (v >> 4) & 0xF, (v >> 20) & 0xF, packed23);

  // After HSTX conversion (unpack to 1 symbol per word)
  printf("  HSTX words (lane1 only): [%03x, %03x, %03x, %03x]\n",
         packed01 & 0x3FF, packed01 >> 10, packed23 & 0x3FF, packed23 >> 10);
}

void hstx_encode_data_island(hstx_data_island_t *out,
                             const data_packet_t *packet, bool vsync_active,
                             bool hsync_active) {
  // HDMI Sync bits are active LOW (0 = Active, 1 = Idle)
  // TERC4 Lane 0 input: bit 0 = H, bit 1 = V
  int hv = (vsync_active ? 0 : 2) | (hsync_active ? 0 : 1);

  // Temporary buffers for lane symbols (32 symbols per lane)
  uint16_t lane0[32];
  uint16_t lane1[32];
  uint16_t lane2[32];

  // Encode packet to lane symbols
  encode_header_to_lane0(packet, lane0, hv, true);
  encode_subpackets_to_lanes(packet, lane1, lane2);

  // Guard band symbol for lane 0
  uint16_t gb_lane0 = TERC4[0xC | hv]; // 0b1100 | hv
  uint32_t guard_word =
      make_hstx_word(gb_lane0, GUARD_BAND_SYMBOL, GUARD_BAND_SYMBOL);

  // Data island structure (36 pixel clocks = 36 HSTX words):
  // Words 0-1: Leading guard band (2 clocks)
  out->words[0] = guard_word;
  out->words[1] = guard_word;

  // Words 2-33: Packet data (32 clocks)
  for (int i = 0; i < 32; i++) {
    out->words[i + 2] = make_hstx_word(lane0[i], lane1[i], lane2[i]);
  }

  // Words 34-35: Trailing guard band (2 clocks)
  out->words[34] = guard_word;
  out->words[35] = guard_word;
}

// Pre-computed null data islands for each sync state
static hstx_data_island_t null_islands[4];
static bool null_islands_initialized = false;

static void init_null_islands(void) {
  if (null_islands_initialized)
    return;

  data_packet_t null_packet;
  packet_set_null(&null_packet);

  for (int vsync = 0; vsync < 2; vsync++) {
    for (int hsync = 0; hsync < 2; hsync++) {
      hstx_encode_data_island(&null_islands[vsync * 2 + hsync], &null_packet,
                              vsync, hsync);
    }
  }
  null_islands_initialized = true;
}

const uint32_t *hstx_get_null_data_island(bool vsync, bool hsync) {
  init_null_islands();
  return null_islands[(vsync ? 2 : 0) | (hsync ? 1 : 0)].words;
}

/**
 * HSTX Audio Test - Video + HDMI Audio via Data Islands
 *
 * Based on pico-examples/hstx/dvi_out_hstx_encoder with audio additions.
 * Outputs 640x480 @ 60Hz with HDMI audio (48kHz stereo).
 *
 * HSTX Pin Assignment (GPIO 12-19) - Custom Spotpear wiring:
 *     GPIO 12: CLKN    GPIO 13: CLKP
 *     GPIO 14: D0N     GPIO 15: D0P  (Lane 0 - Blue)
 *     GPIO 16: D1N     GPIO 17: D1P  (Lane 1 - Green)
 *     GPIO 18: D2N     GPIO 19: D2P  (Lane 2 - Red)
 *
 * Audio implementation:
 * - Data Islands inserted during hsync pulse in vblank
 * - ACR packet sent once per frame
 * - Audio InfoFrame sent once per frame
 *
 * Target: RP2350A (Pico 2)
 */

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/stdlib.h"
#include "data_packet.h"
#include <stdio.h>
#include <math.h>

// ============================================================================
// DVI/HDMI Constants
// ============================================================================

#define TMDS_CTRL_00 0x354u  // vsync=0 hsync=0
#define TMDS_CTRL_01 0x0abu  // vsync=0 hsync=1
#define TMDS_CTRL_10 0x154u  // vsync=1 hsync=0
#define TMDS_CTRL_11 0x2abu  // vsync=1 hsync=1

// Sync symbols: Lane 0 carries sync, Lanes 1&2 are always CTRL_00
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// Data Island preamble: Lane 0 = sync, Lanes 1&2 = CTRL_01 pattern
// Used during hsync pulse (hsync=0), so use H0 variants
#define PREAMBLE_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

// 480p timing
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_H_TOTAL_PIXELS (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
                             MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES  (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
                             MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// HSTX commands
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ============================================================================
// Audio Configuration
// ============================================================================

// Try 32kHz (lowest standard rate) - needs 533 samples/frame, we send 168 (~31%)
// This is still undersampled but much closer than 48kHz (which needs 800)
#define AUDIO_SAMPLE_RATE  32000
#define AUDIO_N_VALUE      4096   // Standard N for 32kHz
#define AUDIO_CTS_VALUE    30000  // CTS for 30MHz pixel clock

// Audio generation
#define TONE_FREQUENCY     440    // A4 note (440 Hz)
#define TONE_AMPLITUDE     8000   // ~25% of int16 max

// Pre-computed sine table for fast audio generation in interrupt
#define SINE_TABLE_SIZE    256
static int16_t sine_table[SINE_TABLE_SIZE];

// Debug: Use a distinctive CTS pattern to trace encoding issues
// Set to 1 to use test CTS values for debugging
// Test values:
//   0x06257 = normal (25175) - bytes: 57, 62, 00 - FAILS (57→00, 62→OK, 00→07)
//   0x62626 = all 0x62       - bytes: 26, 62, 06 - TEST: does 0x62 pattern work everywhere?
//   0x55555 = alternating    - bytes: 55, 55, 05 - TEST: different pattern
//   0x12345 = sequential     - bytes: 45, 23, 01 - TEST: unique bytes
#define DEBUG_USE_TEST_CTS  0
#define DEBUG_TEST_CTS      0x12345  // Try sequential pattern first

// Audio samples per frame (48kHz / 60fps = 800 samples)
// We send audio on every line (525 lines) using fractional accumulation
// samples_per_line = 800/525 ≈ 1.52, so we send 1-2 samples per line
#define SAMPLES_PER_FRAME  (AUDIO_SAMPLE_RATE / 60)  // 800
#define LINES_PER_FRAME    (MODE_V_TOTAL_LINES)       // 525

// Fixed-point accumulator for fractional samples per line (16.16 format)
#define SAMPLES_PER_LINE_FP  ((SAMPLES_PER_FRAME << 16) / LINES_PER_FRAME)  // ~99614 = 1.52 in 16.16

// Set to 1 to enable audio during active video lines (experimental)
// Set to 0 to use vblank-only audio (stable, but undersampled)
#define ENABLE_ACTIVE_LINE_AUDIO  0

// Audio packets per vblank:
// Front porch: lines 1-9 = 9 packets (line 0 is AVI InfoFrame)
// Back porch: lines 12-44 = 33 packets
// Total: 42 packets × 4 samples = 168 samples/frame (still only 21% of 800 needed)
#define AUDIO_PACKETS_PER_FRAME  42

// ============================================================================
// Framebuffer
// ============================================================================

static uint8_t framebuf[MODE_V_ACTIVE_LINES * MODE_H_ACTIVE_PIXELS];

static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xe0) >> 0) | ((g & 0xe0) >> 3) | ((b & 0xc0) >> 6);
}

// Bouncing box state
#define BOX_SIZE 64
static int box_x = 100, box_y = 100;
static int box_dx = 3, box_dy = 2;

static void draw_frame(void) {
    uint8_t bg = rgb332(0, 0, 64);
    uint8_t box = rgb332(255, 255, 0);

    for (int y = 0; y < MODE_V_ACTIVE_LINES; y++) {
        for (int x = 0; x < MODE_H_ACTIVE_PIXELS; x++) {
            if (x >= box_x && x < box_x + BOX_SIZE &&
                y >= box_y && y < box_y + BOX_SIZE) {
                framebuf[y * MODE_H_ACTIVE_PIXELS + x] = box;
            } else {
                framebuf[y * MODE_H_ACTIVE_PIXELS + x] = bg;
            }
        }
    }
}

static void update_box(void) {
    box_x += box_dx;
    box_y += box_dy;

    if (box_x <= 0 || box_x + BOX_SIZE >= MODE_H_ACTIVE_PIXELS) {
        box_dx = -box_dx;
        box_x += box_dx;
    }
    if (box_y <= 0 || box_y + BOX_SIZE >= MODE_V_ACTIVE_LINES) {
        box_dy = -box_dy;
        box_y += box_dy;
    }
}

// ============================================================================
// Command Lists (pre-computed at init time)
// ============================================================================

// Standard vblank line (no data island)
static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

// Active video line
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

// Data island placement: DURING hsync pulse (like hsdaoh)
// Structure: front_porch(16) + sync_before_di(52) + preamble(8) + di(36) + back_porch+active(688)
// Sync_before_di = 96 - 8 - 36 = 52
#define SYNC_BEFORE_DI  (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)  // 96 - 8 - 36 = 52

// Pre-computed vblank lines WITH data islands
// Size: front(2) + sync_before(2) + preamble(2) + di_cmd(1) + di_data(36) + back+active(2) + nop(1) = 46
#define VBLANK_DI_MAX_WORDS 64
static uint32_t vblank_acr_vsync_on[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_acr_vsync_on_len;
static uint32_t vblank_infoframe_vsync_on[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_infoframe_vsync_on_len;
static uint32_t vblank_avi_infoframe[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_avi_infoframe_len;

// Audio state
static uint32_t video_frame_count = 0;
static int audio_frame_counter = 0;  // IEC60958 frame counter (0-191)
static uint32_t audio_phase = 0x40000000;  // Start at 1/4 cycle (90 degrees) so first sample isn't zero
static uint32_t audio_sample_accum = 0;    // Fixed-point accumulator for samples per line

// Active line with data island (ping/pong buffers for DMA)
// Size: front(2) + sync_before(2) + preamble(2) + di_cmd(1) + di_data(36) + back(2) + active_cmd(1) = 46
#define VACTIVE_DI_MAX_WORDS 64
static uint32_t vactive_di_ping[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_pong[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_len;

// Pre-computed audio sample packets for vblank back porch
// We pre-compute a full frame's worth of audio packets
static uint32_t vblank_audio_packets[AUDIO_PACKETS_PER_FRAME][VBLANK_DI_MAX_WORDS];
static uint32_t vblank_audio_packet_len;

// Build a line with data island DURING hsync
// This follows the hsdaoh approach exactly
// vsync: true during vsync period, false otherwise
// active: true for active video lines (include TMDS command at end)
static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active) {
    uint32_t *p = buf;

    // Sync symbols for this line
    // During vsync pulse: vsync=0 (active), During non-vsync: vsync=1 (inactive)
    // Note: V0 = vsync active, V1 = vsync inactive (confusing naming)
    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;  // During hsync pulse
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;  // Outside hsync pulse
    uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;  // During hsync, before DI

    // Front porch (16 clocks) - hsync=1
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_h1;

    // Hsync pulse part 1: before data island (52 clocks) - hsync=0
    *p++ = HSTX_CMD_RAW_REPEAT | SYNC_BEFORE_DI;
    *p++ = sync_h0;

    // Data Island preamble (8 clocks) - hsync=0, with DI preamble pattern on lanes 1&2
    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;

    // Data Island (36 clocks) - hsync=0 encoded in TERC4
    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++) {
        *p++ = di_words[i];
    }

    if (active) {
        // Active line: back porch only, then TMDS for active pixels
        *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
        *p++ = sync_h1;
        *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
        // No NOP - pixel data follows immediately
    } else {
        // Vblank line: back porch + active area all blanking
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }

    return (uint32_t)(p - buf);
}

// Wrapper for backward compatibility
static uint32_t build_vblank_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync) {
    return build_line_with_di(buf, di_words, vsync, false);
}

// Fast inline sine lookup for interrupt context
static inline int16_t __scratch_x("") fast_sine_sample(void) {
    // Use top 8 bits of phase for table lookup
    int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
    // Advance phase: phase_increment = (frequency * 2^32) / sample_rate
    audio_phase += (uint32_t)(((uint64_t)TONE_FREQUENCY << 32) / AUDIO_SAMPLE_RATE);
    return s;
}

// Generate audio samples and encode into command buffer for active line
// Returns the length of the command list
// NOTE: This runs in interrupt context - must be fast!
static uint32_t __scratch_x("") generate_active_line_audio(uint32_t *buf, int num_samples) {
    data_packet_t packet;
    hstx_data_island_t island;

    if (num_samples > 0) {
        // Generate samples using fast table lookup
        audio_sample_t samples[4];
        int n = (num_samples > 4) ? 4 : num_samples;
        for (int i = 0; i < n; i++) {
            int16_t s = fast_sine_sample();
            samples[i].left = s;
            samples[i].right = s;
        }

        // Create audio sample packet
        audio_frame_counter = packet_set_audio_samples(&packet, samples, n, audio_frame_counter);

        // Encode for HSTX - active lines: vsync=1 (inactive), hsync=0 (during hsync pulse)
        hstx_encode_data_island(&island, &packet, true, false);
    } else {
        // No audio this line - send null packet
        packet_set_null(&packet);
        hstx_encode_data_island(&island, &packet, true, false);
    }

    // Build command list for active line with this data island
    return build_line_with_di(buf, island.words, false, true);
}

// Initialize sine table
static void init_sine_table(void) {
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = (float)i * 2.0f * 3.14159265f / SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
    }
}

// Generate a sine wave sample (uses table lookup after init)
static inline int16_t generate_sine_sample(void) {
    return fast_sine_sample();
}

// Initialize audio packets and pre-compute command lists
static void init_audio_packets(void) {
    data_packet_t packet;
    hstx_data_island_t island;

    // ACR packet - sent during vsync (vsync=0 active), during hsync (hsync=0)
#if DEBUG_USE_TEST_CTS
    uint32_t cts_value = DEBUG_TEST_CTS;
    printf("*** DEBUG MODE: Using test CTS=0x%05lx ***\n\n", cts_value);
#else
    uint32_t cts_value = AUDIO_CTS_VALUE;
#endif
    packet_set_acr(&packet, AUDIO_N_VALUE, cts_value);

    // Debug: print ACR packet bytes
    printf("ACR packet header: %02x %02x %02x %02x\n",
           packet.header[0], packet.header[1], packet.header[2], packet.header[3]);
    printf("ACR subpacket[0]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           packet.subpacket[0][0], packet.subpacket[0][1], packet.subpacket[0][2], packet.subpacket[0][3],
           packet.subpacket[0][4], packet.subpacket[0][5], packet.subpacket[0][6], packet.subpacket[0][7]);

    // Debug: trace bit shuffle for CTS bytes
    printf("\nBit shuffle debug for CTS bytes:\n");
    debug_encode_byte(0x00);  // SB0 (reserved)
    debug_encode_byte(0x57);  // SB1 = CTS[7:0]
    debug_encode_byte(0x62);  // SB2 = CTS[15:8]
    debug_encode_byte(0x00);  // SB3 = CTS[19:16]
    printf("\n");

    // Debug: compare with hsdaoh encoding
    debug_hsdaoh_acr_comparison(AUDIO_N_VALUE, cts_value);

    hstx_encode_data_island(&island, &packet, false, false);  // vsync=0, hsync=0 (during hsync pulse in vsync)

    // Debug: print decoded data island
    printf("\nEncoded data island (decoded back to nibbles):\n");
    debug_print_data_island(&island);

    // Debug: dump all 36 words
    printf("\n");
    debug_dump_data_island(&island);

    vblank_acr_vsync_on_len = build_vblank_with_di(vblank_acr_vsync_on, island.words, true);

    // Audio InfoFrame - also during vsync (vsync=0 means active-low vsync pulse)
    packet_set_audio_infoframe(&packet, AUDIO_SAMPLE_RATE, 2, 16);
    printf("\nAudio InfoFrame packet:\n");
    printf("  Header: %02x %02x %02x %02x\n",
           packet.header[0], packet.header[1], packet.header[2], packet.header[3]);
    printf("  Subpacket[0]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           packet.subpacket[0][0], packet.subpacket[0][1], packet.subpacket[0][2],
           packet.subpacket[0][3], packet.subpacket[0][4], packet.subpacket[0][5],
           packet.subpacket[0][6], packet.subpacket[0][7]);
    hstx_encode_data_island(&island, &packet, false, false);  // vsync=0, hsync=0
    vblank_infoframe_vsync_on_len = build_vblank_with_di(vblank_infoframe_vsync_on, island.words, true);

    // AVI InfoFrame - sent during front porch (VIC 1 = 640x480 @ 60Hz)
    packet_set_avi_infoframe(&packet, 1);  // VIC 1 = 640x480 @ 60Hz
    printf("\nAVI InfoFrame packet:\n");
    printf("  Header: %02x %02x %02x %02x\n",
           packet.header[0], packet.header[1], packet.header[2], packet.header[3]);
    printf("  Subpacket[0]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           packet.subpacket[0][0], packet.subpacket[0][1], packet.subpacket[0][2],
           packet.subpacket[0][3], packet.subpacket[0][4], packet.subpacket[0][5],
           packet.subpacket[0][6], packet.subpacket[0][7]);
    hstx_encode_data_island(&island, &packet, true, false);  // vsync=1 (inactive), hsync=0
    vblank_avi_infoframe_len = build_vblank_with_di(vblank_avi_infoframe, island.words, false);

    // Pre-compute audio sample packets for one frame
    // These will be sent during vblank back porch (vsync=1, hsync=0 during DI)
    printf("Generating audio sample packets...\n");
    for (int i = 0; i < AUDIO_PACKETS_PER_FRAME; i++) {
        // Generate 4 stereo samples per packet
        audio_sample_t samples[4];
        for (int j = 0; j < 4; j++) {
            int16_t s = generate_sine_sample();
            samples[j].left = s;
            samples[j].right = s;  // Mono for now
        }

        // Create audio sample packet
        audio_frame_counter = packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);

        // Debug: print first audio sample packet
        if (i == 0) {
            printf("\nFirst Audio Sample packet:\n");
            printf("  Header: %02x %02x %02x %02x\n",
                   packet.header[0], packet.header[1], packet.header[2], packet.header[3]);
            for (int k = 0; k < 4; k++) {
                printf("  Subpacket[%d]: %02x %02x %02x %02x %02x %02x %02x %02x\n", k,
                       packet.subpacket[k][0], packet.subpacket[k][1], packet.subpacket[k][2],
                       packet.subpacket[k][3], packet.subpacket[k][4], packet.subpacket[k][5],
                       packet.subpacket[k][6], packet.subpacket[k][7]);
            }
            printf("  Samples: [%d, %d, %d, %d]\n",
                   samples[0].left, samples[1].left, samples[2].left, samples[3].left);
        }

        // Encode for HSTX - during back porch: vsync=1 (inactive), hsync=0 (during hsync pulse)
        hstx_encode_data_island(&island, &packet, true, false);  // vsync=1, hsync=0

        // Debug: print first audio sample packet's encoded data island
        if (i == 0) {
            printf("\nFirst Audio Sample data island:\n");
            debug_dump_data_island(&island);
        }

        // Build the command list (vsync=false because we're after vsync period)
        vblank_audio_packet_len = build_vblank_with_di(vblank_audio_packets[i], island.words, false);
    }

    printf("Audio packets initialized:\n");
    printf("  ACR: N=%d, CTS=%lu (0x%05lx)\n", AUDIO_N_VALUE, cts_value, cts_value);
    printf("  Sample rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    printf("  Tone: %d Hz\n", TONE_FREQUENCY);
    printf("  Samples per frame target: %d\n", SAMPLES_PER_FRAME);
    printf("  Lines per frame: %d\n", LINES_PER_FRAME);
    printf("  Samples/line (16.16 fixed): %lu (~%.2f)\n",
           (unsigned long)SAMPLES_PER_LINE_FP, SAMPLES_PER_LINE_FP / 65536.0f);
    printf("  Vblank audio packets: %d (pre-computed)\n", AUDIO_PACKETS_PER_FRAME);
#if ENABLE_ACTIVE_LINE_AUDIO
    printf("  Active line audio: DYNAMIC (generated per-line)\n");
#else
    printf("  Active line audio: DISABLED (vblank-only mode)\n");
    printf("  WARNING: Only %d samples/frame vs %d needed!\n",
           AUDIO_PACKETS_PER_FRAME * 4, SAMPLES_PER_FRAME);
#endif
    printf("  ACR cmd list: %lu words\n", vblank_acr_vsync_on_len);
    printf("  Audio packet cmd list: %lu words\n", vblank_audio_packet_len);

    // Dump the ACR command list structure
    printf("\nACR command list dump (first %lu words):\n", vblank_acr_vsync_on_len);
    uint32_t idx = 0;
    while (idx < vblank_acr_vsync_on_len) {
        uint32_t cmd = vblank_acr_vsync_on[idx];
        uint16_t opcode = (cmd >> 12) & 0xF;
        uint16_t count = cmd & 0xFFF;

        if (opcode == 0xF) {  // NOP
            printf("  [%2lu] NOP\n", idx);
            idx++;
        } else if (opcode == 0x1) {  // RAW_REPEAT
            printf("  [%2lu] RAW_REPEAT x%d: 0x%08lx\n", idx, count, vblank_acr_vsync_on[idx+1]);
            idx += 2;
        } else if (opcode == 0x0) {  // RAW
            printf("  [%2lu] RAW x%d: ", idx, count);
            if (count <= 4) {
                for (int i = 1; i <= count && idx+i < vblank_acr_vsync_on_len; i++) {
                    printf("0x%08lx ", vblank_acr_vsync_on[idx+i]);
                }
            } else {
                printf("first=0x%08lx last=0x%08lx",
                       vblank_acr_vsync_on[idx+1],
                       vblank_acr_vsync_on[idx+count]);
            }
            printf("\n");
            idx += 1 + count;
        } else {
            printf("  [%2lu] CMD 0x%x count=%d\n", idx, opcode, count);
            idx++;
        }
    }
}

// ============================================================================
// DMA Handler
// ============================================================================

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static uint audio_packet_index = 0;

void __scratch_x("") dma_irq_handler() {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH &&
        v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {

        // Vsync period (lines 10-11) - send ACR and Audio InfoFrame
        if (v_scanline == MODE_V_FRONT_PORCH) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
            ch->transfer_count = vblank_acr_vsync_on_len;
            video_frame_count++;
            audio_sample_accum = 0;  // Reset accumulator at frame start
        } else {
            ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
            ch->transfer_count = vblank_infoframe_vsync_on_len;
        }

    } else if (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
               v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        // Vblank back porch (lines 12-44) - send audio sample packets (pre-computed)
        if (audio_packet_index < AUDIO_PACKETS_PER_FRAME) {
            ch->read_addr = (uintptr_t)vblank_audio_packets[audio_packet_index];
            ch->transfer_count = vblank_audio_packet_len;
            audio_packet_index++;
        } else {
            ch->read_addr = (uintptr_t)vblank_line_vsync_off;
            ch->transfer_count = count_of(vblank_line_vsync_off);
        }
        // Accumulate samples for vblank lines too
        audio_sample_accum += SAMPLES_PER_LINE_FP;

    } else if (v_scanline < MODE_V_FRONT_PORCH) {
        // Front porch (lines 0-9) - send AVI InfoFrame on line 0, audio on rest
        if (v_scanline == 0) {
            ch->read_addr = (uintptr_t)vblank_avi_infoframe;
            ch->transfer_count = vblank_avi_infoframe_len;
            audio_packet_index = 0;  // Reset for back porch
        } else if (audio_packet_index < AUDIO_PACKETS_PER_FRAME) {
            // Send audio during front porch too (lines 1-9)
            ch->read_addr = (uintptr_t)vblank_audio_packets[audio_packet_index];
            ch->transfer_count = vblank_audio_packet_len;
            audio_packet_index++;
        } else {
            ch->read_addr = (uintptr_t)vblank_line_vsync_off;
            ch->transfer_count = count_of(vblank_line_vsync_off);
        }
        // Accumulate samples for vblank lines too
        audio_sample_accum += SAMPLES_PER_LINE_FP;

    } else if (!vactive_cmdlist_posted) {
#if ENABLE_ACTIVE_LINE_AUDIO
        // Active video lines - generate audio dynamically
        // Calculate how many samples to send this line
        audio_sample_accum += SAMPLES_PER_LINE_FP;
        int num_samples = audio_sample_accum >> 16;
        if (num_samples > 4) num_samples = 4;
        audio_sample_accum -= num_samples << 16;

        // Use ping/pong buffer based on which DMA channel we're setting up
        uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
        vactive_di_len = generate_active_line_audio(buf, num_samples);

        ch->read_addr = (uintptr_t)buf;
        ch->transfer_count = vactive_di_len;
#else
        // Legacy mode: use static vactive_line (no audio during active video)
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
#endif
        vactive_cmdlist_posted = true;
    } else {
        // Post framebuffer pixels for this line
        ch->read_addr = (uintptr_t)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    stdio_init_all();

    // Wait for USB serial to enumerate
    sleep_ms(2000);

    printf("\n\n");
    printf("================================\n");
    printf("HSTX Audio Test\n");
    printf("================================\n");
    printf("640x480 @ 60Hz with HDMI audio\n\n");

    // Initialize framebuffer
    draw_frame();

    // Initialize sine table for fast audio generation
    init_sine_table();

    // Initialize audio packets (pre-compute command lists)
    init_audio_packets();

    // Configure HSTX TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |  // 150MHz / 5 = 30MHz (RP2350 default is 150MHz, not 125MHz!)
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Spotpear pinout
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;

    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + lane * 2;
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0);
    }

    // DMA setup
    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PING,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PONG,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    printf("Starting HSTX output...\n");
    dma_channel_start(DMACH_PING);

    // Animation loop
    uint32_t last_frame = 0;
    while (1) {
        sleep_ms(16);
        update_box();
        draw_frame();

        // Print status every second
        if (video_frame_count >= last_frame + 60) {
            printf("Frame %lu\n", video_frame_count);
            last_frame = video_frame_count;
        }
    }

    return 0;
}

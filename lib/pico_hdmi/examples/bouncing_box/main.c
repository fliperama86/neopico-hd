/**
 * pico_hdmi Bouncing Box Example
 *
 * Demonstrates basic usage of the pico_hdmi library:
 * - 640x480 @ 60Hz HDMI output
 * - Scanline callback for rendering
 * - HDMI audio (Für Elise melody)
 * - Simple animation
 *
 * Target: RP2350 (WeAct RP2350B)
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "pico_hdmi/video_output.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include <string.h>
#include <math.h>

// ============================================================================
// Configuration
// ============================================================================

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240

#define BOX_SIZE 32
#define BG_COLOR   0x0010  // Dark blue (RGB565)
#define BOX_COLOR  0xFFE0  // Yellow (RGB565)

// Audio configuration
#define AUDIO_SAMPLE_RATE 48000
#define TONE_AMPLITUDE 6000

// ============================================================================
// Animation State
// ============================================================================

static volatile int box_x = 50, box_y = 50;
static int box_dx = 2, box_dy = 1;

// ============================================================================
// Audio State - Für Elise
// ============================================================================

#define SINE_TABLE_SIZE 256
static int16_t sine_table[SINE_TABLE_SIZE];
static uint32_t audio_phase = 0;
static uint32_t phase_increment = 0;
static int audio_frame_counter = 0;

// Note frequencies (Hz)
enum {
    REST = 0,
    E3 = 165, G3S = 208, A3 = 220, B3 = 247,
    C4 = 262, D4 = 294, E4 = 330, F4 = 349, G4 = 392, G4S = 415, A4 = 440, B4 = 494,
    C5 = 523, D5 = 587, D5S = 622, E5 = 659, F5 = 698, G5 = 784, G5S = 831, A5 = 880
};

typedef struct { uint16_t freq; uint8_t duration; } note_t;

// Melody 1: Für Elise opening theme (Beethoven)
static const note_t fur_elise[] = {
    // E D# E D# E B D C
    {E5, 12}, {D5S, 12}, {E5, 12}, {D5S, 12}, {E5, 12}, {B4, 12}, {D5, 12}, {C5, 12},
    // A (held)
    {A4, 24}, {REST, 6},
    // C E A
    {C4, 12}, {E4, 12}, {A4, 12},
    // B (held)
    {B4, 24}, {REST, 6},
    // E G# B
    {E4, 12}, {G4S, 12}, {B4, 12},
    // C (held)
    {C5, 24}, {REST, 6},
    // E D# E D# E B D C
    {E5, 12}, {D5S, 12}, {E5, 12}, {D5S, 12}, {E5, 12}, {B4, 12}, {D5, 12}, {C5, 12},
    // A (held)
    {A4, 24}, {REST, 6},
    // C E A
    {C4, 12}, {E4, 12}, {A4, 12},
    // B (held)
    {B4, 24}, {REST, 6},
    // E C B
    {E4, 12}, {C5, 12}, {B4, 12},
    // A (held) - end phrase, loop
    {A4, 36}, {REST, 18},
};

// Melody 2: Korobeiniki / Tetris Theme (Russian folk song, 1861)
static const note_t korobeiniki[] = {
    // Section A (Part 1)
    {E5, 18}, {B4, 9}, {C5, 9}, {D5, 18}, {C5, 9}, {B4, 9},
    {A4, 18}, {A4, 9}, {C5, 9}, {E5, 18}, {D5, 9}, {C5, 9},
    {B4, 27}, {C5, 9}, {D5, 18}, {E5, 18},
    {C5, 18}, {A4, 18}, {A4, 18}, {REST, 18},

    // Section A (Part 2 - the drop)
    {D5, 27}, {F5, 9}, {A5, 18}, {G5, 9}, {F5, 9},
    {E5, 27}, {C5, 9}, {E5, 18}, {D5, 9}, {C5, 9},
    {B4, 18}, {B4, 9}, {C5, 9}, {D5, 18}, {E5, 18},
    {C5, 18}, {A4, 18}, {A4, 18}, {REST, 18},

    // Section B (The bridge)
    {E5, 36}, {C5, 36},
    {D5, 36}, {B4, 36},
    {C5, 36}, {A4, 36},
    {G4S, 36}, {B4, 36},

    {E5, 36}, {C5, 36},
    {D5, 36}, {B4, 36},
    {C5, 18}, {E5, 18}, {A5, 36},
    {G5S, 72},
};

#define FUR_ELISE_LENGTH (sizeof(fur_elise) / sizeof(fur_elise[0]))
#define KOROBEINIKI_LENGTH (sizeof(korobeiniki) / sizeof(korobeiniki[0]))

// Use Korobeiniki for the demo (Für Elise kept for reference)
static const note_t *current_melody = korobeiniki;
static int current_melody_length = KOROBEINIKI_LENGTH;

static int melody_index = 0;
static int note_frames_remaining = 0;

static void init_sine_table(void) {
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = (float)i * 2.0f * 3.14159265f / SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
    }
}

static void advance_melody(void) {
    if (--note_frames_remaining <= 0) {
        melody_index = (melody_index + 1) % current_melody_length;

        note_frames_remaining = current_melody[melody_index].duration;
        uint16_t freq = current_melody[melody_index].freq;
        if (freq > 0) {
            phase_increment = (uint32_t)(((uint64_t)freq << 32) / AUDIO_SAMPLE_RATE);
        } else {
            phase_increment = 0;  // Rest
        }
    }
}

static inline int16_t get_sine_sample(void) {
    if (phase_increment == 0) return 0;  // Rest
    int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
    audio_phase += phase_increment;
    return s;
}

static void generate_audio(void) {
    // Keep the audio queue fed
    while (hstx_di_queue_get_level() < 200) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            int16_t s = get_sine_sample();
            samples[i].left = s;
            samples[i].right = s;
        }

        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);

        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}

// ============================================================================
// Scanline Callback (runs on Core 1)
// ============================================================================

void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst) {
    (void)v_scanline;

    // 2x vertical scaling
    int fb_line = active_line / 2;

    // Read current box position
    int bx = box_x;
    int by = box_y;

    uint32_t bg = BG_COLOR | (BG_COLOR << 16);
    uint32_t box = BOX_COLOR | (BOX_COLOR << 16);

    // Check if this line intersects the box vertically
    if (fb_line >= by && fb_line < by + BOX_SIZE) {
        // Three regions: before box, box, after box
        int i = 0;

        // Region 1: before box
        for (; i < bx; i++) {
            dst[i] = bg;
        }

        // Region 2: box
        for (; i < bx + BOX_SIZE && i < FRAME_WIDTH; i++) {
            dst[i] = box;
        }

        // Region 3: after box
        for (; i < FRAME_WIDTH; i++) {
            dst[i] = bg;
        }
    } else {
        // Fast path: entire line is background
        for (int i = 0; i < FRAME_WIDTH; i++) {
            dst[i] = bg;
        }
    }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

static void update_box(void) {
    int x = box_x + box_dx;
    int y = box_y + box_dy;

    if (x <= 0 || x + BOX_SIZE >= FRAME_WIDTH) {
        box_dx = -box_dx;
        x = box_x + box_dx;
    }
    if (y <= 0 || y + BOX_SIZE >= FRAME_HEIGHT) {
        box_dy = -box_dy;
        y = box_y + box_dy;
    }

    box_x = x;
    box_y = y;
}

int main(void) {
    // Set system clock to 126 MHz for HSTX timing
    set_sys_clock_khz(126000, true);

    stdio_init_all();

    // Initialize LED for heartbeat
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    sleep_ms(1000);

    // Initialize audio
    init_sine_table();
    note_frames_remaining = current_melody[0].duration;
    phase_increment = (uint32_t)(((uint64_t)current_melody[0].freq << 32) / AUDIO_SAMPLE_RATE);

    // Initialize HDMI output
    hstx_di_queue_init();
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);

    // Register scanline callback
    video_output_set_scanline_callback(scanline_callback);

    // Pre-fill audio buffer
    generate_audio();

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Main loop - animation + audio
    uint32_t last_frame = 0;
    bool led_state = false;

    while (1) {
        // Keep audio buffer fed
        generate_audio();

        while (video_frame_count == last_frame) {
            generate_audio();
            tight_loop_contents();
        }
        last_frame = video_frame_count;

        // Update animation
        update_box();

        // Advance melody (one note step per frame)
        advance_melody();

        // LED heartbeat
        if ((video_frame_count % 30) == 0) {
            led_state = !led_state;
            gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        }
    }

    return 0;
}

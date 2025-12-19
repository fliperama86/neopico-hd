/**
 * Audio Pipeline Test Application
 *
 * Displays pipeline status on screen with colored indicators.
 * Uses buttons to toggle pipeline stages.
 *
 * Screen layout:
 * ┌─────────────────────────────────────────────┐
 * │                                             │
 * │   [I2S]  ──▶  [DC]  ──▶  [SRC]  ──▶  HDMI  │
 * │    ●          ○           ●                 │
 * │   55.5k      OFF        LINEAR    48k       │
 * │                                             │
 * │   BTN1: DC Filter    BTN2: SRC Mode         │
 * └─────────────────────────────────────────────┘
 *
 * Colors:
 * - Green box: Stage enabled/active
 * - Dark/Red box: Stage disabled/bypassed
 * - Background: Dark blue
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "audio_ring.h"

#include "neopico_config.h"
#include "audio/audio_pipeline.h"

// =============================================================================
// Display Configuration
// =============================================================================

#define FRAME_WIDTH 320
// With HDMI audio blank_settings (top=8, bottom=8), 16 DVI lines are blanked
// 480 - 16 = 464 visible DVI lines = 232 user scanlines (with 2x pixel doubling)
#define FRAME_HEIGHT 232
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

// Colors (RGB565)
#define COLOR_BG        0x0011  // Dark blue
#define COLOR_BOX_ON    0x07E0  // Green
#define COLOR_BOX_OFF   0x4208  // Dark gray
#define COLOR_ARROW     0xFFFF  // White
#define COLOR_TEXT      0xFFFF  // White
#define COLOR_LABEL     0xBDF7  // Light gray

// Box dimensions
#define BOX_WIDTH  48
#define BOX_HEIGHT 32
#define BOX_Y      100
#define ARROW_Y    (BOX_Y + BOX_HEIGHT/2)

// Box X positions
#define BOX_I2S_X   30
#define BOX_DC_X    110
#define BOX_SRC_X   190
#define BOX_OUT_X   270

// =============================================================================
// DVI State
// =============================================================================

struct dvi_inst dvi0;
static uint16_t scanline_buf[2][FRAME_WIDTH];
// PicoDVI's audio_sample_t (different from our ap_sample_t)
static audio_sample_t hdmi_audio_buffer[256];

// =============================================================================
// Audio Pipeline State
// =============================================================================

static audio_pipeline_t pipeline;
static audio_pipeline_status_t pipeline_status;

// =============================================================================
// Drawing Helpers
// =============================================================================

static void draw_hline(uint16_t *buf, int x, int width, uint16_t color) {
    for (int i = 0; i < width && (x + i) < FRAME_WIDTH; i++) {
        if (x + i >= 0) {
            buf[x + i] = color;
        }
    }
}

static void draw_box_scanline(uint16_t *buf, int y, int box_x, int box_y, int box_w, int box_h, uint16_t color) {
    // Check if this scanline intersects the box
    if (y < box_y || y >= box_y + box_h) return;

    // Top or bottom border
    if (y == box_y || y == box_y + box_h - 1) {
        draw_hline(buf, box_x, box_w, color);
    } else {
        // Left and right borders + fill
        if (box_x >= 0 && box_x < FRAME_WIDTH) buf[box_x] = color;
        if (box_x + box_w - 1 >= 0 && box_x + box_w - 1 < FRAME_WIDTH) buf[box_x + box_w - 1] = color;

        // Fill interior with slightly darker version
        uint16_t fill = (color == COLOR_BOX_ON) ? 0x0400 : 0x2104;
        for (int x = box_x + 2; x < box_x + box_w - 2; x++) {
            if (x >= 0 && x < FRAME_WIDTH) buf[x] = fill;
        }
    }
}

static void draw_arrow_scanline(uint16_t *buf, int y, int x_start, int x_end) {
    // Draw arrow at center Y position (3 pixel high arrow)
    if (y >= ARROW_Y - 1 && y <= ARROW_Y + 1) {
        // Horizontal line
        if (y == ARROW_Y) {
            draw_hline(buf, x_start, x_end - x_start, COLOR_ARROW);
        }
        // Arrowhead
        int tip_x = x_end - 1;
        if (y == ARROW_Y - 1 || y == ARROW_Y + 1) {
            if (tip_x - 3 >= 0 && tip_x - 3 < FRAME_WIDTH) buf[tip_x - 3] = COLOR_ARROW;
        }
        if (y == ARROW_Y) {
            if (tip_x - 2 >= 0) buf[tip_x - 2] = COLOR_ARROW;
            if (tip_x - 1 >= 0) buf[tip_x - 1] = COLOR_ARROW;
        }
    }
}

// Simple 3x5 digit patterns (for rate display)
static const uint8_t digit_patterns[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b010, 0b010, 0b010}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
};

static void draw_digit_scanline(uint16_t *buf, int y, int digit_x, int digit_y, int digit, uint16_t color) {
    if (digit < 0 || digit > 9) return;
    int row = y - digit_y;
    if (row < 0 || row >= 5) return;

    uint8_t pattern = digit_patterns[digit][row];
    for (int col = 0; col < 3; col++) {
        if (pattern & (0b100 >> col)) {
            int x = digit_x + col;
            if (x >= 0 && x < FRAME_WIDTH) buf[x] = color;
        }
    }
}

static void draw_number_scanline(uint16_t *buf, int y, int x, int num_y, int number, uint16_t color) {
    // Draw up to 5 digits
    char str[6];
    snprintf(str, sizeof(str), "%d", number);

    int pos = x;
    for (int i = 0; str[i] && i < 5; i++) {
        if (str[i] >= '0' && str[i] <= '9') {
            draw_digit_scanline(buf, y, pos, num_y, str[i] - '0', color);
            pos += 4;  // 3 pixels wide + 1 spacing
        }
    }
}

// Letter patterns for "ON", "OFF", mode names
static void draw_text_scanline(uint16_t *buf, int y, int text_x, int text_y, const char *text, uint16_t color) {
    // Simple placeholder - just draw a line where text would be
    if (y == text_y + 2) {
        int len = strlen(text);
        draw_hline(buf, text_x, len * 4, color);
    }
}

// =============================================================================
// Generate Scanline - Simple color bars with status indicator
// =============================================================================

// Color bars
static const uint16_t color_bars[] = {
    0xFFFF,  // White
    0xFFE0,  // Yellow
    0x07FF,  // Cyan
    0x07E0,  // Green
    0xF81F,  // Magenta
    0xF800,  // Red
    0x001F,  // Blue
    0x0000   // Black
};

static void generate_scanline(uint16_t *buf, int y) {
    // Dark blue background
    for (int x = 0; x < FRAME_WIDTH; x++) {
        buf[x] = COLOR_BG;
    }

    // === STATUS PANEL at bottom (y >= 180) ===
    if (y >= 180) {
        int panel_y = y - 180;  // 0-51 within panel

        // DC Filter box (left side: x = 20-100)
        int dc_box_x = 20, dc_box_w = 80, dc_box_h = 40;
        if (panel_y >= 5 && panel_y < 5 + dc_box_h) {
            int box_y = panel_y - 5;
            uint16_t dc_color = pipeline_status.dc_filter_enabled ? COLOR_BOX_ON : COLOR_BOX_OFF;

            // Box border
            if (box_y == 0 || box_y == dc_box_h - 1) {
                for (int x = dc_box_x; x < dc_box_x + dc_box_w; x++) buf[x] = dc_color;
            } else {
                buf[dc_box_x] = dc_color;
                buf[dc_box_x + dc_box_w - 1] = dc_color;
                // Fill
                uint16_t fill = pipeline_status.dc_filter_enabled ? 0x0400 : 0x2104;
                for (int x = dc_box_x + 2; x < dc_box_x + dc_box_w - 2; x++) buf[x] = fill;
            }

            // "DC" text (simple) - at y offset 15-19 within box
            if (box_y >= 15 && box_y < 20) {
                int text_row = box_y - 15;
                // D pattern (5 rows)
                const uint8_t D_pat[5] = {0b1110, 0b1001, 0b1001, 0b1001, 0b1110};
                // C pattern
                const uint8_t C_pat[5] = {0b0111, 0b1000, 0b1000, 0b1000, 0b0111};

                int tx = dc_box_x + 28;
                for (int bit = 0; bit < 4; bit++) {
                    if (D_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0xFFFF;
                }
                tx += 6;
                for (int bit = 0; bit < 4; bit++) {
                    if (C_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0xFFFF;
                }

                // ON/OFF indicator
                tx = dc_box_x + 45;
                if (pipeline_status.dc_filter_enabled) {
                    // "ON"
                    const uint8_t O_pat[5] = {0b0110, 0b1001, 0b1001, 0b1001, 0b0110};
                    const uint8_t N_pat[5] = {0b1001, 0b1101, 0b1011, 0b1001, 0b1001};
                    for (int bit = 0; bit < 4; bit++) {
                        if (O_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0x07E0;
                    }
                    tx += 6;
                    for (int bit = 0; bit < 4; bit++) {
                        if (N_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0x07E0;
                    }
                } else {
                    // "OFF"
                    const uint8_t O_pat[5] = {0b0110, 0b1001, 0b1001, 0b1001, 0b0110};
                    const uint8_t F_pat[5] = {0b1111, 0b1000, 0b1110, 0b1000, 0b1000};
                    for (int bit = 0; bit < 4; bit++) {
                        if (O_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0xF800;
                    }
                    tx += 6;
                    for (int bit = 0; bit < 4; bit++) {
                        if (F_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0xF800;
                    }
                    tx += 6;
                    for (int bit = 0; bit < 4; bit++) {
                        if (F_pat[text_row] & (0b1000 >> bit)) buf[tx + bit] = 0xF800;
                    }
                }
            }
        }

        // SRC Mode box (right side: x = 140-280)
        int src_box_x = 140, src_box_w = 140, src_box_h = 40;
        if (panel_y >= 5 && panel_y < 5 + src_box_h) {
            int box_y = panel_y - 5;
            uint16_t src_color;
            switch (pipeline_status.src_mode) {
                case SRC_MODE_NONE:   src_color = 0xF800; break;
                case SRC_MODE_DROP:   src_color = 0xFFE0; break;
                case SRC_MODE_LINEAR: src_color = 0x07E0; break;
                default:              src_color = 0x4208; break;
            }

            // Box border
            if (box_y == 0 || box_y == src_box_h - 1) {
                for (int x = src_box_x; x < src_box_x + src_box_w; x++) buf[x] = src_color;
            } else {
                buf[src_box_x] = src_color;
                buf[src_box_x + src_box_w - 1] = src_color;
                // Darker fill
                for (int x = src_box_x + 2; x < src_box_x + src_box_w - 2; x++) buf[x] = 0x0000;
            }

            // SRC mode number: 0=NONE, 1=DROP, 2=LINEAR
            if (box_y >= 12 && box_y < 25) {
                int num_y = box_y - 12;
                int mode_num = (int)pipeline_status.src_mode;

                // Large digit (13 pixels high)
                // Simple block digit patterns (7 wide x 13 high)
                int dx = src_box_x + 20;
                if (num_y < 13) {
                    // Draw big mode number
                    const uint8_t big_0[13] = {0x3E,0x7F,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x7F,0x3E};
                    const uint8_t big_1[13] = {0x0C,0x1C,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3F,0x3F};
                    const uint8_t big_2[13] = {0x3E,0x7F,0x63,0x03,0x03,0x06,0x0C,0x18,0x30,0x60,0x60,0x7F,0x7F};
                    const uint8_t *pat = (mode_num == 0) ? big_0 : (mode_num == 1) ? big_1 : big_2;

                    for (int bit = 0; bit < 7; bit++) {
                        if (pat[num_y] & (0x40 >> bit)) buf[dx + bit * 2] = src_color;
                        if (pat[num_y] & (0x40 >> bit)) buf[dx + bit * 2 + 1] = src_color;
                    }
                }

                // Mode label
                int lx = src_box_x + 50;
                if (num_y >= 3 && num_y < 8) {
                    int tr = num_y - 3;
                    // Mode labels
                    if (pipeline_status.src_mode == SRC_MODE_NONE) {
                        // "NONE"
                        const uint8_t N[5]={0b1001,0b1101,0b1011,0b1001,0b1001};
                        const uint8_t O[5]={0b0110,0b1001,0b1001,0b1001,0b0110};
                        const uint8_t E[5]={0b1111,0b1000,0b1110,0b1000,0b1111};
                        for(int b=0;b<4;b++){if(N[tr]&(8>>b))buf[lx+b]=src_color;}lx+=6;
                        for(int b=0;b<4;b++){if(O[tr]&(8>>b))buf[lx+b]=src_color;}lx+=6;
                        for(int b=0;b<4;b++){if(N[tr]&(8>>b))buf[lx+b]=src_color;}lx+=6;
                        for(int b=0;b<4;b++){if(E[tr]&(8>>b))buf[lx+b]=src_color;}
                    } else if (pipeline_status.src_mode == SRC_MODE_DROP) {
                        // "DROP"
                        const uint8_t D[5]={0b1110,0b1001,0b1001,0b1001,0b1110};
                        const uint8_t R[5]={0b1110,0b1001,0b1110,0b1010,0b1001};
                        const uint8_t O[5]={0b0110,0b1001,0b1001,0b1001,0b0110};
                        const uint8_t P[5]={0b1110,0b1001,0b1110,0b1000,0b1000};
                        for(int b=0;b<4;b++){if(D[tr]&(8>>b))buf[lx+b]=src_color;}lx+=6;
                        for(int b=0;b<4;b++){if(R[tr]&(8>>b))buf[lx+b]=src_color;}lx+=6;
                        for(int b=0;b<4;b++){if(O[tr]&(8>>b))buf[lx+b]=src_color;}lx+=6;
                        for(int b=0;b<4;b++){if(P[tr]&(8>>b))buf[lx+b]=src_color;}
                    } else {
                        // "LINEAR"
                        const uint8_t L[5]={0b1000,0b1000,0b1000,0b1000,0b1111};
                        const uint8_t I[5]={0b1110,0b0100,0b0100,0b0100,0b1110};
                        const uint8_t N[5]={0b1001,0b1101,0b1011,0b1001,0b1001};
                        const uint8_t E[5]={0b1111,0b1000,0b1110,0b1000,0b1111};
                        const uint8_t A[5]={0b0110,0b1001,0b1111,0b1001,0b1001};
                        const uint8_t R[5]={0b1110,0b1001,0b1110,0b1010,0b1001};
                        for(int b=0;b<4;b++){if(L[tr]&(8>>b))buf[lx+b]=src_color;}lx+=5;
                        for(int b=0;b<4;b++){if(I[tr]&(8>>b))buf[lx+b]=src_color;}lx+=5;
                        for(int b=0;b<4;b++){if(N[tr]&(8>>b))buf[lx+b]=src_color;}lx+=5;
                        for(int b=0;b<4;b++){if(E[tr]&(8>>b))buf[lx+b]=src_color;}lx+=5;
                        for(int b=0;b<4;b++){if(A[tr]&(8>>b))buf[lx+b]=src_color;}lx+=5;
                        for(int b=0;b<4;b++){if(R[tr]&(8>>b))buf[lx+b]=src_color;}
                    }
                }
            }
        }
    }

    // Horizontal reference lines (to detect scrolling)
    if (y == 30 || y == 90 || y == 150) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            buf[x] = 0x4208;  // Dark gray line
        }
    }
}

// =============================================================================
// Audio Output Callback
// =============================================================================

// TX diagnostics
static uint32_t tx_samples_written = 0;
static uint32_t tx_samples_dropped = 0;
static int tx_buffer_min = 256;  // Track minimum buffer space seen
static int tx_buffer_max = 0;    // Track maximum buffer usage seen

static void audio_output_callback(const ap_sample_t *samples, uint32_t count, void *ctx) {
    (void)ctx;

    for (uint32_t i = 0; i < count; i++) {
        int available = get_write_size(&dvi0.audio_ring, false);

        // Track buffer stats
        int used = 256 - available;  // Buffer size is 256
        if (used > tx_buffer_max) tx_buffer_max = used;
        if (available < tx_buffer_min) tx_buffer_min = available;

        if (available > 0) {
            // PicoDVI uses its own audio_sample_t with channels[] array
            audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);
            ptr->channels[0] = samples[i].left;
            ptr->channels[1] = samples[i].right;
            increase_write_pointer(&dvi0.audio_ring, 1);
            tx_samples_written++;
        } else {
            tx_samples_dropped++;
        }
    }
}

// =============================================================================
// Core 1: DVI Output
// =============================================================================

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    // Set voltage and clock for DVI
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    // LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // NOISE FIX: Disable video GPIO input buffers
    // When video signals drive floating inputs, the internal buffers switch
    // at 6 MHz, causing noise that couples into I2S capture.
    // Disable input enable to stop the switching without shorting outputs.
    for (int pin = 0; pin <= 15; pin++) {
        gpio_init(pin);
        gpio_set_input_enabled(pin, false);
    }
    gpio_init(22);
    gpio_set_input_enabled(22, false);

    // Early blink to show we're alive
    for (int i = 0; i < 3; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(100);
    }
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    sleep_ms(1000);  // Wait for USB to enumerate

    printf("\n");
    printf("===========================================\n");
    printf("  Audio Pipeline Test\n");
    printf("===========================================\n");
    printf("BTN1 (GP21): Toggle DC Filter\n");
    printf("BTN2 (GP23): Cycle SRC Mode\n");
    printf("(Connect to GND to activate)\n");
    printf("\n");

    printf("Initializing DVI...\n");

    // Initialize DVI
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    printf("DVI initialized\n");

    // HDMI Audio - blank settings reduce visible area
    // top=8, bottom=8 means 16 DVI lines blanked = 8 user scanlines
    // So FRAME_HEIGHT must be 240 - 8 = 232
    dvi_get_blank_settings(&dvi0)->top = 4 * 2;
    dvi_get_blank_settings(&dvi0)->bottom = 4 * 2;
    dvi_audio_sample_buffer_set(&dvi0, hdmi_audio_buffer, 256);
    dvi_set_audio_freq(&dvi0, 48000, 25200, 6144);
    printf("HDMI audio configured (232 scanlines)\n");

    // Initialize audio pipeline
    printf("Initializing audio pipeline...\n");
    audio_pipeline_config_t pipeline_config = {
        .pin_dat = 36,   // Bank 1 - in_base (lowest GPIO, for `in pins`)
        .pin_ws = 37,    // Bank 1 - `wait pin 1`
        .pin_bck = 38,   // Bank 1 - `wait pin 2`
        .pin_btn1 = 21,  // Bank 0 - freed from old BCK
        .pin_btn2 = 23,  // Bank 0 - freed from old DAT
        .pio = pio1,
        .sm = 0
    };

    if (!audio_pipeline_init(&pipeline, &pipeline_config)) {
        printf("ERROR: Failed to initialize audio pipeline!\n");
        while (1) {
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(100);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            sleep_ms(100);
        }
    }
    printf("Audio pipeline initialized\n");

    printf("Launching DVI on Core 1...\n");

    // Launch DVI on Core 1
    multicore_launch_core1(core1_main);
    printf("DVI started on Core 1\n");

    // DEBUG: Check gpio_base (RP2350 has separate gpiobase register, not in ctrl!)
    printf("PIO1 gpiobase register: %lu\n", (unsigned long)pio1->gpiobase);
    printf("pio_get_gpio_base(pio1): %d\n", pio_get_gpio_base(pio1));

    // Start audio pipeline
    audio_pipeline_start(&pipeline);
    printf("Audio capture started\n\n");

    // Main loop
    uint frame = 0;
    uint buf_idx = 0;
    uint32_t last_status_time = time_us_32();

    while (true) {
        // Generate and output video frame (tight loop, no audio processing)
        // Process audio BEFORE frame (like working code)
        audio_pipeline_process(&pipeline, audio_output_callback, NULL);

        for (int y = 0; y < FRAME_HEIGHT; y++) {
            // Poll audio BEFORE scanline generation (~18µs intervals needed)
            audio_pipeline_process(&pipeline, audio_output_callback, NULL);

            generate_scanline(scanline_buf[buf_idx], y);

            // Poll again after generation
            audio_pipeline_process(&pipeline, audio_output_callback, NULL);

            const uint16_t *scanline = scanline_buf[buf_idx];
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
            buf_idx ^= 1;

            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline));

            // Poll again after queue operations
            audio_pipeline_process(&pipeline, audio_output_callback, NULL);
        }

        frame++;

        // Poll buttons once per frame
        audio_pipeline_poll_buttons(&pipeline);
        audio_pipeline_get_status(&pipeline, &pipeline_status);

        // LED heartbeat
        if ((frame % 60) == 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }

        // Serial status every 2 seconds (single line, overwrite)
        uint32_t now = time_us_32();
        if (now - last_status_time >= 2000000) {
            // Calculate TX drop rate
            static uint32_t last_tx_written = 0;
            static uint32_t last_tx_dropped = 0;
            uint32_t tx_written_delta = tx_samples_written - last_tx_written;
            uint32_t tx_dropped_delta = tx_samples_dropped - last_tx_dropped;
            last_tx_written = tx_samples_written;
            last_tx_dropped = tx_samples_dropped;

            printf("\rCap:%luHz TX:%lu/%lu drop FIFO:%d Buf:%d-%d SRC:%s      ",
                   (unsigned long)pipeline_status.capture_sample_rate,
                   (unsigned long)tx_dropped_delta,
                   (unsigned long)tx_written_delta,
                   pio_sm_get_rx_fifo_level(pio1, 0),
                   tx_buffer_min, tx_buffer_max,
                   src_mode_name(pipeline_status.src_mode));
            fflush(stdout);

            // Reset buffer stats for next period
            tx_buffer_min = 256;
            tx_buffer_max = 0;

            last_status_time = now;
        }

        // Debug: dump raw samples when BTN2 (GP23) is pressed
        static bool btn2_was_pressed = false;
        bool btn2_now = !gpio_get(23);  // Active low
        if (btn2_now && !btn2_was_pressed) {
            printf("\n=== RAW SAMPLE DUMP ===\n");
            // Read directly from PIO FIFO
            for (int i = 0; i < 16 && pio_sm_get_rx_fifo_level(pio1, 0) >= 2; i++) {
                uint32_t raw_l = pio_sm_get(pio1, 0);
                uint32_t raw_r = pio_sm_get(pio1, 0);
                int16_t s_l = (int16_t)(raw_l & 0xFFFF);
                int16_t s_r = (int16_t)(raw_r & 0xFFFF);
                printf("  [%2d] L: 0x%08lX -> %6d  R: 0x%08lX -> %6d\n",
                       i, (unsigned long)raw_l, s_l, (unsigned long)raw_r, s_r);
            }
            printf("======================\n\n");
        }
        btn2_was_pressed = btn2_now;
    }

    return 0;
}

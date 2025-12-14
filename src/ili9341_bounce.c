/**
 * SPI LCD Bouncing Box Demo - PIO Version
 *
 * Targets: TENSTAR ROBOT GMT024-08-SPI8P display (ST7365, 320x480, SPI)
 * Uses PIO for 62.5 MHz SPI output (vs ~25 MHz with hardware SPI)
 *
 * Display: Landscape 480x320, with Neo Geo 320x224 centered (black bars)
 *
 * Pin Configuration:
 *   GPIO 2  - SCK  (SPI Clock via PIO)
 *   GPIO 3  - MOSI (SPI Data via PIO)
 *   GPIO 5  - CS   (Chip Select, GPIO)
 *   GPIO 6  - RST  (Reset, GPIO)
 *   GPIO 7  - D/C  (Data/Command, GPIO)
 *   GPIO 8  - BL   (Backlight, GPIO)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "lcd_pio.pio.h"

// Pin definitions
#define PIN_SCK   2   // SCL - SPI Clock (PIO)
#define PIN_MOSI  3   // SDA - SPI Data (PIO)
#define PIN_CS    5   // CS  - Chip Select (GPIO)
#define PIN_RST   6   // RST - Reset (GPIO)
#define PIN_DC    7   // D/C - Data/Command (GPIO)
#define PIN_BL    8   // BL  - Backlight (GPIO)

// Display dimensions (landscape mode after rotation)
#define DISPLAY_WIDTH  480
#define DISPLAY_HEIGHT 320

// Neo Geo active area (4:3 aspect, centered)
#define ACTIVE_WIDTH   320
#define ACTIVE_HEIGHT  224

// Black bar sizes for centering
#define HBAR_LEFT      ((DISPLAY_WIDTH - ACTIVE_WIDTH) / 2)    // 80 pixels
#define HBAR_RIGHT     (DISPLAY_WIDTH - ACTIVE_WIDTH - HBAR_LEFT)
#define VBAR_TOP       ((DISPLAY_HEIGHT - ACTIVE_HEIGHT) / 2)  // 48 pixels
#define VBAR_BOTTOM    (DISPLAY_HEIGHT - ACTIVE_HEIGHT - VBAR_TOP)

// PIO configuration - 250 MHz sys clock / div / 2 = SPI clock
// ST7365P/ILI9488: official max 20 MHz (50ns min cycle per datasheet)
// div 6.25 = 20 MHz (official max), div 8.0 = 15.6 MHz (conservative)
#define PIO_SPI_CLK_DIV 6.25f  // 20 MHz - ST7365P/ILI9488 official max

// Common LCD commands
#define CMD_SWRESET    0x01
#define CMD_SLPOUT     0x11
#define CMD_NORON      0x13
#define CMD_INVON      0x21
#define CMD_INVOFF     0x20
#define CMD_DISPON     0x29
#define CMD_CASET      0x2A
#define CMD_RASET      0x2B
#define CMD_RAMWR      0x2C
#define CMD_MADCTL     0x36
#define CMD_COLMOD     0x3A

// RGB565 colors (same as DVI test)
#define COLOR_WHITE   0xFFFF
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_GREEN   0x07E0
#define COLOR_MAGENTA 0xF81F
#define COLOR_RED     0xF800
#define COLOR_BLUE    0x001F
#define COLOR_BLACK   0x0000

// Moving box state (same as DVI test)
static int box_x = 50, box_y = 50;
static int box_dx = 2, box_dy = 1;
#define BOX_SIZE 40

// Framebuffer for active area (320x224 = 143,360 bytes)
static uint16_t framebuffer[ACTIVE_WIDTH * ACTIVE_HEIGHT];

// PIO and DMA
static PIO pio = pio0;
static uint sm = 0;
static int dma_chan;

// -----------------------------------------------------------------------------
// PIO SPI functions
// -----------------------------------------------------------------------------

static inline void cs_select() {
    gpio_put(PIN_CS, 0);
}

static inline void cs_deselect() {
    gpio_put(PIN_CS, 1);
}

static void lcd_write_byte(uint8_t data) {
    lcd_spi_put(pio, sm, data);
}

static void lcd_write_cmd(uint8_t cmd) {
    lcd_spi_wait_idle(pio, sm);
    gpio_put(PIN_DC, 0);
    cs_select();
    lcd_write_byte(cmd);
    lcd_spi_wait_idle(pio, sm);
    cs_deselect();
}

static void lcd_write_data(const uint8_t *data, size_t len) {
    gpio_put(PIN_DC, 1);
    cs_select();
    for (size_t i = 0; i < len; i++) {
        lcd_write_byte(data[i]);
    }
    lcd_spi_wait_idle(pio, sm);
    cs_deselect();
}

static void lcd_write_data_byte(uint8_t data) {
    lcd_write_data(&data, 1);
}

// DMA-based framebuffer transfer via PIO
static void lcd_send_framebuffer_dma(const uint8_t *data, size_t len) {
    gpio_put(PIN_DC, 1);
    cs_select();

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);

    dma_channel_configure(dma_chan, &c,
                          &pio->txf[sm],    // Write to PIO TX FIFO
                          data,              // Read from framebuffer
                          len,               // Transfer count
                          true);             // Start immediately

    dma_channel_wait_for_finish_blocking(dma_chan);
    lcd_spi_wait_idle(pio, sm);
    cs_deselect();
}

// -----------------------------------------------------------------------------
// Display initialization
// -----------------------------------------------------------------------------

static void lcd_init(void) {
    // Hardware reset
    gpio_put(PIN_RST, 1);
    sleep_ms(50);
    gpio_put(PIN_RST, 0);
    sleep_ms(100);
    gpio_put(PIN_RST, 1);
    sleep_ms(150);

    // Software reset
    lcd_write_cmd(CMD_SWRESET);
    sleep_ms(150);

    // Exit sleep mode
    lcd_write_cmd(CMD_SLPOUT);
    sleep_ms(150);

    // Set color mode to 16-bit (RGB565)
    lcd_write_cmd(CMD_COLMOD);
    lcd_write_data_byte(0x55);

    // Memory access control (landscape)
    lcd_write_cmd(CMD_MADCTL);
    lcd_write_data_byte(0x60);

    // Inversion off
    lcd_write_cmd(CMD_INVOFF);

    // Normal display mode
    lcd_write_cmd(CMD_NORON);
    sleep_ms(10);

    // Display ON
    lcd_write_cmd(CMD_DISPON);
    sleep_ms(100);
}

// -----------------------------------------------------------------------------
// Drawing functions
// -----------------------------------------------------------------------------

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_write_cmd(CMD_CASET);
    uint8_t caset[] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    lcd_write_data(caset, 4);

    lcd_write_cmd(CMD_RASET);
    uint8_t raset[] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_write_data(raset, 4);

    lcd_write_cmd(CMD_RAMWR);
}

// Pre-swapped colors for faster rendering
#define SWAP_COLOR_WHITE   0xFFFF
#define SWAP_COLOR_YELLOW  0xE0FF
#define SWAP_COLOR_CYAN    0xFF07
#define SWAP_COLOR_GREEN   0xE007
#define SWAP_COLOR_MAGENTA 0x1FF8
#define SWAP_COLOR_RED     0x00F8
#define SWAP_COLOR_BLUE    0x1F00
#define SWAP_COLOR_BG1     0x0842
#define SWAP_COLOR_BG2     0x0421

// Generate entire frame into framebuffer
static void generate_frame(uint frame) {
    uint16_t *fb = framebuffer;

    for (uint y = 0; y < ACTIVE_HEIGHT; y++) {
        if (y < 4) {
            uint16_t c = (y == 0) ? SWAP_COLOR_RED : (y == 1) ? SWAP_COLOR_GREEN :
                         (y == 2) ? SWAP_COLOR_BLUE : SWAP_COLOR_YELLOW;
            for (uint x = 0; x < ACTIVE_WIDTH; x++) {
                *fb++ = c;
            }
            continue;
        }

        if (y >= ACTIVE_HEIGHT - 4) {
            uint16_t c = (y == ACTIVE_HEIGHT-4) ? SWAP_COLOR_CYAN :
                         (y == ACTIVE_HEIGHT-3) ? SWAP_COLOR_MAGENTA :
                         (y == ACTIVE_HEIGHT-2) ? SWAP_COLOR_WHITE : SWAP_COLOR_RED;
            for (uint x = 0; x < ACTIVE_WIDTH; x++) {
                *fb++ = c;
            }
            continue;
        }

        uint y_checker = (y / 20 + frame / 30) & 1;
        bool in_box_y = (y >= box_y && y < box_y + BOX_SIZE);

        for (uint x = 0; x < ACTIVE_WIDTH; x++) {
            if (in_box_y && x >= box_x && x < box_x + BOX_SIZE) {
                *fb++ = SWAP_COLOR_WHITE;
            } else {
                uint checker = ((x / 20) + y_checker) & 1;
                *fb++ = checker ? SWAP_COLOR_BG1 : SWAP_COLOR_BG2;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    // Overclock to 250 MHz for faster PIO SPI
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(250000, true);

    stdio_init_all();
    sleep_ms(1000);

    printf("\n====================================\n");
    printf("SPI LCD Demo - PIO Version\n");
    printf("Display: %dx%d (ST7365)\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    printf("Neo Geo: %dx%d centered\n", ACTIVE_WIDTH, ACTIVE_HEIGHT);
    printf("System clock: %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    printf("====================================\n");

    // Initialize LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Initialize GPIO pins
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);

    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 1);

    gpio_init(PIN_BL);
    gpio_set_dir(PIN_BL, GPIO_OUT);
    gpio_put(PIN_BL, 1);

    // Initialize PIO for SPI
    uint offset = pio_add_program(pio, &lcd_spi_program);
    lcd_spi_program_init(pio, sm, offset, PIN_MOSI, PIN_SCK, PIO_SPI_CLK_DIV);

    // Calculate actual SPI clock
    float pio_clk = clock_get_hz(clk_sys) / PIO_SPI_CLK_DIV / 2.0f;  // /2 for 2 instructions per bit
    printf("PIO SPI clock: %.1f MHz\n", pio_clk / 1000000.0f);

    // Initialize DMA
    dma_chan = dma_claim_unused_channel(true);
    printf("DMA channel: %d\n", dma_chan);

    // Initialize display
    lcd_init();
    printf("Display initialized\n");

    // Clear entire screen to black once
    printf("Clearing screen...\n");
    lcd_set_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    gpio_put(PIN_DC, 1);
    cs_select();
    for (uint i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        lcd_write_byte(0x00);
        lcd_write_byte(0x00);
    }
    lcd_spi_wait_idle(pio, sm);
    cs_deselect();
    printf("Screen cleared\n");

    uint32_t frame = 0;
    uint32_t last_time = to_ms_since_boot(get_absolute_time());

    while (true) {
        // Update box position
        box_x += box_dx;
        box_y += box_dy;
        if (box_x <= 0 || box_x >= ACTIVE_WIDTH - BOX_SIZE) box_dx = -box_dx;
        if (box_y <= 0 || box_y >= ACTIVE_HEIGHT - BOX_SIZE) box_dy = -box_dy;

        // Generate frame
        generate_frame(frame);

        // Set window to active area only
        lcd_set_window(HBAR_LEFT, VBAR_TOP, HBAR_LEFT + ACTIVE_WIDTH - 1, VBAR_TOP + ACTIVE_HEIGHT - 1);

        // Send framebuffer via DMA
        lcd_send_framebuffer_dma((uint8_t *)framebuffer, ACTIVE_WIDTH * ACTIVE_HEIGHT * 2);

        frame++;

        if (frame % 60 == 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);

            uint32_t now = to_ms_since_boot(get_absolute_time());
            uint32_t elapsed = now - last_time;
            if (elapsed > 0) {
                printf("FPS: %lu\n", (unsigned long)(60 * 1000 / elapsed));
            }
            last_time = now;
        }
    }

    return 0;
}

/**
 * DVI Video Output Module
 * Handles DVI serialization and scanline callbacks on Core 1
 */

#include "video_output.h"
#include "hardware_config.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "pico/multicore.h"
#include "hardware/irq.h"

// DVI timing - 480p for HDMI audio support
#define DVI_TIMING dvi_timing_640x480p_60hz

static struct dvi_inst dvi0;
static uint16_t *g_framebuffer = NULL;
static uint g_frame_width = 0;
static uint g_frame_height = 0;

// =============================================================================
// DVI Scanline Callback - runs independently on Core 1's timing
// =============================================================================

static void core1_scanline_callback(uint line_num) {
    (void)line_num;  // We track internally for line doubling

    // Discard any scanline pointers passed back
    uint16_t *bufptr;
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
        ;

    // Track current DVI line (0-479 for 480p)
    static uint dvi_line = 4;  // First two framebuffer lines pushed = 4 DVI lines

    // Each framebuffer line shown twice (line doubling for 480p)
    uint scanline = dvi_line / 2;

    // Return pointer to current row in framebuffer
    bufptr = &g_framebuffer[g_frame_width * scanline];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    dvi_line = (dvi_line + 1) % 480;  // 480p = 480 DVI lines
}

// =============================================================================
// Core 1: DVI Output
// =============================================================================

static void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
    __builtin_unreachable();
}

// =============================================================================
// Public API
// =============================================================================

void video_output_init(uint16_t *framebuffer, uint frame_width, uint frame_height) {
    g_framebuffer = framebuffer;
    g_frame_width = frame_width;
    g_frame_height = frame_height;

    // Initialize DVI with scanline callback
    neopico_dvi_gpio_setup();
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Push first two scanlines to start DVI
    uint16_t *bufptr = g_framebuffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    bufptr += g_frame_width;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
}

void video_output_start(void) {
    multicore_launch_core1(core1_main);
}

struct dvi_inst* video_output_get_dvi(void) {
    return &dvi0;
}

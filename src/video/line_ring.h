#ifndef LINE_RING_H
#define LINE_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/sync.h"

// Line buffer configuration
#define LINE_RING_SIZE 256      // Full frame buffer for stability
#define LINE_WIDTH 320
#define LINES_PER_FRAME 224     // MVS active lines

typedef struct {
    uint16_t lines[LINE_RING_SIZE][LINE_WIDTH];  // ~25KB line buffer

    // Core 0 (producer) state
    volatile uint32_t write_idx;        // Global write position (lines written total)
    volatile uint32_t frame_base_idx;   // Global index where current frame starts

    // Core 1 (consumer) state
    volatile uint32_t read_frame_start; // Global index of display frame start

    // Resync flag - Core 0 requests, Core 1 executes
    volatile bool resync_pending;
} line_ring_t;

extern line_ring_t g_line_ring;

// ============================================================================
// Core 0 API (Producer) - Input capture side
// ============================================================================

// Called at input VSYNC - request HSTX resync
static inline void line_ring_vsync(void) {
    // Mark start of new frame
    g_line_ring.frame_base_idx = g_line_ring.write_idx;
    __dmb();
    // Request Core 1 to resync HSTX
    g_line_ring.resync_pending = true;
}

// Get write pointer for line N within current frame
static inline uint16_t* line_ring_write_ptr(uint16_t line) {
    uint32_t idx = g_line_ring.frame_base_idx + line;
    return g_line_ring.lines[idx % LINE_RING_SIZE];
}

// Signal that lines 0..(total_lines-1) of current frame are written
static inline void line_ring_commit(uint16_t total_lines) {
    __dmb();  // Ensure line data visible before updating index
    g_line_ring.write_idx = g_line_ring.frame_base_idx + total_lines;
}

// ============================================================================
// Core 1 API (Consumer) - HDMI output side
// ============================================================================

// Check if resync is requested (called from DMA ISR)
// Only clears the flag - actual sync happens at output VSYNC via line_ring_output_vsync()
static inline bool line_ring_should_resync(void) {
    if (g_line_ring.resync_pending) {
        g_line_ring.resync_pending = false;
        return true;
    }
    return false;
}

// Called at output VSYNC (when not resyncing)
static inline void line_ring_output_vsync(void) {
    // Sync to current input frame
    g_line_ring.read_frame_start = g_line_ring.frame_base_idx;
    __dmb();
}

// Check if line is ready and still in buffer
static inline bool line_ring_ready(uint16_t line) {
    uint32_t target_idx = g_line_ring.read_frame_start + line;
    uint32_t write_pos = g_line_ring.write_idx;

    // Line must have been written
    if (target_idx >= write_pos) {
        return false;  // Not written yet
    }

    // Line must still be in buffer (not overwritten)
    if (write_pos - target_idx > LINE_RING_SIZE) {
        return false;  // Buffer overrun - data was overwritten
    }

    return true;
}

// Get read pointer for line N in current display frame
static inline const uint16_t* line_ring_read_ptr(uint16_t line) {
    __dmb();  // Ensure we see latest committed data
    uint32_t target_idx = g_line_ring.read_frame_start + line;
    return g_line_ring.lines[target_idx % LINE_RING_SIZE];
}

#endif // LINE_RING_H

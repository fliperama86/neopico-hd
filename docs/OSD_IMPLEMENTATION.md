# OSD (On-Screen Display) Implementation

NeoPico-HD implements a lightweight, pixel-accurate OSD designed to overlay system information and menus without interfering with the captured video stream.

## Architecture

### 1. Separate Layer Strategy

To maintain the integrity of the captured Neo Geo video signal, the OSD is **not** drawn into the main 320x240 RGB565 framebuffer. Instead, it resides in its own memory space:

- **Resolution**: 320x240 (matching internal video resolution).
- **Format**: 1bpp (Monochrome) or 2bpp (4-color palette).
- **Memory Footprint**:
  - 1bpp: ~9.6 KB
  - 2bpp: ~19.2 KB
- **Placement**: Managed entirely on **Core 1**.

### 2. Core 1 "Injection" Rendering

The OSD is overlaid in real-time during the horizontal pixel doubling phase in the `dma_irq_handler` on Core 1.

As pixels are read from the RGB565 framebuffer to be doubled for 480p output, the ISR checks the corresponding bit in the OSD buffer. If the OSD bit is set, the OSD color is substituted for the video pixel.

```c
// Conceptual doubling + overlay loop
for (int i = 0; i < 320; i++) {
    uint16_t pixel = frame_buffer[i];
    if (osd_active && (osd_buffer[line_offset] & (1 << (i % 32)))) {
        pixel = OSD_TEXT_COLOR;
    }
    line_buffer[i*2] = pixel;
    line_buffer[i*2 + 1] = pixel;
}
```

## UI & Aesthetics

- **Font**: Standard 8x8 pixel-art font (consistent with 1990s arcade aesthetic).
- **Scaling**: OSD pixels are 1:1 with captured Neo Geo pixels (320x240), then doubled by the output hardware to 640x480.
- **Transparency**: Any OSD pixel not set to "active" remains transparent, showing the game video underneath.

## Navigation & Logic

- **State Machine**: The menu is driven by a simple state machine (e.g., `OSD_HIDDEN`, `OSD_MAIN_MENU`, `OSD_SETTINGS`).
- **Input**: Menu navigation is handled via board buttons or defined controller combos, processed in the main loop and flagged to Core 1.

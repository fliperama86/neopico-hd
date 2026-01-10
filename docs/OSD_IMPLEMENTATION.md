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

## Critical Timing Constraints (Core 1)

The OSD injection logic runs inside the **Core 1 DMA ISR**, which is the most time-critical part of the firmware. 

### 1. The H-Blank Deadline
The HSTX hardware consumes data at 25.2 MHz. The ISR must finish processing a complete scanline (640 pixels) within the horizontal blanking interval of the previous line. If the ISR execution time exceeds this window, the HSTX FIFO will underrun, causing the HDMI signal to drop (No Signal).

### 2. Implementation Guidelines
To maintain a stable signal, future OSD or scaling logic MUST adhere to these rules:

*   **Avoid Per-Pixel Branching**: NEVER use `if` or `switch` statements inside the inner pixel loop. Doing so adds comparison and branching overhead 640 times per line, which is enough to break HDMI sync.
*   **Loop Splitting**: If an overlay is only present on a specific part of the screen, split the processing into three sequential loops (Before-OSD, OSD-Area, After-OSD). This allows the CPU to perform high-speed sequential memory copies without logic overhead.
*   **Scratch X Usage**: All code called by the scanline doubler must be marked with `__scratch_x("")` to ensure it runs from zero-wait-state RAM.
*   **Minimize Memory Access**: Always process pixels in 32-bit words (2 pixels at a time) whenever possible to maximize bus bandwidth.

## UI & Aesthetics

- **Font**: Standard 8x8 pixel-art font (consistent with 1990s arcade aesthetic).
- **Scaling**: OSD pixels are 1:1 with captured Neo Geo pixels (320x240), then doubled by the output hardware to 640x480.
- **Transparency**: Any OSD pixel not set to "active" remains transparent, showing the game video underneath.

## Navigation & Logic

- **State Machine**: The menu is driven by a simple state machine (e.g., `OSD_HIDDEN`, `OSD_MAIN_MENU`, `OSD_SETTINGS`).
- **Input**: Menu navigation is handled via board buttons or defined controller combos, processed in the main loop and flagged to Core 1.

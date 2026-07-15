# OSD (On-Screen Display) Implementation

NeoPico-HD implements a lightweight, pixel-accurate OSD designed to overlay system information and menus without interfering with the captured video stream.

The root-menu title includes the firmware version from CMake's single
`project(... VERSION ...)` definition, for example `NeoPico-HD v0.9.1`.

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

### 3. Fake-Translucent Background

`NEOPICO_OSD_FAKE_BLEND=ON` enables a compile-time-gated panel effect. It is not a general alpha compositor. OSD background pixels (`OSD_COLOR_BG`, black) retain 12.5% of the captured RGB565 game pixels underneath, producing an effective 87.5% black-panel opacity, while nonblack text and icon pixels remain fully opaque.

The implementation handles two RGB565 pixels per iteration and selects the dimmed game or opaque OSD value without a data-dependent inner-loop branch. Dedicated 2x, 3x, and 4x blend-and-scale kernels run from scratch Y; the scanline callback selects the appropriate kernel before processing the OSD span. The no-capture fallback remains an opaque OSD over the fallback color.

The effect is enabled by default. Disable it for an opaque panel when
configuring the firmware:

```sh
cmake -S . -B build-opaque-osd -DNEOPICO_OSD_FAKE_BLEND=OFF
```

The option defaults to `ON`, requires `NEOPICO_ENABLE_OSD=ON`, and remains
incompatible with `NEOPICO_EXP_PRECOMPOSED_HDMI`. Precomposed builds must set
`NEOPICO_OSD_FAKE_BLEND=OFF` explicitly.

## Critical Timing Constraints (Core 1)

The OSD injection logic runs inside the **Core 1 DMA ISR**, which is the most time-critical part of the firmware.

### 1. The H-Blank Deadline
The HSTX hardware consumes data at 25.2 MHz. The ISR must finish processing a complete scanline (640 pixels) within the horizontal blanking interval of the previous line. If the ISR execution time exceeds this window, the HSTX FIFO will underrun, causing the HDMI signal to drop (No Signal).

### 2. Implementation Guidelines
To maintain a stable signal, future OSD or scaling logic MUST adhere to these rules:

*   **Avoid Per-Pixel Branching**: NEVER use `if` or `switch` statements inside the inner pixel loop. Doing so adds comparison and branching overhead 640 times per line, which is enough to break HDMI sync.
*   **Loop Splitting**: If an overlay is only present on a specific part of the screen, split the processing into three sequential loops (Before-OSD, OSD-Area, After-OSD). This allows the CPU to perform high-speed sequential memory copies without logic overhead.
*   **Scratch RAM Placement**: Keep the scanline callback in scratch X. Dedicated hot helper kernels may use scratch Y when their placement and section budgets are audited; never leave scanline-critical code in flash.
*   **Minimize Memory Access**: Always process pixels in 32-bit words (2 pixels at a time) whenever possible to maximize bus bandwidth.
*   **Fixed Fake Blend Only**: Keep the experimental panel effect branch-free inside the packed-pixel loop. Do not add arbitrary alpha levels or per-pixel conditionals to the scanline path.

## UI & Aesthetics

- **Font**: Standard 8x8 pixel-art font (consistent with 1990s arcade aesthetic).
- **Scaling**: OSD pixels are 1:1 with captured Neo Geo pixels (320x240), then doubled by the output hardware to 640x480.
- **Background treatment**: The default fake-blend path dims game pixels under black OSD background while nonblack OSD pixels remain opaque. Set `NEOPICO_OSD_FAKE_BLEND=OFF` for a fully opaque panel.

## Navigation & Logic

- **State Machine**: The menu is driven by a simple state machine (e.g., `OSD_HIDDEN`, `OSD_MAIN_MENU`, `OSD_SETTINGS`).
- **Input**: Menu navigation is handled via board buttons or defined controller combos, with all menu state and drawing processed on Core 1.
- **Controller Mapping**: Controller inputs default to ON for MVS/AES builds and OFF for SNES builds; either can be overridden with `NEOPICO_OSD_CONTROLLER_INPUTS`. The active-low inputs use weak internal pull-ups and map START=GP0, SELECT=GP1, UP=GP3, and DOWN=GP2. UP+SELECT opens the hidden OSD, UP/DOWN moves and wraps the selection, START confirms, and SELECT returns or cancels.
- **Physical Buttons**: GP25 MENU and GP26 BACK retain the legacy two-button confirm/cycle behavior.

### MVS Colors Selector

`NEOPICO_MVS_COLOR_MODEL_MENU=ON` adds a persistent `Colors` leaf to the
root menu. It is enabled in the MVS release asset and defaults to `OFF` for
custom builds. It has two values and no `Off` value:

- `Digital`: `Exact RGB555 mapping`.
- `Analog`: `Models NEOGEO DAC levels`.

Only the selected option's description line is displayed. No additional status
or effect text is shown. The build system still rejects combining this menu
with `NEOPICO_ENABLE_DARK_SHADOW=ON`.

UP/DOWN and the physical BACK button move the highlight and request an immediate
preview. Core 0 reads the request once at the next input VSYNC and selects one
LUT base pointer for the entire frame, so a frame cannot mix models. Controller
SELECT restores the model that was committed when the Colors leaf was opened
and returns to the root menu. Controller START or the physical MENU button
commits the preview. The `*` marker identifies the committed choice. No firmware
reset is requested.

Core 0 generates both 32K-entry RGB565 tables at boot. They occupy 128 KiB,
which is 64 KiB more than the feature-off build. Pointer selection happens once
per frame; the active-pixel loop still performs one halfword LUT read per pixel
and contains no model branch.

Confirmation also queues the settings record from Core 1. Core 0 writes it only
after a complete input frame, or after the no-signal timeout, then resets the
capture PIO state for a clean resynchronization. The entire menu build is forced
to run from SRAM, allowing Core 1 HDMI interrupts to continue while flash XIP is
temporarily unavailable. The displayed game frame can be held briefly during
the rare flash write, but the firmware does not reboot. This live persistence
path remains experimental until hardware validation. Further menu actions are
ignored while the write is pending. Factory reset restores `Digital`.

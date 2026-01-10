# NeoPico-HD Developer Best Practices

A quick reference guide for maintaining stability and performance in the NeoPico-HD codebase.

## 🟢 DO's

### Timing & Performance
*   **DO Split Loops**: When implementing OSD or overlays, split the pixel loop into sequential sections (Before, Inside, After) instead of using `if` checks per pixel.
*   **DO Use Scratch Memory**: Mark critical interrupt handlers (video/audio) with `__scratch_x("")` or `__scratch_y("")` to run them from zero-wait-state RAM.
*   **DO Process in Batches**: When manipulating pixels, read/write 32-bit words (2 pixels) whenever possible to maximize bus efficiency.

### Hardware Interaction
*   **DO Disable Pulls**: Always call `gpio_disable_pulls()` on input pins to avoid the RP2350-E9 "sticky pin" erratum.
*   **DO Use `WAIT PIN`**: In PIO programs, prefer `wait pin` over `wait gpio` to avoid issues with Bank 1 relative addressing offsets.
*   **DO Respect Bank 1**: Remember that GPIOs 25-47 live in Bank 1 and require special handling (like `GPIOBASE` offsets) compared to Bank 0.

### Audio & Sync
*   **DO Monitor Buffers**: Rely on the feedback loop (buffer fill level) to manage audio sync, rather than trusting static clock calculations.
*   **DO Clamp Counters**: When implementing time-based logic, always clamp or wrap accumulators to prevent overflow during long runtimes.

---

## 🔴 DON'Ts

### Timing & Performance
*   **DON'T Branch in Loops**: Never place an `if` or `switch` statement inside the inner 640-pixel scanline loop. It *will* break HDMI sync.
*   **DON'T Block Interrupts**: Never perform long operations (like `printf` or heavy math) inside the DMA or PIO interrupts.
*   **DON'T Assume 60Hz**: Do not assume the input video is exactly 60.00Hz. The Neo Geo clock drifts; always use the measured sync signals.

### Memory & Resources
*   **DON'T Mix Cores**: Do not access Core 0 resources (like the PIO1 capture state) from Core 1 without careful synchronization.
*   **DON'T Use Floating Point**: Avoid floating-point math in critical paths (ISR) as it is significantly slower than fixed-point arithmetic on this MCU.

### Stability
*   **DON'T Trust Default Init**: Don't rely on `gpio_init()` alone for input pins; it enables pulls by default which can cause voltage latching on 5V-tolerant inputs.
*   **DON'T Forget Ground**: Never test video capture without a solid ground connection between the Pico and the MVS. Digital noise will look like software bugs.

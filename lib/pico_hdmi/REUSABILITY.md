# pico_hdmi Reusability Roadmap

This document outlines the planned architectural changes to transition `pico_hdmi` from a firmware-specific component to a generic, reusable library for any RP2350 project requiring HSTX-based DVI/HDMI output.

## Priority 1: High - Decouple Pixel Source (Provider Pattern)

Currently, the library's DMA ISR assumes a specific 320x240 RGB565 framebuffer and performs hardcoded line doubling.

### Planned Change:

- **Remove** the global `framebuf` pointer from the library.
- **Implement** a scanline callback mechanism:
  ```c
  typedef void (*video_output_fill_fn)(uint16_t *line_buffer, uint32_t line_num);
  ```
- The library will provide a double-buffered internal line buffer (640 pixels).
- The ISR will call the user-provided function to fill the "next" buffer while the current one is being sent via DMA.

## Priority 2: High - Parameterize HDMI Metadata

ACR (Audio Clock Regeneration) and InfoFrames are currently hardcoded for a specific resolution and audio sample rate.

### Planned Change:

- Define a `video_output_config_t` structure:
  ```c
  typedef struct {
      uint32_t audio_sample_rate; // 32000, 44100, 48000
      uint8_t vic;                // Video Identification Code (e.g., 1 for 640x480)
      // ... other settings
  } video_output_config_t;
  ```
- Update `video_output_init()` to accept this configuration.

## Priority 3: Medium - Generalize HSTX Expander Configuration

The library currently hard-configures the HSTX hardware for RGB565 input.

### Planned Change:

- Add an enumeration for supported input formats:
  - `DVI_FORMAT_RGB565`
  - `DVI_FORMAT_RGB555`
  - `DVI_FORMAT_RGB888` (requires different expander settings)
- Allow the user to select the format during initialization to dynamically set the `expand_tmds` and `expand_shift` registers.

## Priority 4: Medium - Dynamic Resource Management

The library currently hard-claims DMA channels 0 and 1 and IRQ 0.

### Planned Change:

- Use `dma_claim_unused_channel()` at runtime to avoid conflicts with other libraries (like WiFi or SD card drivers).
- Store the assigned channel numbers in a internal state structure.

## Priority 5: Low - Resolution Independence

The timing constants (front porch, sync width, etc.) are currently static macros.

### Planned Change:

- Move timing constants into a `dvi_timing_t` lookup table.
- Allow users to select standard profiles (e.g., `DVI_TIMING_640x480_60HZ`) or provide custom timings for non-standard displays.

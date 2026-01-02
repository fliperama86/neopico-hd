⏺ Key Differences:

hstx_lab:

- 126 MHz system clock
- Single-core (all on Core 0)
- HSTX hardware block for DVI (no PIO)
- 8-bit RGB332 framebuffer
- Synthetic audio (440Hz sine)

neopico-hd:

- 252 MHz system clock (2x faster)
- Dual-core (Core 0: capture/audio, Core 1: DVI rendering)
- PicoDVI (PIO-based) - allocates PIO0=DVI, PIO1=video, PIO2=audio
- 16-bit RGB565 framebuffer
- I2S audio capture from MVS hardware

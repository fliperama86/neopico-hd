# pico_hdmi Examples

Example projects demonstrating the pico_hdmi HDMI output library for RP2350.

## bouncing_box

Simple animated demo showing:
- 640x480 @ 60Hz HDMI output
- Scanline callback rendering
- 2x scaling from 320x240 framebuffer
- Basic animation loop

### Building

```bash
cd bouncing_box
mkdir build && cd build
cmake ..
make
```

Flash the resulting `bouncing_box.uf2` to your Pico 2.

### Hardware

Requires HSTX pins connected to an HDMI connector:
- GPIO 12-13: Clock pair
- GPIO 14-15: Data 0 (Blue)
- GPIO 16-17: Data 1 (Green)
- GPIO 18-19: Data 2 (Red)

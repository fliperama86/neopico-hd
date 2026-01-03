Instructions for AI agents when working with this repository.

## Architecture Context

NeoPico-HD is a pure digital Neo Geo MVS capture system using the RP2350B.

- **Core 0**: Handles pixel-perfect video capture (PIO+DMA).
- **Core 1**: Dedicated to HSTX HDMI output at 640x480 (2x scaled from 320x240).
- **Hardware**: Specifically designed for the WeAct RP2350B board using both Bank 0 and Bank 1 GPIOs.

## High-Priority Commands

```bash
# Build & Flash
./scripts/build.sh && ./flash

# Serial Log Monitor
screen /dev/tty.usbmodem* 115200
```

## Maintenance Guidelines

### Audio Pipeline

- **I2S Format**: Right-justified, WS High = Left, BCK Rising Edge.
- **Sample Rate**: Input ~55.5kHz, Output 48kHz (SRC required).
- **HDMI Audio**: Transmitted via Data Islands during blanking intervals.

### Video Capture

- **Sync**: Self-synchronizes to CSYNC falling edge per line.
- **DMA**: Uses a ping-pong scheme. Do NOT block Core 0 for long periods during active frame capture.

### HSTX Output

- **Clock**: 126 MHz system clock, 25.2 MHz pixel clock (126/5).
- **Format**: 640x480 @ 60Hz with RGB565 framebuffer (320x240 doubled 2x2).
- **Audio**: HDMI Data Islands with 48kHz stereo.

### RP2350B Pin Mapping

- **Audio**: GPIO 0-2 (PIO2)
- **Video**: GPIO 25-43 (PIO1)
- **HSTX**: GPIO 12-19 (hardware TMDS encoder)

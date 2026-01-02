Instructions for AI agents when working with this repository.

## Architecture Context

NeoPico-HD is a pure digital Neo Geo MVS capture system using the RP2350B.

- **Core 0**: Handles pixel-perfect video capture (PIO+DMA) and digital audio processing (I2S+SRC).
- **Core 1**: Dedicated to the PicoDVI stack, rendering 240p video doubled to 480p for HDMI compatibility.
- **Hardware**: Specifically designed for the WeAct RP2350B board using both Bank 0 and Bank 1 GPIOs.

## High-Priority Commands

```bash
# Full Build & Flash
./scripts/build.sh && ./scripts/flash.sh

# Debug OSD Toggle (in src/main.c)
#define DEBUG_AUDIO_INFO 1

# Serial Log Monitor
screen /dev/tty.usbmodem* 115200
```

## Maintenance Guidelines

### Audio Pipeline

- **I2S Format**: Right-justified, WS High = Left, BCK Rising Edge.
- **Sample Rate**: Input ~55.5kHz, Output 48kHz (SRC required).
- **Polling**: Must call `audio_pipeline_process` frequently to avoid ring buffer overflow.

### Video Capture

- **Sync**: Self-synchronizes to CSYNC falling edge per line.
- **DMA**: Uses a ping-pong scheme. Do NOT block Core 0 for long periods during active frame capture.

### Diagnostics

- Use the built-in OSD for debugging. Key metrics: `LRCK MEAS` (should be 55555Hz), `PIO PC` (should be changing), and `DMA ADDR` (should be moving).
- Don't use "cat" or "screen" to capture USB logs as they can get stuck waiting forever if the device doesn't respond, instead, use a Python script with timeout.

### RP2350B Pin Mapping

- **Audio**: GPIO 0-2 (PIO2)
- **Video**: GPIO 25-43 (PIO1)
- **DVI**: GPIO 12-19 (PIO0)

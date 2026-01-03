Instructions for AI agents when working with this repository.

## Architecture Overview

NeoPico-HD is a pure digital Neo Geo MVS capture system using the RP2350B.

- **Core 0**: Handles pixel-perfect video capture (PIO1+DMA). Runs the main coordination loop.
- **Core 1**: Dedicated to HSTX HDMI output and the audio pipeline (polling PIO2, SRC, and TERC4 encoding).
- **Hardware**: Designed for the WeAct RP2350B board using both Bank 0 and Bank 1 GPIOs.

## Essential Documentation

Agents MUST refer to these for implementation details:

- `README.md`: High-level overview and pin mapping.
- `docs/HDMI_HSTX_IMPLEMENTATION.md`: HDMI protocol, sync polarity, and Data Island details.
- `docs/MVS_MV1C_DIGITAL_VIDEO.md`: MVS video tap points and PIO capture logic (Bank 1).
- `docs/MVS_MV1C_DIGITAL_AUDIO.md`: MVS audio tap points and I2S/SRC pipeline.

## Maintenance Guidelines

### Audio Pipeline

- **I2S Format**: Right-justified, WS High = Left, BCK Rising Edge.
- **Sample Rate**: Input ~55.5kHz, Output 48kHz (SRC required).
- **HDMI Audio**: MUST use **Validity Bit = 0**.
- **Location**: All audio processing MUST run on **Core 1** to prevent jitter on Core 0.

### Video Capture

- **Sync**: Self-synchronizes to CSYNC falling edge per line.
- **Priority**: Core 0 should ONLY handle capture to ensure rock-solid sync.

### HSTX Output

- **Timing**: MUST be exactly **800 cycles per line**.
- **Mode**: Uses hardware TMDS encoding for video and RAW for Data Islands.

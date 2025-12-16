# YM2610 Audio Logic Analyzer

Simple logic analyzer for debugging Neo Geo MVS audio capture.

## Purpose

Captures and analyzes BCK (bit clock), DAT (serial data), and WS (word select) signals from the YM2610 audio chip to validate hardware wiring and signal integrity.

## Pin Configuration

| GPIO | Signal | Source | Description |
|------|--------|--------|-------------|
| 21 | BCK | YM2610 pin 5 (øS) | Bit clock, ~1.78 MHz |
| 23 | DAT | YM2610 pin 8 (OPO) | Serial audio data |
| 24 | WS | YM3016 pin 3 (SH1) | Word select, ~55.5 kHz |

## Building

```bash
# Set SDK path
export PICO_SDK_PATH=/path/to/pico-sdk

# Build
./build.sh

# Flash to Pico
picotool load -f build/audio_logic_analyzer.uf2
```

## Usage

### Interactive Mode (via Serial)

Connect to Pico serial port (115200 baud):

```bash
screen /dev/tty.usbmodem* 115200
```

Commands:
- `C` - Capture and analyze signals
- `A` - Re-analyze last capture
- `T` - Show timing pattern (first 256 samples)
- `B` - Stream binary data (for Python viewer)
- `R` - Raw GPIO polling (slow, for stuck signals)
- `H` - Help

### Python Viewer

```bash
# Install dependencies
pip install pyserial matplotlib numpy

# Capture and view
python viewer.py

# Or specify port
python viewer.py /dev/tty.usbmodem12345

# Save capture to file
python viewer.py -s capture.bin

# Load and view saved capture
python viewer.py -f capture.bin

# Save plot to file
python viewer.py -o signals.png
```

## Expected Results

For a working YM2610 connection:

| Signal | Expected Frequency | Notes |
|--------|-------------------|-------|
| BCK | ~1776 kHz | 55.5 kHz × 32 bits |
| WS | ~55.5 kHz | Sample rate |
| DAT | varies | Should have transitions |

### BCK/WS Ratio

The ratio of BCK to WS frequency should be ~32 (32 bits per audio sample).

## Troubleshooting

### No BCK transitions
- Check wiring to YM2610 pin 5 (øS)
- Verify MVS is powered on with game running

### No WS transitions
- Check wiring to YM3016 pin 3 (SH1)
- WS comes from DAC, ensure it's connected

### Very slow frequencies
- Might be picking up noise instead of actual signal
- Check ground connection
- Verify you're connected to the right pins

### BCK/WS ratio not 32
- Signals may be swapped
- One signal might be from wrong source

#!/bin/bash
# Build the audio logic analyzer firmware

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "Building Audio Logic Analyzer..."
echo "================================"

# Check PICO_SDK_PATH
if [ -z "$PICO_SDK_PATH" ]; then
    echo "Error: PICO_SDK_PATH not set"
    echo "Please run: export PICO_SDK_PATH=/path/to/pico-sdk"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo "Configuring..."
cmake .. -DPICO_BOARD=pico2

# Build
echo "Building..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo ""
echo "Build complete!"
echo "Firmware: $BUILD_DIR/audio_logic_analyzer.uf2"
echo ""
echo "To flash:"
echo "  1. Hold BOOTSEL and plug in Pico"
echo "  2. cp $BUILD_DIR/audio_logic_analyzer.uf2 /Volumes/RP2350"
echo ""
echo "Or use picotool:"
echo "  picotool load -f $BUILD_DIR/audio_logic_analyzer.uf2"

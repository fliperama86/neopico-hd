#!/bin/bash

set -e

echo "=== Raspberry Pi Pico Deployment Script ==="

# Build the project
echo "Building project..."
if [ ! -d "build" ]; then
    mkdir build
    cd build
    cmake ..
    cd ..
fi

cd build
make -j$(sysctl -n hw.ncpu)
cd ..

echo "Build complete!"

# Check if UF2 file exists
UF2_FILE="build/src/freq_counter.uf2"
if [ ! -f "$UF2_FILE" ]; then
    echo "Error: UF2 file not found at $UF2_FILE"
    exit 1
fi

echo "UF2 file found: $UF2_FILE"

# Find the Pico mount point
PICO_MOUNT="/Volumes/RPI-RP2"

if [ ! -d "$PICO_MOUNT" ]; then
    echo ""
    echo "Error: Pico not found at $PICO_MOUNT"
    echo "Please:"
    echo "  1. Connect your Pico while holding the BOOTSEL button"
    echo "  2. Or unplug it, hold BOOTSEL, and plug it back in"
    echo "  3. Make sure it appears as a USB drive named RPI-RP2"
    exit 1
fi

echo "Pico found at: $PICO_MOUNT"

# Copy the UF2 file to the Pico
echo "Copying UF2 file to Pico..."
cp "$UF2_FILE" "$PICO_MOUNT/"

echo ""
echo "=== Deployment Complete! ==="
echo "The Pico will automatically reboot and start running your program."
echo "Connect to serial monitor (e.g., screen /dev/tty.usbmodem* 115200) to see output."

#!/bin/bash

set -e

# Target firmware (default: neopico_hd)
TARGET="${1:-neopico_hd}"

echo "=== NeoPico HD Flash Script ==="
echo "Target: $TARGET"
echo ""

# Build the project
echo "Building project..."
if [ ! -d "build" ]; then
    mkdir build
    cd build
    cmake ..
    cd ..
fi

cd build
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) "$TARGET"
cd ..

echo "Build complete!"
echo ""

# Check if UF2 file exists
UF2_FILE="build/src/${TARGET}.uf2"
if [ ! -f "$UF2_FILE" ]; then
    echo "Error: UF2 file not found at $UF2_FILE"
    exit 1
fi

echo "UF2 file ready: $(ls -lh $UF2_FILE | awk '{print $9, "(" $5 ")"}')"
echo ""

# Reset Pico into BOOTSEL mode
echo "=== RESETTING PICO ==="
picotool reboot -f -u 2>/dev/null || true
echo "Waiting for Pico to enter BOOTSEL mode..."
sleep 2

# Flash firmware
echo "=== FLASHING ==="
picotool load -f "$UF2_FILE" || {
    echo "✗ Flash failed! Make sure Pico is connected."
    exit 1
}

echo ""
echo "✓ Firmware flashed successfully"
echo ""

# Reboot Pico to run firmware
echo "=== REBOOTING ==="
picotool reboot || true

echo ""
echo "=== DONE ==="
echo ""

#!/bin/bash
# Common flash script - called by flash.sh and flash_analyzer.sh
# Usage: flash_common.sh <firmware> <serial> <name>

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

FIRMWARE="$1"
SERIAL="$2"
NAME="$3"

if [ -z "$FIRMWARE" ] || [ -z "$SERIAL" ] || [ -z "$NAME" ]; then
    echo "Usage: $0 <firmware> <serial> <name>"
    exit 1
fi

if ! command -v picotool &> /dev/null; then
    echo "Error: picotool not found"
    exit 1
fi

if [ ! -f "$FIRMWARE" ]; then
    echo "Error: Firmware not found: $FIRMWARE"
    exit 1
fi

echo "=== Flashing $NAME ($SERIAL) ==="
echo "Firmware: $FIRMWARE"

# Try to flash, retry with wait if needed
for attempt in 1 2 3; do
    if picotool load --ser "$SERIAL" -f -x "$FIRMWARE" 2>&1; then
        echo "Done!"
        exit 0
    fi

    echo "Attempt $attempt failed, waiting 2s for device..."
    sleep 2
done

echo "Error: Failed to flash after 3 attempts"
echo "Try manually: hold BOOTSEL, plug in, then run:"
echo "  picotool load -x $FIRMWARE --ser $SERIAL"
exit 1

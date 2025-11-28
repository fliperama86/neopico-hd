#!/bin/bash

DEVICE="/dev/tty.usbmodem21101"
LOGFILE="captured_output.txt"

# Remove old capture file
rm -f $LOGFILE screenlog.0

echo "=== Frame Capture with Screen Logging ==="
echo "Device: $DEVICE"
echo "Output: $LOGFILE"
echo ""
echo "Instructions:"
echo "  1. Screen will start with logging enabled"
echo "  2. Wait until you see '=== Frame output complete ==='"
echo "  3. Press Ctrl-A then K to quit screen"
echo "  4. Press Y to confirm"
echo ""
echo "Starting in 3 seconds..."
sleep 3

# Start screen with logging enabled
# -L turns on logging (creates screenlog.0)
screen -L $DEVICE 115200

# After screen exits, move the log file and extract the PBM
if [ -f screenlog.0 ]; then
    mv screenlog.0 $LOGFILE
    echo ""
    echo "Screen closed. Extracting PBM..."
    "$(dirname "$0")/extract_pbm.sh"
else
    echo "ERROR: No screenlog.0 file created"
    exit 1
fi

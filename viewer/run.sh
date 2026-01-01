#!/bin/bash
# Launch MVS Capture Viewer

cd "$(dirname "$0")"

# Activate venv
if [ ! -d "venv" ]; then
    echo "Error: venv not found. Create it with: python3 -m venv venv && venv/bin/pip install pygame pyserial"
    exit 1
fi

source venv/bin/activate

# Run viewer
python main.py "$@"

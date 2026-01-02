#!/bin/bash
# Common build script - called by build.sh and build_analyzer.sh
# Usage: build_common.sh <target> <name>

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

TARGET="$1"
NAME="$2"

if [ -z "$TARGET" ] || [ -z "$NAME" ]; then
    echo "Usage: $0 <target> <name>"
    exit 1
fi

# Check for PICO_SDK_PATH
if [ -z "$PICO_SDK_PATH" ]; then
    echo "Error: PICO_SDK_PATH environment variable not set"
    echo "Set it with: export PICO_SDK_PATH=/path/to/pico-sdk"
    exit 1
fi

echo "=== Building $NAME ==="

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Run cmake if not configured or if CMakeLists.txt changed
if [ ! -f "Makefile" ] || [ ../CMakeLists.txt -nt Makefile ]; then
    echo "Configuring project..."
    cmake ..
fi

# Build target
echo "Compiling..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) "$TARGET"

echo ""
echo "Done! Output: $BUILD_DIR/$TARGET.uf2"

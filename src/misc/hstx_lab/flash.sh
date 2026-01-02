#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/scripts/env.sh"

"$SCRIPT_DIR/scripts/flash_common.sh" \
    "build/hstx_lab.uf2" \
    "$DVI_PICO_SERIAL" \
    "DVI Pico"

# CLAUDE.md

Instructions for Claude Code (claude.ai/code) when working with this repository.

## Important

**Always read `README.md` first** for project documentation, architecture, pin configurations, and build instructions. Update `README.md` when documenting important changes or new features.

## Quick Reference

```bash
# Build
./scripts/build.sh
# or: cd build && make neopico_hd

# Flash
picotool load -f build/src/neopico_hd.uf2

# Serial monitor
screen /dev/tty.usbmodem* 115200
```

## AI-Specific Guidelines

### Code Style

- When using terminal output, prefer updating the same line instead of printing new lines (use `\r` and `fflush`)
- PIO code is timing-critical - changes require hardware testing
- Avoid large buffers - memory budget is tight (~150KB framebuffer on 520KB SRAM)

### RP2350 Errata

- **Errata E9**: Internal pull-downs on GPIO 30+ have a hardware bug. Use pull-ups for buttons, or external resistors.

### Aliases

- `CAV` = "cps2_digiav MVS" (reference implementation in `reference/cps2_digiav/`)

### Testing Without Hardware

The `dvi_test` and `audio_pipeline_test` targets can verify DVI output and audio pipeline without MVS hardware connected.

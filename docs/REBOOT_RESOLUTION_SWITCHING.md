# Reboot Resolution Switching

NeoPico-HD uses a reboot-based OSD selector for 240p, 480p, and 720p. It is enabled by default with:

- `NEOPICO_RESOLUTION_MENU=ON`
- `NEOPICO_RESOLUTION_MENU_720P=ON`
- `NEOPICO_SETTINGS_FLASH=ON`
- `NEOPICO_OSD_RES_CONFIRM=ON`

No extra CMake flags are required for the standard MVS or SNES build.

## Behavior

1. Open the root OSD menu.
2. Enter `Resolution`.
3. Cycle with BACK or controller SELECT.
4. Confirm with MENU or controller START.
5. Firmware saves the candidate mode and reboots.
6. Keep the new mode with MENU/START, or revert with BACK/SELECT. Timeout also reverts.

The selected resolution is stored in the last flash sector and survives power loss. Watchdog scratch registers carry the selected mode and confirmation state across the warm reboot without changing the flash record again.

## Fixed-Mode Builds

Disable `NEOPICO_RESOLUTION_MENU` and `NEOPICO_OSD_RES_CONFIRM`, then select a fixed output with `NEOPICO_VIDEO_240P` or `NEOPICO_VIDEO_720P`. The non-RT 720p release path also uses `NEOPICO_USE_NONRT_HDMI=ON`.

## Static Audit Helper

Use `scripts/audit_firmware.py <build-directory>` before flashing timing-sensitive builds. It checks section sizes, critical HSTX/scanline symbols, scratch memory usage, and flash-resident Core 1 background symbols.

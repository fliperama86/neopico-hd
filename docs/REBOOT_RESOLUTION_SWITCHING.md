# Reboot Resolution Switching

NeoPico-HD uses a reboot-based OSD selector for 240p, 480p, and 720p. It is enabled by default with:

- `NEOPICO_RESOLUTION_MENU=ON`
- `NEOPICO_RESOLUTION_MENU_720P=ON`
- `NEOPICO_SETTINGS_FLASH=ON`
- `NEOPICO_OSD_RES_CONFIRM=ON`

This reboot behavior is specific to resolution changes. The optional MVS
`Colors` selector applies its Digital/Analog LUT at input VSYNC and persists it
without rebooting the firmware.

No extra CMake flags are required for the standard MVS or SNES build.

## Behavior

1. Open the root OSD menu.
2. Enter `Resolution`.
3. Move with controller UP/DOWN, or cycle with the physical BACK button.
4. Confirm with MENU or controller START.
5. Firmware saves the candidate mode and reboots.
6. Keep the new mode with MENU/START, or revert with BACK/SELECT. Timeout also reverts.

The selected resolution is stored in the last flash sector and survives power loss. Watchdog scratch registers carry the selected mode and confirmation state across the warm reboot without changing the flash record again.

## 240p HDMI Compatibility

All three selector modes use the same runtime output engine and unified application scanline callback. The 240p mode still requires mode-specific HDMI metadata because its 1280x240 timing is not a CEA video mode.

Production builds use the hardware-validated conservative VIC-0 AVI payload for 240p: `PB1=0x00`, `PB2=0x08`, and pixel repetition `PR=0`. The newer active-format/aspect payload with `PR=3` produced black video while HDMI audio continued on tested scaler paths. The compatibility payload applies only to 240p; 480p and 720p retain their normal AVI metadata.

## Factory Reset

Hold the physical MENU and BACK buttons together for at least 5 seconds. The gesture works independently of OSD visibility so it can recover from an unusable video mode. Firmware persists the factory defaults, 480p output, MV1C Digital audio, and Digital MVS color when that selector is compiled, then immediately reboots into 480p.

## Fixed-Mode Builds

Disable `NEOPICO_RESOLUTION_MENU` and `NEOPICO_OSD_RES_CONFIRM`, then select a fixed output with `NEOPICO_VIDEO_240P` or `NEOPICO_VIDEO_720P`. The non-RT 720p release path also uses `NEOPICO_USE_NONRT_HDMI=ON`.

## Static Audit Helper

Use `scripts/audit_firmware.py <build-directory>` before flashing timing-sensitive builds. It checks section sizes, critical HSTX/scanline symbols, scratch memory usage, and flash-resident Core 1 background symbols.

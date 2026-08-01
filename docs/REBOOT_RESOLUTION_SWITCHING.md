# Reboot Resolution Switching

NeoPico-HD uses a reboot-based OSD selector for 240p, 480p, and 720p. It is
permanently on: the resolution menu, its 720p entry, flash-backed settings
persistence, and the post-switch keep/revert confirmation prompt are all
always compiled in -- this is the only shipped output-mode path.

This reboot behavior is specific to resolution changes. The MVS `Colors`
selector (available whenever DARK/SHADOW is off) applies its Digital/Analog
LUT at input VSYNC and persists it without rebooting the firmware.

No extra CMake flags are required for the standard MVS or SNES build.

## Behavior

1. Open the root OSD menu.
2. Enter `Resolution`.
3. Move with controller UP/DOWN, or cycle with the physical BACK button.
4. Confirm with MENU or controller START.
5. Firmware carries the candidate mode in watchdog scratch and reboots.
6. Keep and persist the new mode with MENU/START, or revert with BACK/SELECT. Timeout also reverts.

The selected resolution is stored in the last flash sector and survives power loss. Watchdog scratch registers carry the selected mode and confirmation state across the warm reboot without changing the flash record again.

## Exact-Clock 720p Mode

The standard three-mode selector's `720p` entry uses a 1280x720
reduced-blanking raster with a 64.000 MHz pixel clock from a 320 MHz system and
HSTX clock. Its totals are 1440x741 (horizontal 1280/48/32/80, vertical
720/3/5/13), its refresh is 59.979 Hz, and its sync polarities are H+/V-. The
80-pixel back porch carries the HDMI Data Island. Audio uses N=6144 and
CTS=64000 for an exact 48 kHz recovered rate.

This timing is non-CTA. Its AVI InfoFrame therefore uses VIC 0 while explicitly
declaring a 16:9 picture. It reduced the former HSTX clock from 372 MHz to
320 MHz and passed initial sink smoke tests, but it is not guaranteed to be
more compatible than CTA VIC 4 on every TV.

## 240p HDMI Compatibility

All selector modes use the same runtime output engine and unified application scanline callback. The 240p mode still requires mode-specific HDMI metadata because its 1280x240 timing is not a CEA video mode.

Production builds use the hardware-validated conservative VIC-0 AVI payload for 240p: `PB1=0x00`, `PB2=0x08`, and pixel repetition `PR=0`. The newer active-format/aspect payload with `PR=3` produced black video while HDMI audio continued on tested scaler paths. The compatibility payload applies only to 240p. The exact-clock 720p descriptor also uses VIC 0, but independently declares a valid 16:9 active aspect.

## Factory Reset

Hold the physical MENU and BACK buttons together for at least 5 seconds. The gesture works independently of OSD visibility so it can recover from an unusable video mode. Firmware persists the factory defaults, 480p output, MV1C Digital audio, and Digital MVS color when that selector is compiled, then immediately reboots into 480p.

## Fixed-Mode Builds

The reboot-based resolution selector can no longer be disabled, and the
alternate output paths it replaced (a fixed compile-time 720p mode and the
compile-time, non-RT pico_hdmi backend it required) have been deleted
outright. The runtime pico_hdmi path with the reboot-based selector is the
only shipped output-mode path.

## Static Audit Helper

Use `scripts/audit_firmware.py <build-directory>` before flashing timing-sensitive builds. It checks section sizes, critical HSTX/scanline symbols, scratch memory usage, and flash-resident Core 1 background symbols.

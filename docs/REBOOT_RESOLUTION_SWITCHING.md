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

Two additional PC-monitor entries can be compiled in with:

- `NEOPICO_RESOLUTION_MENU_PC_MODES=ON`

This option is default-off and requires both base selector options above. It
adds modes to the selector; it does not replace or modify the existing
1280x720 HDTV entry.

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

## Optional PC Monitor Modes

The PC-enabled selector contains five entries in this order: 240p, 480p,
1280x720 HDTV, native 960x720 PC, and 1024x768 PC. Both PC modes apply an exact
3x scale to the 320x240 source.

| Selector entry | Pixel clock | Horizontal timing (active/front/sync/back/total) | Vertical timing (active/front/sync/back/total) | Sync | Refresh | Active-area layout |
| -------------- | ----------- | ------------------------------------------------ | ---------------------------------------------- | ---- | ------- | ------------------ |
| `960x720 PC` | 56.000 MHz | 960/48/96/144/1248 | 720/3/4/21/748 | H-, V+ | 59.989 Hz | 960x720 content, no active black border |
| `1024x768 PC` | 64.800 MHz | 1024/24/136/156/1340 | 768/3/6/29/806 | H-, V- | 59.998 Hz | 960x720 content centered with 32-pixel left/right and 24-line top/bottom black borders |

Both descriptors use HDMI AVI VIC 0 with a 4:3 active aspect. The native mode
is based on CVT 960x720 timing at an exact RP2350-achievable pixel clock. The
bordered mode is close to VESA 1024x768 DMT, with a slightly adjusted pixel
clock and horizontal total.

For scanline headroom, both modes run the RP2350 system clock at 384 MHz. HSTX
uses an independent PLL_USB-derived clock: 280 MHz for native 960x720 and
324 MHz for 1024x768. Before repurposing PLL_USB, the firmware routes USB and
ADC to an exact 48 MHz derived from PLL_SYS and moves `clk_peri` to the 12 MHz
crystal.

These modes have passed clean firmware builds and static SRAM/timing-layout
audits. They have not yet been tested on a physical RP2350B or PC monitor. Keep
the option off in release firmware until sink lock, image placement, audio,
mode confirmation/revert, and sustained stability have been verified.

Build the optional selector with:

```bash
cmake -S . -B build_pc_modes -DNEOPICO_RESOLUTION_MENU_PC_MODES=ON
cmake --build build_pc_modes --target neopico_hd -j4
python3 scripts/audit_firmware.py build_pc_modes
```

## 240p HDMI Compatibility

All selector modes use the same runtime output engine and unified application scanline callback. The 240p mode still requires mode-specific HDMI metadata because its 1280x240 timing is not a CEA video mode.

Production builds use the hardware-validated conservative VIC-0 AVI payload for 240p: `PB1=0x00`, `PB2=0x08`, and pixel repetition `PR=0`. The newer active-format/aspect payload with `PR=3` produced black video while HDMI audio continued on tested scaler paths. The compatibility payload applies only to 240p. The exact-clock 720p descriptor also uses VIC 0, but independently declares a valid 16:9 active aspect.

## Factory Reset

Hold the physical MENU and BACK buttons together for at least 5 seconds. The gesture works independently of OSD visibility so it can recover from an unusable video mode. Firmware persists the factory defaults, 480p output, MV1C Digital audio, and Digital MVS color when that selector is compiled, then immediately reboots into 480p.

## Fixed-Mode Builds

Disable `NEOPICO_RESOLUTION_MENU` and `NEOPICO_OSD_RES_CONFIRM`, then select a fixed output with `NEOPICO_VIDEO_240P` or `NEOPICO_VIDEO_720P`. The non-RT 720p release path also uses `NEOPICO_USE_NONRT_HDMI=ON`.

## Static Audit Helper

Use `scripts/audit_firmware.py <build-directory>` before flashing timing-sensitive builds. It checks section sizes, critical HSTX/scanline symbols, scratch memory usage, and flash-resident Core 1 background symbols.

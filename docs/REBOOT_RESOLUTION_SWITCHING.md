# Reboot Resolution Switching

This note documents the stable 480p / 240p / 720p switching path found during
the April 2026 investigation.

## Stable Baseline

The stable build keeps HDMI audio enabled and removes the OSD/menu module from
the switching path. Resolution changes are requested by the BACK button, stored
in watchdog scratch registers, and applied on reboot.

Known-good configuration:

```bash
cmake -S . -B build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi \
  -DNEOPICO_ENABLE_OSD=OFF \
  -DNEOPICO_VIDEO_DVI_ONLY=OFF \
  -DNEOPICO_VIDEO_240P=OFF \
  -DNEOPICO_VIDEO_720P=OFF \
  -DNEOPICO_EXP_REBOOT_MODE_SWITCH=ON \
  -DNEOPICO_EXP_REBOOT_MODE_SWITCH_720P=ON \
  -DNEOPICO_EXP_REBOOT_BUTTON_CYCLER=ON \
  -DNEOPICO_EXP_LEGACY_240P_AVI_INFOFRAME=ON
cmake --build build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi --target neopico_hd -j8
```

Flash with:

```bash
pi flash build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi/src/neopico_hd.uf2
```

Behavior:

- BACK cycles `480p -> 240p -> 720p -> 480p`.
- The selected mode is written to watchdog scratch and takes effect after the
  watchdog reboot.
- No OSD is linked or displayed in this baseline.
- HDMI audio remains enabled.

## Why The OSD-Free Path Matters

The OSD selector builds were unstable even when the menu was never opened. That
made the OSD drawing itself unlikely to be the only cause. The stable cycler
keeps resolution selection out of `menu_diag_experiment.c`, so OSD buffers,
selector rendering, and menu callbacks are not part of the linked hot image.

In the tested build, removing the OSD path reduced `.bss` by about 58 KB versus
the OSD-enabled 720p cycler. This is a practical guard against RP2350 layout and
bus-timing sensitivity, especially because HSTX output, audio work, DMA, and
XIP flash/cache behavior share tight real-time margins.

## HDMI Details Required For Stability

### 240p AVI InfoFrame

240p is a non-CEA / out-of-spec HDMI mode for this firmware. The stable path
uses the v0.1.5-style VIC-0 AVI metadata for 240p:

- `PB1 = 0x00`
- `PB2 = 0x08`
- pixel repetition field `PR = 0`

The newer 240p metadata with active-format/aspect signaling and `PR = 3` caused
sync failures in the full HDMI/audio stream on the tested RetroTINK-4K path.
480p and 720p keep their normal AVI metadata.

### 720p Data Island Builder

Runtime-mode selector builds must support both wide-HSYNC modes (480p/240p) and
the narrow-HSYNC 720p mode. The stable PicoHDMI runtime path keeps the 480p/240p
Data Island builder specialized for negative-sync DI-in-HSYNC lines, while 720p
uses a separate back-porch Data Island builder in scratch-Y. This prevents the
480p/240p hot builder from growing into a combined all-mode path.

## Static Audit Helper

Use `scripts/audit_firmware.py` before flashing timing-sensitive builds:

```bash
scripts/audit_firmware.py build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi
scripts/audit_firmware.py <candidate-build> --baseline <known-good-build>
```

The audit checks section sizes, critical HSTX/scanline symbols, scratch memory
usage, and flash-resident Core 1 background symbols. It cannot prove sink
stability, but it catches obvious layout regressions before a flash/watch cycle.

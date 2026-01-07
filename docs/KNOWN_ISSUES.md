# Known Issues

This document tracks known hardware and software limitations of the NeoPico-HD project.

## HDMI Sync Stability

### 1. Soft Reset Reliability

**Issue**: Performing a soft reset on the RP2350 (e.g., via `picotool` or a reset button) often results in a "No Signal" state on the connected TV or scaler.
**Symptoms**: The HDMI sink fails to handshake with the HSTX hardware after a reboot.
**Workaround**:

1. Power OFF the Neo Geo MVS.
2. Reset the NeoPico-HD (Pico).
3. Power ON the Neo Geo MVS.

### 2. Back-Powering & 5V Logic Levels

**Observation**: The power LED on the WeAct RP2350 board remains illuminated when the MVS is powered ON, even if the Pico's USB cable is disconnected.
**Cause**: The Neo Geo MVS uses **5V logic**, while the RP2350 is a **3.3V device**. Without level shifters, 5V is being fed directly into the Pico's GPIOs. This current leaks through the internal ESD protection diodes into the 3.3V rail.
**Risk**: This creates massive electrical noise on the internal bus, causes "latch-up" stress on the I/O ring, and can lead to permanent hardware degradation.
**Workaround**: Current software implements "Safe Mode" (No Pulls + Schmitt Triggers) to mitigate stress, but **Level Shifters (e.g., 74LVC245 or TXU0104) are highly recommended.**

---

## OSD & UI

- **Current Status**: OSD is currently disabled in the main branch to prioritize HDMI signal stability during video capture development.

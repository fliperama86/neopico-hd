# Genlock

## Problem

MVS runs at `~59.18 Hz`. HDMI output at 60 Hz causes periodic frame repeats (`~every 2s`), visible as stutter in scrolling games.

## Current: Software Genlock

Adjust sysclk so HDMI pixel clock produces ~59.18 Hz output. No extra hardware.

- `NEOPICO_EXP_GENLOCK_STATIC=ON` + `NEOPICO_GENLOCK_TARGET_FPS_X100=5918` — sets sysclk at boot.
- `NEOPICO_EXP_VTOTAL_MATCH=ON` + `NEOPICO_EXP_VTOTAL_LINES=532` — alternative: keep pixel clock fixed, adjust vertical total.
- Relies on TV accepting non-standard refresh rate (most do).

## Future: Hardware Genlock via GPIN

RP2350 has two GPIO clock inputs (GPIN0/GPIN1) routable to any clock domain including clk_sys and clk_hstx:

| GPIN  | Available GPIOs |
| ----- | --------------- |
| GPIN0 | GP12\*, GP20    |
| GPIN1 | GP14\*, GP22    |

\*GP12/GP14 are HSTX (locked). GP20 or GP22 are usable with pin reassignment.

**Path:** Feed MVS PCLK (6 MHz) into GPIN → PLL multiplies to ~124 MHz → clk_sys/clk_hstx phase-locked to MVS. True genlock, no drift.

**API:** `clock_configure_gpin(clk_ref, gpio, src_freq, freq)` — SDK built-in.

**Board change needed:** Route MVS PCLK to GP20 or GP22 (requires I2S pin reassignment if GP22).

**Note:** SWCLK is a dedicated debug pin with no function mux — cannot be repurposed as GPIN or GPIO.

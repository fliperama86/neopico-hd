# DVI Pin Configuration for RP2350

## Working Configuration

```
CLK:  GP31-32 (CLKN/CLKP)
D0:   GP25-26 (D0N/D0P)
D1:   GP27-28 (D1N/D1P)
D2:   GP29-30 (D2N/D2P)
```

## Key Findings

### 1. PIO GPIO Window Limitation
RP2350 PIO can only access a 32-pin window at a time. Default is GPIO 0-31.

**Fix for GP32+:** Call `pio_set_gpio_base(pio0, 16)` before `dvi_init()` to shift window to GPIO 16-47.

### 2. PWM Clock Pin Requirement
PWM clock requires both pins on the same PWM slice (even-numbered base pin).

**Fix for odd clock pins:** Add `DVI_USE_PIO_CLOCK=1` compile definition to use PIO-based clock instead.

## Implementation

See `src/neopico_config.h` for the shared configuration:
- `neopico_dvi_cfg` - DVI serializer pin mapping
- `neopico_dvi_gpio_setup()` - Call before `dvi_init()`

All DVI targets in CMakeLists.txt include `DVI_USE_PIO_CLOCK=1`.

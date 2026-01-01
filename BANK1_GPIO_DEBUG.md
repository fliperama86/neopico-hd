# Bank 1 GPIO Debug Log - NeoPico-HD

**Date**: 2025-12-31
**Issue**: Video capture not working - black screen on HDMI and Python viewer

---

## Problem Statement

NeoPico-HD video capture showing no video output. Both HDMI and Python viewer (USB serial) show black/no frames.

---

## Root Cause: RP2350 PIO Bank 1 GPIO Limitation

**Technical Issue**: PIO instructions have 5-bit address fields
- `wait gpio N` - can only address N = 0-31 (5-bit field)
- `jmp pin N` - same 5-bit limitation
- **Bank 1 GPIOs (32-47) cannot be addressed directly!**

---

## Pin Configuration (Hardware Verified)

| Pin | Function | Bank | Status |
|-----|----------|------|--------|
| GP29 | PCLK (Pixel Clock) | Bank 0 | ✅ 6 MHz detected |
| GP46 | CSYNC (Composite Sync) | Bank 1 | ✅ ~16 kHz detected (via GPIO) |
| GP31-46 | RGB555 Data (scrambled) | Bank 1 | ⚠️ PIO cannot access |

**Verification Methods**:
- Oscilloscope: 6 MHz on GP29 ✓
- Direct GPIO read: GP46 toggles 10-20 times per 100ms ✓
- PIO test on GP29: 5.99 MHz detected ✓
- PIO test on GP46: **FAILED with wait gpio** ❌

---

## Attempted Solutions

### 1. Manual gpio_base Configuration (FAILED)
```c
pio->ctrl = (pio->ctrl & ~(0x1F << PIO_GPIOBASE_LSB)) | (32 << PIO_GPIOBASE_LSB);
```
- Set PIO CTRL.GPIOBASE to 32 (Bank 1 offset)
- **Result**: Still failed - gpio_base doesn't affect instruction encoding

### 2. JMP PIN Polling Workaround (FAILED)
```pio
wait_high:
    jmp pin found_high      ; Jump if pin high
    jmp wait_high           ; Loop
found_high:
```
- **Problem**: `jmp pin` also has 5-bit limitation!
- `sm_config_set_jmp_pin()` uses 5-bit EXECCTRL register field

### 3. SDK Function + WAIT PIN (TEST SUCCESS, MAIN CODE FAILED)

**Test Firmware** (`src/misc/bank1_test.c`):
```c
// Used SDK 2.1.1+ function for Bank 1 GPIO support
pio_claim_free_sm_and_add_program_for_gpio_range(
    &pio_edge_program, &pio, &sm, &offset,
    TEST_PIN, 1, true  // gpio_base=46, count=1, set_gpio_base=true
);

// PIO program
wait 0 pin, 0  // Wait for low
wait 1 pin, 0  // Wait for high (edge)
jmp x--, 0     // Count
```

**Result**: ✅ **SUCCESS!**
- Edges counted: **16,154 Hz** on GP46
- PIO selected: **PIO2** SM0
- gpio_base reported: **0** (not 32 as expected!)
- IN_BASE set to: **46** (pin_offset = TEST_PIN - gpio_base)

**Main Firmware** (`src/video/video_capture.c`):
- Applied same SDK function
- Simplified sync program to identical `wait pin` logic
- **Result**: ❌ **FAILED** - continuous timeouts
- Debug output: "Frame capture timeout" repeating
- Diagnosis: `wait_for_vsync()` never detects edges

---

## UPDATE: Boot Messages Investigation (2025-12-31 22:42)

**Finding**: Boot messages are NOT missing - they're printed before USB CDC enumerates!
- Main firmware runs successfully through all initialization
- Timeout messages appear (count 90+) proving stdio works
- Boot printfs happen during 2-second stdio_init delay before USB ready
- **Conclusion**: Firmware initializes correctly, but video capture fails in `wait_for_vsync()`

**USB Enumeration**: After flashing with picotool, explicit `picotool reboot` required for USB CDC to appear.

---

## UPDATE: Deep Dive Investigation (2025-12-31 23:00)

### Critical Findings

**✅ SDK Function Works Correctly**:
- `pio_claim_free_sm_and_add_program_for_gpio_range()` successfully claims PIO2 SM0
- `gpio_base = 0` (same as working test firmware)
- `pin_offset = PIN_CSYNC - gpio_base = 46 - 0 = 46`
- IN_BASE correctly set to 46

**✅ PIO Configuration Verified**:
```
PIO: PIO2 SM0
gpio_base: 0
pin_offset: 46
SM enabled: Yes
```

**❌ FIFO Always Empty**:
- 782,000+ empty checks per 100ms timeout
- Zero data ever pushed to FIFO
- `wait pin` instructions never trigger

### Debugging Steps Taken

1. **PIO Conflict Eliminated**:
   - Changed audio pipeline from PIO2 to PIO1 (then disabled entirely)
   - Disabled pixel capture program - sync runs alone
   - Still fails

2. **Exact Test Replication**:
   - Copied exact 3-instruction edge counter from working bank1_test
   - Matched GPIO initialization sequence exactly
   - Used same SDK function call
   - Used same pin_offset calculation
   - Still fails

3. **PIO Program Evolution**:
   ```pio
   ; Final simplified version (still fails)
   wait 0 pin, 0      ; Wait for CSYNC low
   wait 1 pin, 0      ; Wait for CSYNC high
   push noblock       ; Push edge to FIFO
   jmp entry_point    ; Loop
   ```

4. **Initialization Sequence**:
   ```c
   // Matches test exactly
   gpio_init(PIN_CSYNC);
   gpio_set_dir(PIN_CSYNC, GPIO_IN);
   gpio_pull_down(PIN_CSYNC);
   pio_gpio_init(g_pio_mvs, PIN_CSYNC);

   sm_config_set_in_pins(&c, pin_offset);  // 46
   pio_sm_init(g_pio_mvs, g_sm_sync, offset_sync, &c);
   pio_sm_set_enabled(g_pio_mvs, g_sm_sync, true);
   ```

### Hypotheses Remaining

1. **Hardware State**: MVS might be off/not outputting CSYNC during tests
   - User confirmed MVS works via SCART
   - Test firmware detected 16 kHz on GP46 earlier
   - **Need to verify MVS is currently ON**

2. **DVI Interference**: PicoDVI on PIO0 running before video capture init
   - Unlikely since different PIO blocks
   - But timing/GPIO state might matter

3. **RP2350 PIO2 Bank 1 Access**: gpio_base=0 might not allow Bank 1 access
   - Test had gpio_base=0 and worked
   - Contradicts this theory unless context-dependent

4. **Unknown Initialization Order Issue**: Something about full firmware breaks PIO
   - Test runs standalone, works
   - Main firmware has DVI/audio/complex init, fails
   - Same PIO code, same SDK function, different result

## Current Investigation: Why Test Works But Main Code Doesn't

### Differences Between Test and Main Code

| Aspect | Test (Working) | Main (Failing) |
|--------|---------------|----------------|
| **PIO Programs** | 1 simple program | 2 programs (sync + pixel) |
| **Setup** | SDK function only | SDK function + manual add |
| **PIO Usage** | Dedicated PIO2 | Shared PIO (both programs) |
| **Other PIOs** | None | PIO0 used by DVI output |
| **GPIO Init** | Simple test pin | Complex: DVI, I2S, Video |
| **Timing** | No time-critical ops | DVI running on Core 1 |

### Hypotheses

1. **PIO Conflict**: Sync on PIO2, but pixel added manually might fail
2. **GPIO Initialization Order**: DVI init (PIO0) might affect PIO2 gpio_base
3. **Timing Issue**: Complex sync program has bugs vs simple edge counter
4. **IN_BASE Misconfiguration**: Init function not setting IN_BASE correctly

---

## Detailed Test Results

### bank1_test.c Output (WORKING)
```
TEST 1: Direct GPIO Read (baseline)
  Sample 1: 26 toggles in 100ms
  Sample 2: 10 toggles in 100ms
  Sample 3: 7 toggles in 100ms

TEST 2: PIO Edge Counter with SDK gpio_range function
  Using pio_claim_free_sm_and_add_program_for_gpio_range()
  Claimed PIO2 SM0, offset=29
  gpio_base=0, pin_offset=46
  Edges counted: 16154 (16154 Hz)
  ✓ SUCCESS! Bank 1 GPIO accessible with jmp pin!
  CSYNC frequency: ~16154 Hz (~15 kHz expected)
```

### Main Firmware Output (FAILING)
```
Starting Core 1 (DVI)
Starting line-by-line capture loop
Frame capture timeout (count: 10)
Frame capture timeout (count: 20)
Frame capture timeout (count: 30)
...
```
Timeout rate: ~100ms per timeout = 10 Hz timeout loop

---

## PIO Code Evolution

### Original (BROKEN - used jmp pin for Bank 1)
```pio
wait_csync_high:
    jmp pin csync_is_high       ; BROKEN: jmp pin can't access GP46
    jmp wait_csync_high
csync_is_high:
    ; ... pixel counting logic
```

### Current Simplified Version (STILL BROKEN)
```pio
.wrap_target
    wait 0 pin, 0               ; Wait for CSYNC low (IN_BASE=46, pin 0)
    wait 1 pin, 0               ; Wait for CSYNC high (rising edge)
    set x, 31
    mov isr, x
    push noblock
.wrap
```

**Configuration**:
- SDK function claims PIO and SM for GPIO range 46
- `sm_config_set_in_pins(&c, pin_csync)` where pin_csync = 46
- PIO should be PIO2 (as in test)

---

---

## Summary of Current State (2025-12-31 23:05)

### What Works
- ✅ bank1_test.c firmware successfully detects 16 kHz on GP46 (Bank 1 GPIO)
- ✅ SDK function correctly claims PIO2 SM0 and sets up for Bank 1 access
- ✅ Main firmware initializes without errors
- ✅ PIO configuration matches working test exactly

### What Doesn't Work
- ❌ Main firmware PIO FIFO always empty
- ❌ `wait pin` instructions never trigger in main firmware context
- ❌ Same code works in test, fails in main

### Critical Question
**Is the MVS currently powered on and outputting CSYNC?**

The test firmware successfully detected signals earlier today. If the MVS is currently off, that would explain why the FIFO is empty. The user confirmed MVS works via SCART, but we need to verify it's powered on RIGHT NOW during these tests.

## Next Steps

1. **IMMEDIATE**: Verify MVS hardware state
   - Check if MVS is powered on
   - Verify CSYNC signal present on GP46 with oscilloscope OR
   - Flash bank1_test again and confirm it still detects 16 kHz

2. **If MVS is ON and test still works**:
   - Compare exact memory/register state between test and main firmware
   - Try minimal main.c with ONLY sync detection (no DVI, no audio)
   - Check if DVI init on PIO0 affects GPIO state globally

3. **Alternative Approaches**:
   - Use direct GPIO polling instead of PIO as temporary workaround
   - Try HSTX for DVI output to free up PIO0
   - Investigate RP2350-specific PIO Bank 1 requirements

---

## Hardware Info

- **Board**: WeAct Studio RP2350B Core
- **MVS**: Neo Geo arcade board (CPS2 DigiAV reference)
- **SDK Version**: pico-sdk with `pio_claim_free_sm_and_add_program_for_gpio_range()` available
- **System Clock**: 150 MHz
- **DVI Output**: 640x480p 60Hz via PicoDVI (PIO0)

---

## Code References

- Test firmware: `/Users/dudu/Projects/neopico-hd/src/misc/bank1_test.c`
- Main capture: `/Users/dudu/Projects/neopico-hd/src/video/video_capture.c`
- PIO programs: `/Users/dudu/Projects/neopico-hd/src/video/video_capture.pio`
- Pin definitions: `/Users/dudu/Projects/neopico-hd/src/pins.h`

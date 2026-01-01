# RP2350 Bank 1 & PIO Capture: Technical Findings

This document summarizes the technical investigation into video capturing using Bank 1 GPIOs (GP32-47) on the RP2350 B revision.

## 1. Hardware Architecture (Bank 1)
*   **GPIO Layout:** RP2350 has 48 GPIOs. Bank 0 (0-31) and Bank 1 (32-47).
*   **The GPIOBASE Register:** PIO instances on RP2350 (PIO0 and PIO1) have a new register at offset `0x168`. This register defines the base of a 32-pin window accessible to the PIO.
*   **PIO2 Limitation:** PIO2 is a "reduced" instance and **lacks the GPIOBASE register**. It is permanently fixed to Bank 0 (0-31). Video capture must use PIO0 or PIO1.

## 2. PIO Instruction Limitations
*   **5-Bit Fields:** The `WAIT GPIO <n>` and `JMP PIN <n>` instructions use a 5-bit field for the pin number. This allows addressing only 32 pins (0-31).
*   **Relative Addressing:** On RP2350, these 5 bits are **relative to GPIOBASE**. 
    *   If `GPIOBASE = 16`, then `WAIT GPIO 0` actually waits for **GP16**.
    *   To wait for **GP46**, the instruction needs index `30` (46 - 16).

## 3. SDK "Help" Trap
One of the most unexpected findings was the behavior of `pio_add_program()`:
*   **Automatic Instruction Modification:** When `PICO_PIO_USE_GPIO_BASE` is defined, the SDK's program loader **automatically XORs the 5th bit** of `WAIT GPIO` instructions based on the current `GPIOBASE`.
*   **Conflict:** if you manually calculate the relative index in your `.pio` file (e.g., using 13 for GP29), and the SDK "helps" you by XORing it again, the PIO ends up looking at the wrong pin entirely.
*   **Absolute vs Relative:** The SDK expects you to use **absolute** GPIO numbers in the `.pio` file, and it will attempt to remap them at runtime. However, this remapping is inconsistent across different SDK versions and depends on the `pinhi` field in the `pio_sm_config` struct.

## 4. Struct Layout Mismatch
*   **`pio_sm_config` Size:** On RP2350, the `pio_sm_config` struct has an additional 32-bit field `pinhi`. 
*   **Memory Corruption:** If the application is compiled with different flags than the SDK library, the offsets for `pinctrl`, `execctrl`, and `shiftctrl` will be misaligned, leading to silent failures or crashes when calling `pio_sm_init`.

## 5. RP2350-E9 Erratum (Sticky Pins)
*   **The Bug:** Internal pull-down resistors on the RP2350 B revision can cause GPIOs to "latch" at approximately 2.1V if they were ever enabled.
*   **Impact:** This prevents the signal from reaching a valid logic "Low", causing PIO `WAIT 0` instructions to stall forever.
*   **Mitigation:** Explicitly call `gpio_disable_pulls()` on all capture pins and avoid `pio_gpio_init()` (which can sometimes re-enable default pulls) without subsequent pull-disable calls.

## 6. Current Implementation Strategy: "Total Manual Control"
To bypass SDK ambiguity and hardware quirks, the current working strategy is:
1.  **Forced GPIOBASE:** Manually write `16` to the `0x168` register of PIO1.
2.  **Relative PIO Assembly:** Use `WAIT PIN` instead of `WAIT GPIO` where possible, as `WAIT PIN` is always relative to `IN_BASE` and avoids the SDK's XOR remapping.
3.  **Direct Register Overrides:** After calling `pio_sm_init()`, manually overwrite the `PINCTRL` and `EXECCTRL` registers with the precisely calculated 5-bit relative indices.
4.  **USB Keep-Alive:** Include `tud_task()` in timeout loops to prevent the serial debug connection from dropping during capture stalls.

## 7. PIO Data Alignment & Shift Direction
*   **The Trap:** When `autopush` is enabled, the behavior of `IN` instructions depends on `shift_right`.
    *   **Shift Right (true):** Data enters at the MSB (bit 31) and shifts towards LSB. After 18 bits, the data resides in `ISR[31:14]`.
    *   **Shift Left (false):** Data enters at the LSB (bit 0) and shifts towards MSB. After 18 bits, the data resides in `ISR[17:0]`.
*   **Recommendation:** Use `shift_left` for capturing contiguous GPIO fields if your C-side extraction logic expects bits starting at index 0.

## 8. WAIT PIN vs WAIT GPIO (The GPIOBASE Trap)
*   **WAIT GPIO <n>:** On RP2350 with `GPIOBASE` set, the 5-bit instruction field is an **offset from GPIOBASE**. 
    *   Example: `WAIT 1 GPIO 43` with `GPIOBASE=16` actually waits for index `43 % 32 = 11`. Index 11 in that window is **GP27**.
*   **WAIT PIN <n>:** This is always relative to the state machine's **`IN_BASE`** (set in `PINCTRL`). It is much more predictable for Bank 1 capture as it bypasses the SDK's automatic instruction remapping and `GPIOBASE` modulo arithmetic.
*   **Recommendation:** Use `WAIT PIN` for all synchronization logic when working with Bank 1.

## 9. Diagnostic Log Reference
*   **PC Tracker:** Monitor the PIO Program Counter to see which `WAIT` instruction is stalling.
*   **Activity Map:** A 10ms software-based toggle counter to verify the MVS is physically sending pulses.
*   **Function Check:** Verify `gpio_get_function` returns `7` (PIO1) for all capture pins.

---
*Last Updated: Jan 1, 2026*







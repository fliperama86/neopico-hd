/**
 * PIO Program Counter Tracker
 *
 * Monitors PIO state machine execution in real-time.
 * Shows which instruction the PIO is stuck on and internal state.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

// Test configuration - monitors sync program on PIO1
#define TEST_PIO pio1
#define TEST_SM 0
#define PIN_CSYNC 46
#define PIN_PCLK 29

// Simple edge counter PIO program (same as in video_capture.pio)
static const uint16_t test_program[] = {
    0x20a0, // 0: wait 1 pin, 0      ; Wait for CSYNC High (GP46)
    0xa127, // 1: mov x, !null       ; Initialize counter
    0x202f, // 2: wait 0 pin, 15     ; Wait for PCLK Low (GP29)
    0x20af, // 3: wait 1 pin, 15     ; Wait for PCLK High
    0x0042, // 4: jmp x-- 2          ; Decrement and loop
};

static const struct pio_program test_pio_program = {
    .instructions = test_program,
    .length = 5,
    .origin = -1,
};

void print_pio_state(PIO pio, uint sm, uint offset) {
    // Read SM registers
    uint32_t addr = pio->sm[sm].addr;
    uint32_t execctrl = pio->sm[sm].execctrl;
    uint32_t pinctrl = pio->sm[sm].pinctrl;

    uint pc = addr & 0x1F;
    uint instruction = pio->instr_mem[pc];

    printf("\n=== PIO%d SM%d State ===\n", pio_get_index(pio), sm);
    printf("  PC: %u (offset+%u)\n", pc, pc - offset);
    printf("  Instruction at PC: 0x%04x\n", instruction);

    // Decode instruction
    uint opcode = instruction >> 13;
    const char *opcode_names[] = {
        "JMP", "WAIT", "IN", "OUT", "PUSH/PULL", "MOV", "IRQ", "SET"
    };
    printf("  Opcode: %s\n", opcode_names[opcode & 0x7]);

    if (opcode == 1) {  // WAIT
        uint pol = (instruction >> 7) & 1;
        uint source = (instruction >> 5) & 3;
        uint index = instruction & 0x1F;
        const char *sources[] = {"GPIO", "PIN", "IRQ", "???"};
        printf("  -> WAIT %d %s %u\n", pol, sources[source], index);
    }

    printf("  EXECCTRL: 0x%08lx (JMP_PIN=%lu)\n",
           execctrl, (execctrl >> 24) & 0x1F);
    printf("  PINCTRL:  0x%08lx (IN_BASE=%lu)\n",
           pinctrl, (pinctrl >> 15) & 0x1F);

    // Check FIFO
    uint32_t fstat = pio->fstat;
    bool rxfull = (fstat >> (sm + PIO_FSTAT_RXFULL_LSB)) & 1;
    bool rxempty = (fstat >> (sm + PIO_FSTAT_RXEMPTY_LSB)) & 1;
    printf("  RX FIFO: %s\n", rxfull ? "FULL" : (rxempty ? "EMPTY" : "HAS DATA"));
}

int main() {
    stdio_init_all();
    sleep_ms(5000);

    printf("\n\n=== PIO PC TRACKER ===\n");
    printf("Monitoring PIO execution to find stall point\n\n");

    // Force GPIOBASE
    printf("Setting GPIOBASE=16 for Bank 1 access...\n");
    *(volatile uint32_t*)((uintptr_t)TEST_PIO + 0x168) = 16;
    uint32_t actual_gpiobase = *(volatile uint32_t*)((uintptr_t)TEST_PIO + 0x168);
    printf("  Actual GPIOBASE: %lu\n", actual_gpiobase);

    // Setup GPIOs
    printf("\nInitializing GPIOs...\n");
    gpio_init(PIN_CSYNC);
    gpio_set_dir(PIN_CSYNC, GPIO_IN);
    gpio_disable_pulls(PIN_CSYNC);
    pio_gpio_init(TEST_PIO, PIN_CSYNC);

    gpio_init(PIN_PCLK);
    gpio_set_dir(PIN_PCLK, GPIO_IN);
    gpio_disable_pulls(PIN_PCLK);
    pio_gpio_init(TEST_PIO, PIN_PCLK);

    printf("  GP%d function: %d (should be 7 for PIO1)\n",
           PIN_CSYNC, gpio_get_function(PIN_CSYNC));
    printf("  GP%d function: %d (should be 7 for PIO1)\n",
           PIN_PCLK, gpio_get_function(PIN_PCLK));

    // Load program
    pio_clear_instruction_memory(TEST_PIO);
    uint offset = pio_add_program(TEST_PIO, &test_pio_program);
    printf("\nProgram loaded at offset %u\n", offset);

    // Claim and configure SM
    uint sm = pio_claim_unused_sm(TEST_PIO, true);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_clkdiv(&c, 1.0f);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    pio_sm_init(TEST_PIO, sm, offset, &c);

    // Manual register override
    printf("\nManual register configuration:\n");
    uint pin_idx_sync = 30;  // GP46 is index 30 (46 - 16)
    TEST_PIO->sm[sm].pinctrl =
        (TEST_PIO->sm[sm].pinctrl & ~0x000f8000) | (pin_idx_sync << 15);
    TEST_PIO->sm[sm].execctrl =
        (TEST_PIO->sm[sm].execctrl & ~0x1f000000) | (pin_idx_sync << 24);

    printf("  IN_BASE set to index %u (GP%u)\n", pin_idx_sync, 16 + pin_idx_sync);
    printf("  JMP_PIN set to index %u (GP%u)\n", pin_idx_sync, 16 + pin_idx_sync);

    // Read back to verify
    uint32_t pinctrl_actual = TEST_PIO->sm[sm].pinctrl;
    uint32_t in_base_actual = (pinctrl_actual >> 15) & 0x1F;
    printf("  Verified IN_BASE: %lu (GP%lu)\n", in_base_actual, 16 + in_base_actual);

    printf("\n=== Starting PIO ===\n");
    print_pio_state(TEST_PIO, sm, offset);

    pio_sm_set_enabled(TEST_PIO, sm, true);
    printf("\nPIO enabled. Monitoring for 5 seconds...\n");

    for (int i = 0; i < 10; i++) {
        sleep_ms(500);
        printf("\n--- T+%d.%ds ---", i/2, (i%2)*5);
        print_pio_state(TEST_PIO, sm, offset);

        // Check if PC is changing
        uint32_t pc1 = TEST_PIO->sm[sm].addr & 0x1F;
        sleep_ms(10);
        uint32_t pc2 = TEST_PIO->sm[sm].addr & 0x1F;

        if (pc1 == pc2) {
            printf("  ⚠️  PC NOT ADVANCING - STALLED at instruction %lu\n", pc1 - offset);

            // Try to identify why
            if ((pc1 - offset) == 0) {
                printf("     Stuck at: wait 1 pin, 0 (CSYNC High)\n");
                printf("     -> Check if GP46 is receiving CSYNC signal\n");
            } else if ((pc1 - offset) == 2 || (pc1 - offset) == 3) {
                printf("     Stuck waiting for PCLK\n");
                printf("     -> Check if GP29 is receiving PCLK signal\n");
            }
        } else {
            printf("  ✅ PC advancing (%lu -> %lu)\n", pc1 - offset, pc2 - offset);
        }
    }

    printf("\n=== Final State ===\n");
    print_pio_state(TEST_PIO, sm, offset);

    // Try reading FIFO
    uint32_t fifo_count = 0;
    while (!pio_sm_is_rx_fifo_empty(TEST_PIO, sm) && fifo_count < 5) {
        uint32_t val = pio_sm_get(TEST_PIO, sm);
        printf("FIFO value %lu: 0x%08lx\n", fifo_count++, val);
    }

    if (fifo_count == 0) {
        printf("❌ FIFO is empty - PIO is not producing data\n");
    } else {
        printf("✅ FIFO has %lu values\n", fifo_count);
    }

    printf("\nTest complete.\n");

    while (true) {
        tight_loop_contents();
    }
}

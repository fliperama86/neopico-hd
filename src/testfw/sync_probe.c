#include "testfw/sync_probe.h"

#include "pico/sync.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/timer.h"

#include "sync_probe.pio.h"

// Accept a broad range initially; HUD can refine once we see real data.
#define SYNC_PROBE_MIN_PERIOD_US 5U
#define SYNC_PROBE_MAX_PERIOD_US 1000U
#define SYNC_PROBE_IRQ_INDEX 0U
#define SYNC_PROBE_FRAME_GAP_US 90U
#define SYNC_PROBE_FRAME_MARKER_DEBOUNCE_US 8000U
#define SYNC_PROBE_FRAME_MIN_US 12000U
#define SYNC_PROBE_FRAME_MAX_US 25000U

static PIO s_pio = pio1;
static uint s_sm = 0;
static bool s_initialized = false;
static uint32_t s_pin_csync = 0;

static volatile uint32_t s_last_edge_ts_us = 0;
static volatile uint32_t s_edge_count_total = 0;
static volatile uint32_t s_last_period_us = 0;

// Window accumulators (reset on snapshot read)
static volatile uint32_t s_sum_period_us = 0;
static volatile uint32_t s_samples_window = 0;
static volatile uint32_t s_min_period_us = 0xFFFFFFFFU;
static volatile uint32_t s_max_period_us = 0U;

// Independent GPIO sampler stats (non-PIO).
static volatile uint32_t s_gpio_edges_total = 0;
static volatile uint32_t s_gpio_edges_window = 0;
static volatile uint32_t s_gpio_high_samples_window = 0;
static volatile uint32_t s_gpio_low_samples_window = 0;
static volatile uint32_t s_gpio_last_level = 0;
static volatile uint32_t s_pio_flag_polls_total = 0;
static volatile uint32_t s_pio_flag_polls_window = 0;
static volatile uint32_t s_lines_since_frame_gap = 0;
static volatile uint32_t s_last_frame_ts_us = 0;
static volatile uint32_t s_last_frame_period_us = 0;
static volatile uint32_t s_last_frame_lines = 0;
static volatile uint32_t s_frame_sum_period_us = 0;
static volatile uint32_t s_frame_samples_window = 0;

static inline void sync_probe_record_edge(uint32_t now_us)
{
    const uint32_t prev_us = s_last_edge_ts_us;
    s_last_edge_ts_us = now_us;
    s_edge_count_total++;

    if (prev_us == 0U) {
        return;
    }

    const uint32_t period_us = now_us - prev_us;
    if (period_us >= SYNC_PROBE_FRAME_GAP_US) {
        const uint32_t prev_frame_ts = s_last_frame_ts_us;
        const uint32_t since_prev_marker = (prev_frame_ts == 0U) ? 0U : (now_us - prev_frame_ts);
        if (prev_frame_ts == 0U || since_prev_marker >= SYNC_PROBE_FRAME_MARKER_DEBOUNCE_US) {
            if (prev_frame_ts != 0U) {
                const uint32_t frame_period_us = since_prev_marker;
                if (frame_period_us >= SYNC_PROBE_FRAME_MIN_US && frame_period_us <= SYNC_PROBE_FRAME_MAX_US) {
                    s_last_frame_period_us = frame_period_us;
                    s_last_frame_lines = s_lines_since_frame_gap;
                    s_frame_sum_period_us += frame_period_us;
                    s_frame_samples_window++;
                }
            }
            s_last_frame_ts_us = now_us;
            s_lines_since_frame_gap = 0;
        }
        // Do not feed frame-gap intervals into line-period statistics.
        return;
    }
    s_lines_since_frame_gap++;

    if (period_us < SYNC_PROBE_MIN_PERIOD_US || period_us > SYNC_PROBE_MAX_PERIOD_US) {
        return;
    }

    s_last_period_us = period_us;
    s_sum_period_us += period_us;
    s_samples_window++;
    if (period_us < s_min_period_us) {
        s_min_period_us = period_us;
    }
    if (period_us > s_max_period_us) {
        s_max_period_us = period_us;
    }
}

static void sync_probe_init(uint32_t pin_csync)
{
    if (s_initialized) {
        return;
    }

    // Match capture path mapping: use GPIOBASE=16 so GP27 becomes pin index 11.
    const uint32_t gpio_base = (pin_csync >= 16U) ? 16U : 0U;
    pio_set_gpio_base(s_pio, gpio_base);
    // Keep behavior aligned with working capture code on RP2350.
    *(volatile uint32_t *)((uintptr_t)s_pio + 0x168) = gpio_base;

    pio_gpio_init(s_pio, pin_csync);
    gpio_set_dir(pin_csync, GPIO_IN);
    gpio_disable_pulls(pin_csync);
    gpio_set_input_hysteresis_enabled(pin_csync, true);
    s_pin_csync = pin_csync;
    s_gpio_last_level = gpio_get(s_pin_csync) ? 1U : 0U;

    const uint prog_offs = pio_add_program(s_pio, &sync_probe_rise_program);
    s_sm = (uint)pio_claim_unused_sm(s_pio, true);

    pio_sm_config c = sync_probe_rise_program_get_default_config(prog_offs);
    sm_config_set_clkdiv(&c, 1.0F);
    pio_sm_init(s_pio, s_sm, prog_offs, &c);

    const uint32_t pin_idx = pin_csync - gpio_base;
    // WAIT PIN and JMP PIN decode via these SM fields.
    s_pio->sm[s_sm].pinctrl = (s_pio->sm[s_sm].pinctrl & ~0x000f8000U) | (pin_idx << 15);
    s_pio->sm[s_sm].execctrl = (s_pio->sm[s_sm].execctrl & ~0x1f000000U) | (pin_idx << 24);

    // Polled mode: do not depend on NVIC delivery for probe telemetry.
    pio_interrupt_clear(s_pio, SYNC_PROBE_IRQ_INDEX);
    s_pio->inte0 &= ~(1U << SYNC_PROBE_IRQ_INDEX);

    pio_sm_set_enabled(s_pio, s_sm, true);
    s_initialized = true;
}

static void sync_probe_poll_gpio(void)
{
    if (!s_initialized) {
        return;
    }

    // Burst-sample GPIO to detect transitions independently from PIO IRQ path.
    for (uint32_t i = 0; i < 128U; i++) {
        const uint32_t level = gpio_get(s_pin_csync) ? 1U : 0U;
        if (level) {
            s_gpio_high_samples_window++;
        } else {
            s_gpio_low_samples_window++;
        }

        if (level != s_gpio_last_level) {
            s_gpio_last_level = level;
            s_gpio_edges_total++;
            s_gpio_edges_window++;
        }
    }

    // Poll PIO IRQ flag directly to separate "PIO sees edge" from "NVIC handler runs".
    if (pio_interrupt_get(s_pio, SYNC_PROBE_IRQ_INDEX)) {
        pio_interrupt_clear(s_pio, SYNC_PROBE_IRQ_INDEX);
        s_pio_flag_polls_total++;
        s_pio_flag_polls_window++;
        sync_probe_record_edge(timer_hw->timerawl);
    }
}

static void sync_probe_get_snapshot(const sync_probe_snapshot_t *out)
{
    if (!out) {
        return;
    }

    uint32_t edge_total;
    uint32_t samples;
    uint32_t last_period;
    uint32_t sum;
    uint32_t min_p;
    uint32_t max_p;
    uint32_t gpio_edges_total;
    uint32_t gpio_edges_window;
    uint32_t gpio_high_samples_window;
    uint32_t gpio_low_samples_window;
    uint32_t pio_flag_polls_total;
    uint32_t pio_flag_polls_window;
    uint32_t frame_last_period_us;
    uint32_t frame_samples_window;
    uint32_t frame_sum_period_us;
    uint32_t frame_lines_last;

    const uint32_t irq_state = save_and_disable_interrupts();
    edge_total = s_edge_count_total;
    samples = s_samples_window;
    last_period = s_last_period_us;
    sum = s_sum_period_us;
    min_p = s_min_period_us;
    max_p = s_max_period_us;
    gpio_edges_total = s_gpio_edges_total;
    gpio_edges_window = s_gpio_edges_window;
    gpio_high_samples_window = s_gpio_high_samples_window;
    gpio_low_samples_window = s_gpio_low_samples_window;
    pio_flag_polls_total = s_pio_flag_polls_total;
    pio_flag_polls_window = s_pio_flag_polls_window;
    frame_last_period_us = s_last_frame_period_us;
    frame_samples_window = s_frame_samples_window;
    frame_sum_period_us = s_frame_sum_period_us;
    frame_lines_last = s_last_frame_lines;

    s_samples_window = 0;
    s_sum_period_us = 0;
    s_min_period_us = 0xFFFFFFFFU;
    s_max_period_us = 0U;
    s_gpio_edges_window = 0;
    s_gpio_high_samples_window = 0;
    s_gpio_low_samples_window = 0;
    s_pio_flag_polls_window = 0;
    s_frame_samples_window = 0;
    s_frame_sum_period_us = 0;
    restore_interrupts(irq_state);

    out->edge_count_total = edge_total;
    out->samples_window = samples;
    out->last_period_us = last_period;
    out->valid_window = (samples > 0U);
    if (samples > 0U) {
        const uint32_t avg = sum / samples;
        out->avg_period_us = avg;
        out->min_period_us = min_p;
        out->max_period_us = max_p;
        // kHz * 100 (e.g. 1573 => 15.73 kHz)
        out->line_hz_x100 = (avg > 0U) ? (100000U / avg) : 0U;
    } else {
        out->avg_period_us = 0U;
        out->min_period_us = 0U;
        out->max_period_us = 0U;
        out->line_hz_x100 = 0U;
    }
    out->gpio_edges_total = gpio_edges_total;
    out->gpio_edges_window = gpio_edges_window;
    out->gpio_high_samples_window = gpio_high_samples_window;
    out->gpio_low_samples_window = gpio_low_samples_window;
    out->pio_flag_polls_total = pio_flag_polls_total;
    out->pio_flag_polls_window = pio_flag_polls_window;
    out->frame_last_period_us = frame_last_period_us;
    out->frame_lines_last = frame_lines_last;
    out->frame_samples_window = frame_samples_window;
    out->valid_frame_window = (frame_samples_window > 0U);
    if (frame_samples_window > 0U) {
        const uint32_t frame_avg = frame_sum_period_us / frame_samples_window;
        out->frame_avg_period_us = frame_avg;
        out->frame_hz_x100 = (frame_avg > 0U) ? (100000000U / frame_avg) : 0U;
    } else {
        out->frame_avg_period_us = 0U;
        out->frame_hz_x100 = 0U;
    }
}

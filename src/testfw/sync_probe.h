#ifndef TESTFW_SYNC_PROBE_H
#define TESTFW_SYNC_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t edge_count_total;
    uint32_t samples_window;
    uint32_t last_period_us;
    uint32_t avg_period_us;
    uint32_t min_period_us;
    uint32_t max_period_us;
    uint32_t line_hz_x100;
    uint32_t gpio_edges_total;
    uint32_t gpio_edges_window;
    uint32_t gpio_high_samples_window;
    uint32_t gpio_low_samples_window;
    uint32_t pio_flag_polls_total;
    uint32_t pio_flag_polls_window;
    uint32_t frame_last_period_us;
    uint32_t frame_avg_period_us;
    uint32_t frame_hz_x100;
    uint32_t frame_lines_last;
    uint32_t frame_samples_window;
    bool valid_window;
    bool valid_frame_window;
} sync_probe_snapshot_t;

void sync_probe_init(uint32_t pin_csync);
void sync_probe_poll_gpio(void);
void sync_probe_get_snapshot(sync_probe_snapshot_t *out);

#endif // TESTFW_SYNC_PROBE_H

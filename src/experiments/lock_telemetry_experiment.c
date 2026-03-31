#include "lock_telemetry_experiment.h"

#include "pico_hdmi/video_output_rt.h"

#include "pico/time.h"

#include "hardware/clocks.h"

#include <stdint.h>
#include <string.h>

#include "osd/fast_osd.h"
#include "testfw/sync_probe.h"

typedef struct {
    uint32_t last_update_ms;
    uint32_t last_out_frames;
    uint32_t last_control_ms;
    uint32_t last_coarse_ms;
    uint16_t vtotal_cmd;
    uint16_t vtotal_min_cmd;
    uint16_t vtotal_max_cmd;
    uint32_t sysclk_khz_cmd;
    int8_t last_err_sign;
    uint8_t err_sign_streak;
    uint32_t act_sticky_until_ms;
    const char *act_sticky;
    bool vtrim_initialized;
} lock_telemetry_state_t;

static lock_telemetry_state_t s_state;

#define TESTFW_VTRIM_VTOTAL_MIN_FLOOR 200U
#define TESTFW_VTRIM_VTOTAL_MAX_CEIL 800U
#define TESTFW_VTRIM_WINDOW 20U
#define TESTFW_VTRIM_DEADBAND_X100 20U
#define TESTFW_VTRIM_START_HOLDOFF_MS 3000U
#define TESTFW_VTRIM_STEP_PERIOD_MS 1000U
#define TESTFW_VTRIM_SIGN_STREAK_REQUIRED 3U
#define TESTFW_ACT_STICKY_MS 1500U

#define TESTFW_SYSCLK_MIN_KHZ 123000U
#define TESTFW_SYSCLK_MAX_KHZ 127000U
#define TESTFW_SYSCLK_STEP_KHZ 25U
#define TESTFW_COARSE_STEP_PERIOD_MS 3000U
#define TESTFW_COARSE_ERR_X100 150U

static void telemetry_clear_row(uint8_t row)
{
    if (row >= FAST_OSD_ROWS) {
        return;
    }
    fast_osd_puts(row, 0, "                            ");
}

static void telemetry_write_row(uint8_t row, const char *text)
{
    telemetry_clear_row(row);
    fast_osd_puts(row, 0, text);
}

static uint8_t telemetry_u32_to_dec(char *dst, uint32_t value)
{
    char rev[10];
    uint8_t n = 0;
    if (value == 0U) {
        dst[0] = '0';
        return 1U;
    }
    while (value > 0U && n < (uint8_t)sizeof(rev)) {
        rev[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    for (uint8_t i = 0; i < n; i++) {
        dst[i] = rev[n - 1U - i];
    }
    return n;
}

static void telemetry_put_label_u32(uint8_t row, const char *label, uint32_t value)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));
    while (*label && pos < sizeof(line)) {
        line[pos++] = *label++;
    }
    if (pos < sizeof(line)) {
        pos += telemetry_u32_to_dec(&line[pos], value);
    }
    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static void telemetry_put_fps(uint8_t row, uint32_t out_fps_x100)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *a = "out:";
    while (*a && pos < sizeof(line)) {
        line[pos++] = *a++;
    }
    pos += telemetry_u32_to_dec(&line[pos], out_fps_x100 / 100U);
    if (pos + 2U < sizeof(line)) {
        line[pos++] = '.';
        line[pos++] = (char)('0' + ((out_fps_x100 / 10U) % 10U));
        line[pos++] = (char)('0' + (out_fps_x100 % 10U));
    }

    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static void telemetry_put_mode(uint8_t row, uint16_t h, uint16_t v)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *prefix = "mode:";
    while (*prefix && pos < sizeof(line)) {
        line[pos++] = *prefix++;
    }
    pos += telemetry_u32_to_dec(&line[pos], h);
    if (pos < sizeof(line)) {
        line[pos++] = 'x';
    }
    pos += telemetry_u32_to_dec(&line[pos], v);
    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static void telemetry_put_frame(uint8_t row, uint32_t out_frames)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *a = "ofrm:";
    while (*a && pos < sizeof(line)) {
        line[pos++] = *a++;
    }
    pos += telemetry_u32_to_dec(&line[pos], out_frames);
    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static void telemetry_put_probe_line(uint8_t row, const sync_probe_snapshot_t *ss)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *prefix = "line:";
    while (*prefix && pos < sizeof(line)) {
        line[pos++] = *prefix++;
    }
    if (ss->line_hz_x100 > 0U) {
        pos += telemetry_u32_to_dec(&line[pos], ss->line_hz_x100 / 100U);
        if (pos + 2U < sizeof(line)) {
            line[pos++] = '.';
            line[pos++] = (char)('0' + ((ss->line_hz_x100 / 10U) % 10U));
            line[pos++] = (char)('0' + (ss->line_hz_x100 % 10U));
        }
    } else if (pos + 3U < sizeof(line)) {
        line[pos++] = 'n';
        line[pos++] = '/';
        line[pos++] = 'a';
    }
    if (pos + 2U < sizeof(line)) {
        line[pos++] = 'k';
        line[pos++] = 'H';
        line[pos++] = 'z';
    }
    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static void telemetry_put_probe_frame(uint8_t row, const sync_probe_snapshot_t *ss)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *prefix = "in frm:";
    while (*prefix && pos < sizeof(line)) {
        line[pos++] = *prefix++;
    }

    // Prefer marker-based frame rate only when it is in a plausible range.
    // Otherwise use a robust line-based estimate for Neo Geo (~272 total lines).
    bool marker_plausible = false;
    if (ss->frame_hz_x100 >= 5500U && ss->frame_hz_x100 <= 6200U && ss->frame_lines_last >= 240U &&
        ss->frame_lines_last <= 320U) {
        marker_plausible = true;
    }

    uint32_t frame_hz_x100 = marker_plausible ? ss->frame_hz_x100 : 0U;
    if (frame_hz_x100 == 0U && ss->line_hz_x100 > 0U) {
        // Line-based estimator: frame_hz = line_hz / 272.
        frame_hz_x100 = (ss->line_hz_x100 * 1000U) / 272U;
    }

    if (frame_hz_x100 > 0U) {
        pos += telemetry_u32_to_dec(&line[pos], frame_hz_x100 / 100U);
        if (pos + 2U < sizeof(line)) {
            line[pos++] = '.';
            line[pos++] = (char)('0' + ((frame_hz_x100 / 10U) % 10U));
            line[pos++] = (char)('0' + (frame_hz_x100 % 10U));
        }
        if (marker_plausible && ss->frame_lines_last > 0U && pos + 3U < sizeof(line)) {
            line[pos++] = ' ';
            line[pos++] = 'l';
            line[pos++] = ':';
            pos += telemetry_u32_to_dec(&line[pos], ss->frame_lines_last);
        }
    } else if (pos + 3U < sizeof(line)) {
        line[pos++] = 'n';
        line[pos++] = '/';
        line[pos++] = 'a';
    }

    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static uint32_t telemetry_estimate_input_fps_x100(const sync_probe_snapshot_t *ss)
{
    if (ss->frame_hz_x100 >= 5500U && ss->frame_hz_x100 <= 6200U && ss->frame_lines_last >= 240U &&
        ss->frame_lines_last <= 320U) {
        return ss->frame_hz_x100;
    }
    if (ss->line_hz_x100 > 0U) {
        // Line-based estimator: frame_hz = line_hz / 272.
        return (ss->line_hz_x100 * 1000U) / 272U;
    }
    return 0U;
}

static void telemetry_put_trim_status(uint8_t row, uint32_t in_fps_x100, uint16_t vtotal_cmd)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *prefix = "ctl in:";
    while (*prefix && pos < sizeof(line)) {
        line[pos++] = *prefix++;
    }

    if (in_fps_x100 > 0U) {
        pos += telemetry_u32_to_dec(&line[pos], in_fps_x100 / 100U);
        if (pos + 2U < sizeof(line)) {
            line[pos++] = '.';
            line[pos++] = (char)('0' + ((in_fps_x100 / 10U) % 10U));
            line[pos++] = (char)('0' + (in_fps_x100 % 10U));
        }
    } else if (pos + 3U < sizeof(line)) {
        line[pos++] = 'n';
        line[pos++] = '/';
        line[pos++] = 'a';
    }

    if (pos + 4U < sizeof(line)) {
        line[pos++] = ' ';
        line[pos++] = 'v';
        line[pos++] = 't';
        line[pos++] = ':';
        pos += telemetry_u32_to_dec(&line[pos], vtotal_cmd);
    }

    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static uint32_t telemetry_get_pixel_clock_hz(void)
{
    const video_mode_t *mode = video_output_active_mode;
    return clock_get_hz(clk_sys) / ((uint32_t)mode->hstx_clk_div * (uint32_t)mode->hstx_csr_clkdiv);
}

static void telemetry_put_vt_clk(uint8_t row, uint16_t vt, uint32_t clk_khz)
{
    char line[28];
    uint8_t pos = 0;
    memset(line, ' ', sizeof(line));

    const char *a = "vt:";
    while (*a && pos < sizeof(line)) {
        line[pos++] = *a++;
    }
    pos += telemetry_u32_to_dec(&line[pos], vt);

    if (pos + 4U < sizeof(line)) {
        line[pos++] = ' ';
        line[pos++] = 'c';
        line[pos++] = 'k';
        line[pos++] = ':';
    }
    pos += telemetry_u32_to_dec(&line[pos], clk_khz);

    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

static void telemetry_put_err_action(uint8_t row, int32_t err_x100, const char *act, uint8_t gate)
{
    char line[28];
    uint8_t pos = 0;
    uint32_t abs_x100;
    memset(line, ' ', sizeof(line));

    const char *prefix = "e:";
    while (*prefix && pos < sizeof(line)) {
        line[pos++] = *prefix++;
    }

    if (err_x100 < 0) {
        if (pos < sizeof(line)) {
            line[pos++] = '-';
        }
        abs_x100 = (uint32_t)(-err_x100);
    } else {
        if (pos < sizeof(line)) {
            line[pos++] = '+';
        }
        abs_x100 = (uint32_t)err_x100;
    }

    pos += telemetry_u32_to_dec(&line[pos], abs_x100 / 100U);
    if (pos + 2U < sizeof(line)) {
        line[pos++] = '.';
        line[pos++] = (char)('0' + ((abs_x100 / 10U) % 10U));
        line[pos++] = (char)('0' + (abs_x100 % 10U));
    }

    if (pos + 4U < sizeof(line)) {
        line[pos++] = ' ';
        line[pos++] = 'a';
        line[pos++] = ':';
    }
    while (*act && pos < sizeof(line)) {
        line[pos++] = *act++;
    }

    if (pos + 3U < sizeof(line)) {
        line[pos++] = ' ';
        line[pos++] = 'g';
        line[pos++] = ':';
        pos += telemetry_u32_to_dec(&line[pos], gate);
    }

    line[sizeof(line) - 1U] = '\0';
    telemetry_write_row(row, line);
}

void lock_telemetry_experiment_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.act_sticky = "--";

    fast_osd_clear();
    osd_show();

    telemetry_write_row(0, "NEOPICO TESTFW");
    telemetry_write_row(1, "gray+OSD rt lock test");
    telemetry_write_row(2, "gpio edges w: 0");
    telemetry_write_row(15, "in frm: n/a");
}

void lock_telemetry_experiment_tick_background(void)
{
    sync_probe_poll_gpio();

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if ((now_ms - s_state.last_update_ms) < 500U) {
        return;
    }

    const uint32_t out_frames = video_frame_count;
    const uint32_t elapsed_ms = (s_state.last_update_ms == 0U) ? 0U : (now_ms - s_state.last_update_ms);
    const uint32_t out_delta = out_frames - s_state.last_out_frames;

    s_state.last_update_ms = now_ms;
    s_state.last_out_frames = out_frames;

    uint32_t out_fps_x100 = 0;
    if (elapsed_ms > 0U) {
        out_fps_x100 = (out_delta * 100000U) / elapsed_ms;
    }

    sync_probe_snapshot_t ss;
    memset(&ss, 0, sizeof(ss));
    sync_probe_get_snapshot(&ss);
    const uint32_t in_fps_x100 = telemetry_estimate_input_fps_x100(&ss);
    int32_t err_x100 = 0;
    const char *act_now = "HD";

    if (!s_state.vtrim_initialized) {
        s_state.vtotal_cmd = rt_v_total_lines;
        uint32_t lo = (s_state.vtotal_cmd > TESTFW_VTRIM_WINDOW) ? (uint32_t)s_state.vtotal_cmd - TESTFW_VTRIM_WINDOW
                                                                 : TESTFW_VTRIM_VTOTAL_MIN_FLOOR;
        uint32_t hi = (uint32_t)s_state.vtotal_cmd + TESTFW_VTRIM_WINDOW;
        if (lo < TESTFW_VTRIM_VTOTAL_MIN_FLOOR) {
            lo = TESTFW_VTRIM_VTOTAL_MIN_FLOOR;
        }
        if (hi > TESTFW_VTRIM_VTOTAL_MAX_CEIL) {
            hi = TESTFW_VTRIM_VTOTAL_MAX_CEIL;
        }
        s_state.vtotal_min_cmd = (uint16_t)lo;
        s_state.vtotal_max_cmd = (uint16_t)hi;
        s_state.last_control_ms = now_ms;
        s_state.last_coarse_ms = now_ms;
        s_state.sysclk_khz_cmd = clock_get_hz(clk_sys) / 1000U;
        s_state.vtrim_initialized = true;
    }

    // Track the real applied runtime vtotal (library may clamp internally).
    s_state.vtotal_cmd = rt_v_total_lines;

    if (elapsed_ms > 0U && in_fps_x100 > 0U && out_fps_x100 > 0U) {
        err_x100 = (int32_t)in_fps_x100 - (int32_t)out_fps_x100;
        int8_t err_sign = 0;
        if (err_x100 > (int32_t)TESTFW_VTRIM_DEADBAND_X100) {
            err_sign = 1;
        } else if (err_x100 < -(int32_t)TESTFW_VTRIM_DEADBAND_X100) {
            err_sign = -1;
        }

        if (err_sign == 0) {
            s_state.last_err_sign = 0;
            s_state.err_sign_streak = 0;
            act_now = "DB";
        } else {
            if (err_sign == s_state.last_err_sign) {
                if (s_state.err_sign_streak < 255U) {
                    s_state.err_sign_streak++;
                }
            } else {
                s_state.last_err_sign = err_sign;
                s_state.err_sign_streak = 1U;
            }

            if (now_ms < TESTFW_VTRIM_START_HOLDOFF_MS) {
                act_now = "ST";
            } else if ((now_ms - s_state.last_control_ms) < TESTFW_VTRIM_STEP_PERIOD_MS) {
                act_now = "SH";
            } else if (s_state.err_sign_streak < TESTFW_VTRIM_SIGN_STREAK_REQUIRED) {
                act_now = "GT";
            } else if (err_sign > 0) {
                if (s_state.vtotal_cmd > s_state.vtotal_min_cmd) {
                    s_state.vtotal_cmd--;
                    rt_v_total_lines = s_state.vtotal_cmd;
                    act_now = "S-";
                } else {
                    act_now = "MN";
                }
                s_state.last_control_ms = now_ms;
                s_state.last_err_sign = 0;
                s_state.err_sign_streak = 0;
            } else {
                if (s_state.vtotal_cmd < s_state.vtotal_max_cmd) {
                    s_state.vtotal_cmd++;
                    rt_v_total_lines = s_state.vtotal_cmd;
                    act_now = "S+";
                } else {
                    act_now = "MX";
                }
                s_state.last_control_ms = now_ms;
                s_state.last_err_sign = 0;
                s_state.err_sign_streak = 0;
            }
        }

        const bool near_vtotal_limit = (s_state.vtotal_cmd <= (uint16_t)(s_state.vtotal_min_cmd + 1U)) ||
                                       (s_state.vtotal_cmd >= (uint16_t)(s_state.vtotal_max_cmd - 1U));
        if (now_ms >= TESTFW_VTRIM_START_HOLDOFF_MS &&
            (now_ms - s_state.last_coarse_ms) >= TESTFW_COARSE_STEP_PERIOD_MS && near_vtotal_limit &&
            (err_x100 > (int32_t)TESTFW_COARSE_ERR_X100 || err_x100 < -(int32_t)TESTFW_COARSE_ERR_X100)) {
            if (err_x100 > 0) {
                if (s_state.sysclk_khz_cmd < TESTFW_SYSCLK_MAX_KHZ) {
                    s_state.sysclk_khz_cmd += TESTFW_SYSCLK_STEP_KHZ;
                    set_sys_clock_khz(s_state.sysclk_khz_cmd, true);
                    video_output_reconfigure_clock();
                    video_output_update_acr(telemetry_get_pixel_clock_hz());
                    act_now = "K+";
                }
            } else {
                if (s_state.sysclk_khz_cmd > TESTFW_SYSCLK_MIN_KHZ) {
                    s_state.sysclk_khz_cmd -= TESTFW_SYSCLK_STEP_KHZ;
                    set_sys_clock_khz(s_state.sysclk_khz_cmd, true);
                    video_output_reconfigure_clock();
                    video_output_update_acr(telemetry_get_pixel_clock_hz());
                    act_now = "K-";
                }
            }
            s_state.last_coarse_ms = now_ms;
        }
    } else {
        act_now = "NE";
        s_state.last_err_sign = 0;
        s_state.err_sign_streak = 0;
    }

    // Keep last action visible briefly for easier on-screen reading.
    if (act_now[0] == 'S' || act_now[0] == 'M') {
        s_state.act_sticky = act_now;
        s_state.act_sticky_until_ms = now_ms + TESTFW_ACT_STICKY_MS;
    } else if (now_ms >= s_state.act_sticky_until_ms) {
        s_state.act_sticky = act_now;
    }

    telemetry_put_label_u32(2, "gpio edges w:", ss.gpio_edges_window);
    telemetry_put_label_u32(3, "pio flag w:", ss.pio_flag_polls_window);
    telemetry_put_fps(4, out_fps_x100);
    telemetry_put_trim_status(5, in_fps_x100, s_state.vtotal_cmd);
    telemetry_put_err_action(6, err_x100, s_state.act_sticky, s_state.err_sign_streak);
    telemetry_put_vt_clk(7, rt_v_total_lines, clock_get_hz(clk_sys) / 1000U);
    telemetry_put_probe_line(8, &ss);
    if (ss.valid_window) {
        telemetry_put_label_u32(9, "per avg us:", ss.avg_period_us);
        telemetry_put_label_u32(10, "per min us:", ss.min_period_us);
        telemetry_put_label_u32(11, "per max us:", ss.max_period_us);
        telemetry_put_label_u32(12, "edges win:", ss.samples_window);
        telemetry_put_label_u32(13, "pio edges t:", ss.edge_count_total);
        telemetry_put_label_u32(14, "last per us:", ss.last_period_us);
    } else {
        telemetry_write_row(9, "per avg us: n/a");
        telemetry_write_row(10, "per min us: n/a");
        telemetry_write_row(11, "per max us: n/a");
        telemetry_write_row(12, "edges win: 0");
        telemetry_put_label_u32(13, "pio edges t:", ss.edge_count_total);
        telemetry_put_label_u32(14, "last per us:", ss.last_period_us);
    }
    telemetry_put_probe_frame(15, &ss);
}

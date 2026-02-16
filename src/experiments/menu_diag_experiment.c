#include "menu_diag_experiment.h"

#include "pico/time.h"

#include "hardware/gpio.h"

#include "mvs_pins.h"
#include "osd/fast_osd.h"
#include "osd/selftest_layout.h"

#if EXP_MENU_DIAG
// Global frame counter from video output runtime.
extern volatile uint32_t video_frame_count;

static bool s_btn_was_pressed = false;
static uint32_t s_last_press_ms = 0;
static uint32_t s_last_update_frame = 0;
#endif

void menu_diag_experiment_init(void)
{
#if EXP_MENU_DIAG
    s_btn_was_pressed = false;
    s_last_press_ms = 0;
    s_last_update_frame = video_frame_count;
#endif
}

void menu_diag_experiment_on_menu_open(void)
{
#if EXP_MENU_DIAG
    selftest_layout_reset();
    s_last_update_frame = video_frame_count;
#endif
}

void menu_diag_experiment_on_menu_close(void)
{
#if EXP_MENU_DIAG
    // Keep existing OSD buffer contents; visibility controls display.
#endif
}

void menu_diag_experiment_tick_background(void)
{
#if EXP_MENU_DIAG
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool btn_pressed = !gpio_get(PIN_OSD_BTN_MENU); // active low

    // Simple edge + debounce handling on Core1 background tick.
    if (btn_pressed && !s_btn_was_pressed && (now_ms - s_last_press_ms) >= 200U) {
        s_last_press_ms = now_ms;
        osd_toggle();
        if (osd_visible) {
            menu_diag_experiment_on_menu_open();
        } else {
            menu_diag_experiment_on_menu_close();
        }
    }
    s_btn_was_pressed = btn_pressed;

    if (osd_visible && (video_frame_count - s_last_update_frame) >= 60U) {
        s_last_update_frame = video_frame_count;
        // Static diagnostics experiment: no live signal sampling yet.
        selftest_layout_update(video_frame_count, false, 0);
    }
#endif
}

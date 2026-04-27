#include "menu_diag_experiment.h"

#if NEOPICO_ENABLE_OSD

#include "pico/time.h"

#include "hardware/gpio.h"

#include "mvs_pins.h"
#include "osd/fast_osd.h"
#include "video_pipeline.h"
#if NEOPICO_ENABLE_SELFTEST
#include "osd/selftest_layout.h"

#define SELFTEST_SHADOW_HOLD_UPDATES 30U
#endif

#ifndef NEOPICO_EXP_REBOOT_MODE_SWITCH
#define NEOPICO_EXP_REBOOT_MODE_SWITCH 0
#endif

#ifndef NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
#define NEOPICO_EXP_REBOOT_MODE_SWITCH_720P 0
#endif

#ifndef NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI
#define NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI 0
#endif

#ifndef NEOPICO_EXP_RAM_SELECTOR_UI
#define NEOPICO_EXP_RAM_SELECTOR_UI 0
#endif

#ifndef NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY
#define NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY 0
#endif

#ifndef NEOPICO_EXP_STATIC_OSD_SELECT_ONLY
#define NEOPICO_EXP_STATIC_OSD_SELECT_ONLY 0
#endif

#ifndef NEOPICO_EXP_STATIC_OSD_APPLY
#define NEOPICO_EXP_STATIC_OSD_APPLY 0
#endif

#ifndef NEOPICO_EXP_RAM_OSD_APPLY_PATH
#define NEOPICO_EXP_RAM_OSD_APPLY_PATH 0
#endif

#if NEOPICO_EXP_RAM_SELECTOR_UI
#define SELECTOR_UI_RAM(name) __not_in_flash_func(name)
#else
#define SELECTOR_UI_RAM(name) name
#endif

#if NEOPICO_EXP_RAM_OSD_APPLY_PATH
#define SELECTOR_UI_APPLY_RAM(name) __no_inline_not_in_flash_func(name)
#else
#define SELECTOR_UI_APPLY_RAM(name) SELECTOR_UI_RAM(name)
#endif

#define NEOPICO_REBOOT_SELECTOR_UI                                                                                     \
    (NEOPICO_EXP_REBOOT_MODE_SWITCH && !NEOPICO_ENABLE_SELFTEST && !NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI)

#define NEOPICO_STATIC_OSD_TOGGLE_ONLY (NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY && NEOPICO_REBOOT_SELECTOR_UI)
#define NEOPICO_STATIC_OSD_SELECT_ONLY (NEOPICO_EXP_STATIC_OSD_SELECT_ONLY && NEOPICO_REBOOT_SELECTOR_UI)
#define NEOPICO_STATIC_OSD_APPLY (NEOPICO_EXP_STATIC_OSD_APPLY && NEOPICO_REBOOT_SELECTOR_UI)
#define NEOPICO_STATIC_OSD_PRERENDER                                                                                   \
    ((NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY || NEOPICO_EXP_STATIC_OSD_SELECT_ONLY || NEOPICO_EXP_STATIC_OSD_APPLY) &&     \
     NEOPICO_REBOOT_SELECTOR_UI)

#if NEOPICO_REBOOT_SELECTOR_UI
#define RES_SELECTOR_TITLE_ROW 1
#define RES_SELECTOR_CURRENT_ROW 4
#define RES_SELECTOR_FIRST_OPTION_ROW 7
#define RES_SELECTOR_HINT_ROW 13
#endif

// Global frame counter from video output runtime.
extern volatile uint32_t video_frame_count;

static bool s_btn_was_pressed = false;
static uint32_t s_last_press_ms = 0;
#if NEOPICO_EXP_REBOOT_MODE_SWITCH
static bool s_back_was_pressed = false;
static uint32_t s_last_back_press_ms = 0;
#if NEOPICO_REBOOT_SELECTOR_UI
static video_pipeline_reboot_mode_t s_selected_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
#endif
#endif
#if NEOPICO_ENABLE_SELFTEST
static uint32_t s_last_update_frame = 0;
static uint32_t s_video_hi = 0;
static uint32_t s_video_lo = 0;
static uint32_t s_video_samples = 0;
static uint32_t s_audio_hi = 0;
static uint32_t s_audio_lo = 0;
static uint32_t s_audio_samples = 0;
static uint32_t s_shadow_hold_updates = 0;
#endif

#if NEOPICO_REBOOT_SELECTOR_UI
static const char *SELECTOR_UI_RAM(resolution_label)(video_pipeline_reboot_mode_t mode)
{
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return "240p";
        case VIDEO_PIPELINE_REBOOT_MODE_720P:
            return "720p";
        default:
            return "480p";
    }
}

static video_pipeline_reboot_mode_t SELECTOR_UI_RAM(resolution_next)(video_pipeline_reboot_mode_t mode)
{
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            return VIDEO_PIPELINE_REBOOT_MODE_240P;
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return VIDEO_PIPELINE_REBOOT_MODE_720P;
#endif
        default:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
}

static bool SELECTOR_UI_RAM(resolution_selector_option_row)(video_pipeline_reboot_mode_t mode, uint8_t *row)
{
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            *row = RES_SELECTOR_FIRST_OPTION_ROW;
            return true;
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            *row = RES_SELECTOR_FIRST_OPTION_ROW + 2;
            return true;
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
        case VIDEO_PIPELINE_REBOOT_MODE_720P:
            *row = RES_SELECTOR_FIRST_OPTION_ROW + 4;
            return true;
#endif
        default:
            return false;
    }
}

static void SELECTOR_UI_RAM(resolution_selector_render_option)(uint8_t row, video_pipeline_reboot_mode_t mode)
{
    const bool selected = (s_selected_mode == mode);
    const bool current = (video_pipeline_reboot_requested_mode() == mode);
    const uint16_t color = selected ? OSD_COLOR_YELLOW : current ? OSD_COLOR_GREEN : OSD_COLOR_FG;
    fast_osd_putc_color(row, 3, selected ? '>' : ' ', color);
    fast_osd_puts_color(row, 5, resolution_label(mode), color);
    if (current) {
        fast_osd_puts_color(row, 11, "current", OSD_COLOR_GREEN);
    }
}

static void SELECTOR_UI_RAM(resolution_selector_render_option_mode)(video_pipeline_reboot_mode_t mode)
{
    uint8_t row = 0;
    if (resolution_selector_option_row(mode, &row)) {
        resolution_selector_render_option(row, mode);
    }
}

static void SELECTOR_UI_RAM(resolution_selector_update_selection)(video_pipeline_reboot_mode_t previous_mode)
{
    if (previous_mode == s_selected_mode) {
        return;
    }
    resolution_selector_render_option_mode(previous_mode);
    resolution_selector_render_option_mode(s_selected_mode);
}

static void SELECTOR_UI_RAM(resolution_selector_render_full)(void)
{
    fast_osd_clear();
    fast_osd_puts_color(RES_SELECTOR_TITLE_ROW, 2, "NeoPico-HD Output", OSD_COLOR_YELLOW);
    fast_osd_puts_color(RES_SELECTOR_CURRENT_ROW, 2, "Current:", OSD_COLOR_GRAY);
    fast_osd_puts_color(RES_SELECTOR_CURRENT_ROW, 11, resolution_label(video_pipeline_reboot_requested_mode()),
                        OSD_COLOR_GREEN);
    fast_osd_puts_color(RES_SELECTOR_FIRST_OPTION_ROW - 2, 2, "Resolution", OSD_COLOR_FG);

    resolution_selector_render_option(RES_SELECTOR_FIRST_OPTION_ROW, VIDEO_PIPELINE_REBOOT_MODE_480P);
    resolution_selector_render_option(RES_SELECTOR_FIRST_OPTION_ROW + 2, VIDEO_PIPELINE_REBOOT_MODE_240P);
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
    resolution_selector_render_option(RES_SELECTOR_FIRST_OPTION_ROW + 4, VIDEO_PIPELINE_REBOOT_MODE_720P);
#endif

#if NEOPICO_STATIC_OSD_APPLY
    fast_osd_puts_color(RES_SELECTOR_HINT_ROW, 2, "MENU apply BACK move", OSD_COLOR_GRAY);
#elif NEOPICO_STATIC_OSD_SELECT_ONLY
    fast_osd_puts_color(RES_SELECTOR_HINT_ROW, 2, "MENU show/hide BACK move", OSD_COLOR_GRAY);
#elif NEOPICO_STATIC_OSD_TOGGLE_ONLY
    fast_osd_puts_color(RES_SELECTOR_HINT_ROW, 2, "MENU show/hide", OSD_COLOR_GRAY);
#else
    fast_osd_puts_color(RES_SELECTOR_HINT_ROW, 2, "MENU apply  BACK change", OSD_COLOR_GRAY);
#endif
}

static void SELECTOR_UI_APPLY_RAM(resolution_selector_apply)(void)
{
    if (s_selected_mode == video_pipeline_reboot_requested_mode()) {
        osd_hide();
        menu_diag_experiment_on_menu_close();
        return;
    }
    osd_hide();
    menu_diag_experiment_on_menu_close();
    video_pipeline_request_reboot_mode(s_selected_mode);
}
#endif

void menu_diag_experiment_init(void)
{
    s_btn_was_pressed = false;
    s_last_press_ms = 0;
#if NEOPICO_EXP_REBOOT_MODE_SWITCH
    s_back_was_pressed = false;
    s_last_back_press_ms = 0;
#if NEOPICO_REBOOT_SELECTOR_UI
    s_selected_mode = video_pipeline_reboot_requested_mode();
#if NEOPICO_STATIC_OSD_PRERENDER
    resolution_selector_render_full();
#endif
#endif
#endif
#if NEOPICO_ENABLE_SELFTEST
    s_last_update_frame = video_frame_count;
    s_video_hi = 0;
    s_video_lo = 0;
    s_video_samples = 0;
    s_audio_hi = 0;
    s_audio_lo = 0;
    s_audio_samples = 0;
    s_shadow_hold_updates = 0;
#endif
    if (osd_visible) {
        menu_diag_experiment_on_menu_open();
    }
}

void menu_diag_experiment_on_menu_open(void)
{
#if NEOPICO_REBOOT_SELECTOR_UI && !NEOPICO_STATIC_OSD_PRERENDER
    s_selected_mode = video_pipeline_reboot_requested_mode();
    resolution_selector_render_full();
#endif
#if NEOPICO_ENABLE_SELFTEST
    selftest_layout_reset();
    s_last_update_frame = video_frame_count;
    s_video_hi = 0;
    s_video_lo = 0;
    s_video_samples = 0;
    s_audio_hi = 0;
    s_audio_lo = 0;
    s_audio_samples = 0;
    s_shadow_hold_updates = 0;
#endif
}

void menu_diag_experiment_on_menu_close(void)
{
    // Keep existing OSD buffer contents; visibility controls display.
}

void menu_diag_experiment_tick_background(void)
{
#if NEOPICO_STATIC_OSD_TOGGLE_ONLY || NEOPICO_STATIC_OSD_SELECT_ONLY || NEOPICO_STATIC_OSD_APPLY
    {
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        const bool btn_pressed = !gpio_get(PIN_OSD_BTN_MENU); // active low
        if (btn_pressed && !s_btn_was_pressed && (now_ms - s_last_press_ms) >= 200U) {
            s_last_press_ms = now_ms;
#if NEOPICO_STATIC_OSD_APPLY
            if (osd_visible) {
                resolution_selector_apply();
            } else {
                osd_show();
            }
#else
            osd_toggle();
#endif
        }
        s_btn_was_pressed = btn_pressed;
#if NEOPICO_STATIC_OSD_SELECT_ONLY || NEOPICO_STATIC_OSD_APPLY
        const bool back_pressed = !gpio_get(PIN_OSD_BTN_BACK); // active low
        if (osd_visible && back_pressed && !s_back_was_pressed && (now_ms - s_last_back_press_ms) >= 200U) {
            const video_pipeline_reboot_mode_t previous_mode = s_selected_mode;
            s_last_back_press_ms = now_ms;
            s_selected_mode = resolution_next(s_selected_mode);
            resolution_selector_update_selection(previous_mode);
        }
        s_back_was_pressed = back_pressed;
#endif
        return;
    }
#endif

#if NEOPICO_REBOOT_SELECTOR_UI
    const bool btn_pressed = !gpio_get(PIN_OSD_BTN_MENU); // active low
    if (!osd_visible) {
        if (btn_pressed && !s_btn_was_pressed) {
            s_btn_was_pressed = true;
            s_back_was_pressed = false;
            s_last_press_ms = to_ms_since_boot(get_absolute_time());
            menu_diag_experiment_on_menu_open();
            osd_show();
        } else {
            s_btn_was_pressed = btn_pressed;
            s_back_was_pressed = false;
        }
        return;
    }
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
#else
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool btn_pressed = !gpio_get(PIN_OSD_BTN_MENU); // active low
#endif

    // Simple edge + debounce handling on Core1 background tick.
    if (btn_pressed && !s_btn_was_pressed && (now_ms - s_last_press_ms) >= 200U) {
        s_last_press_ms = now_ms;
#if NEOPICO_REBOOT_SELECTOR_UI
        if (osd_visible) {
            resolution_selector_apply();
        } else {
            menu_diag_experiment_on_menu_open();
            osd_show();
        }
#else
        if (osd_visible) {
            osd_hide();
            menu_diag_experiment_on_menu_close();
        } else {
            menu_diag_experiment_on_menu_open();
            osd_show();
        }
#endif
    }
    s_btn_was_pressed = btn_pressed;

#if NEOPICO_EXP_REBOOT_MODE_SWITCH
    const bool back_pressed = !gpio_get(PIN_OSD_BTN_BACK); // active low
    if (back_pressed && !s_back_was_pressed && (now_ms - s_last_back_press_ms) >= 200U) {
        s_last_back_press_ms = now_ms;
#if NEOPICO_REBOOT_SELECTOR_UI
        if (osd_visible) {
            const video_pipeline_reboot_mode_t previous_mode = s_selected_mode;
            s_selected_mode = resolution_next(s_selected_mode);
            resolution_selector_update_selection(previous_mode);
        }
#else
#if NEOPICO_EXP_REBOOT_MODE_SWITCH_720P
        video_pipeline_reboot_mode_t next_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
        switch (video_pipeline_reboot_requested_mode()) {
            case VIDEO_PIPELINE_REBOOT_MODE_480P:
                next_mode = VIDEO_PIPELINE_REBOOT_MODE_240P;
                break;
            case VIDEO_PIPELINE_REBOOT_MODE_240P:
                next_mode = VIDEO_PIPELINE_REBOOT_MODE_720P;
                break;
            default:
                next_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
                break;
        }
        video_pipeline_request_reboot_mode(next_mode);
#else
        video_pipeline_request_reboot_240p(!video_pipeline_reboot_requested_240p());
#endif
#endif
    }
    s_back_was_pressed = back_pressed;
#endif

#if NEOPICO_ENABLE_SELFTEST
    if (osd_visible) {
        uint32_t video_sample = 0;
        if (gpio_get(PIN_MVS_CSYNC)) {
            video_sample |= SELFTEST_BIT_CSYNC;
        }
        if (gpio_get(PIN_MVS_PCLK)) {
            video_sample |= SELFTEST_BIT_PCLK;
        }
        if (gpio_get(PIN_MVS_SHADOW)) {
            video_sample |= SELFTEST_BIT_SHADOW;
        }
        if ((video_sample & SELFTEST_BIT_SHADOW) != 0U) {
            s_shadow_hold_updates = SELFTEST_SHADOW_HOLD_UPDATES;
        }
        if (gpio_get(PIN_MVS_DARK)) {
            video_sample |= SELFTEST_BIT_DARK;
        }
        if (gpio_get(PIN_MVS_R0)) {
            video_sample |= SELFTEST_BIT_R0;
        }
        if (gpio_get(PIN_MVS_R1)) {
            video_sample |= SELFTEST_BIT_R1;
        }
        if (gpio_get(PIN_MVS_R2)) {
            video_sample |= SELFTEST_BIT_R2;
        }
        if (gpio_get(PIN_MVS_R3)) {
            video_sample |= SELFTEST_BIT_R3;
        }
        if (gpio_get(PIN_MVS_R4)) {
            video_sample |= SELFTEST_BIT_R4;
        }
        if (gpio_get(PIN_MVS_G0)) {
            video_sample |= SELFTEST_BIT_G0;
        }
        if (gpio_get(PIN_MVS_G1)) {
            video_sample |= SELFTEST_BIT_G1;
        }
        if (gpio_get(PIN_MVS_G2)) {
            video_sample |= SELFTEST_BIT_G2;
        }
        if (gpio_get(PIN_MVS_G3)) {
            video_sample |= SELFTEST_BIT_G3;
        }
        if (gpio_get(PIN_MVS_G4)) {
            video_sample |= SELFTEST_BIT_G4;
        }
        if (gpio_get(PIN_MVS_B0)) {
            video_sample |= SELFTEST_BIT_B0;
        }
        if (gpio_get(PIN_MVS_B1)) {
            video_sample |= SELFTEST_BIT_B1;
        }
        if (gpio_get(PIN_MVS_B2)) {
            video_sample |= SELFTEST_BIT_B2;
        }
        if (gpio_get(PIN_MVS_B3)) {
            video_sample |= SELFTEST_BIT_B3;
        }
        if (gpio_get(PIN_MVS_B4)) {
            video_sample |= SELFTEST_BIT_B4;
        }
        s_video_hi |= video_sample;
        s_video_lo |= ~video_sample;
        s_video_samples++;

        uint32_t audio_sample = 0;
        if (gpio_get(PIN_I2S_BCK)) {
            audio_sample |= SELFTEST_BIT_BCK;
        }
        if (gpio_get(PIN_I2S_WS)) {
            audio_sample |= SELFTEST_BIT_WS;
        }
        if (gpio_get(PIN_I2S_DAT)) {
            audio_sample |= SELFTEST_BIT_DAT;
        }
        s_audio_hi |= audio_sample;
        s_audio_lo |= ~audio_sample;
        s_audio_samples++;
    }

    if (osd_visible && (video_frame_count - s_last_update_frame) >= 60U) {
        s_last_update_frame = video_frame_count;
        uint32_t toggled_bits = 0;
        bool has_snapshot = false;
        if (s_video_samples > 0U) {
            const uint32_t video_mask = SELFTEST_VIDEO_BITS_MASK;
            toggled_bits = s_video_hi & s_video_lo & video_mask;
            has_snapshot = true;
            s_video_hi = 0;
            s_video_lo = 0;
            s_video_samples = 0;
        }
        if (s_audio_samples > 0U) {
            toggled_bits |= s_audio_hi & s_audio_lo & (SELFTEST_BIT_BCK | SELFTEST_BIT_WS | SELFTEST_BIT_DAT);
            has_snapshot = true;
            s_audio_hi = 0;
            s_audio_lo = 0;
            s_audio_samples = 0;
        }
        if (s_shadow_hold_updates > 0U) {
            toggled_bits |= SELFTEST_BIT_SHADOW;
            s_shadow_hold_updates--;
        }
        // Full video + full audio diagnostics phase; no capture-path interaction.
        selftest_layout_update(video_frame_count, has_snapshot, toggled_bits);
    }
#endif
}

#else // !NEOPICO_ENABLE_OSD

void menu_diag_experiment_init(void)
{
}
void menu_diag_experiment_on_menu_open(void)
{
}
void menu_diag_experiment_on_menu_close(void)
{
}
void menu_diag_experiment_tick_background(void)
{
}

#endif // NEOPICO_ENABLE_OSD

#include "menu_diag_experiment.h"

#if NEOPICO_ENABLE_OSD

#include "pico/time.h"

#include "hardware/gpio.h"

#include "mvs_pins.h"
#include "osd/fast_osd.h"
#include "video_pipeline.h"
#if NEOPICO_ENABLE_SELFTEST
#include "osd/selftest_layout.h"

#if NEOPICO_EXP_PRECOMPOSED_HDMI
extern volatile uint32_t g_neopico_resync_count;
#define SELFTEST_RESYNC_ROW 15

static void selftest_draw_resync_count(void)
{
    char buf[12];
    uint32_t v = g_neopico_resync_count;
    int i = (int)sizeof(buf) - 1;
    buf[i--] = 0;
    do {
        buf[i--] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U && i >= 0);
    fast_osd_puts_color(SELFTEST_RESYNC_ROW, 1, "RS", OSD_COLOR_GRAY);
    fast_osd_puts_color(SELFTEST_RESYNC_ROW, 4, &buf[i + 1],
                        g_neopico_resync_count ? OSD_COLOR_YELLOW : OSD_COLOR_GREEN);
}
#endif

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

#ifndef NEOPICO_OSD_ROOT_MENU
#define NEOPICO_OSD_ROOT_MENU 0
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

#if NEOPICO_OSD_ROOT_MENU
// Root menu hosts both leaf screens, so the selector UI no longer excludes
// the selftest layout.
#define NEOPICO_REBOOT_SELECTOR_UI (NEOPICO_EXP_REBOOT_MODE_SWITCH && !NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI)
#else
#define NEOPICO_REBOOT_SELECTOR_UI                                                                                     \
    (NEOPICO_EXP_REBOOT_MODE_SWITCH && !NEOPICO_ENABLE_SELFTEST && !NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI)
#endif

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
#if NEOPICO_EXP_REBOOT_MODE_SWITCH || NEOPICO_OSD_ROOT_MENU
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

#if NEOPICO_OSD_ROOT_MENU
// ===========================================================================
// Root OSD menu: two entries (Resolution / Self Test), each present only if
// its feature is compiled in. BACK cycles, MENU enters; leaf screens return
// to the root on MENU; the root auto-hides after 8 s of inactivity. All
// logic and drawing run on the Core 1 background tick -- nothing here may
// ever touch the capture path or the scratch_x/scratch_y sections (see
// SCRATCHBOOK: code presence in those areas has caused sync drops).
// ===========================================================================

typedef enum {
    MENU_SCREEN_HIDDEN = 0,
    MENU_SCREEN_ROOT,
    MENU_SCREEN_RESOLUTION,
    MENU_SCREEN_SELFTEST,
} menu_screen_t;

#define ROOT_TITLE_ROW 1
#define ROOT_FIRST_ENTRY_ROW 5
#define ROOT_HINT_ROW 13
#define ROOT_IDLE_HIDE_MS 8000U

static menu_screen_t s_screen = MENU_SCREEN_HIDDEN;
static uint8_t s_root_sel = 0;
static uint32_t s_root_last_input_ms = 0;

static const char *const s_root_entry_labels[] = {
#if NEOPICO_REBOOT_SELECTOR_UI
    "Resolution",
#endif
#if NEOPICO_ENABLE_SELFTEST
    "Self Test",
#endif
};
#define ROOT_ENTRY_COUNT (sizeof(s_root_entry_labels) / sizeof(s_root_entry_labels[0]))

#if NEOPICO_REBOOT_SELECTOR_UI || NEOPICO_ENABLE_SELFTEST
#else
#error NEOPICO_OSD_ROOT_MENU needs at least one of NEOPICO_EXP_REBOOT_MODE_SWITCH / NEOPICO_ENABLE_SELFTEST
#endif

static menu_screen_t root_entry_screen(uint8_t idx)
{
    uint8_t i = 0;
    (void)i;
#if NEOPICO_REBOOT_SELECTOR_UI
    if (idx == i++) {
        return MENU_SCREEN_RESOLUTION;
    }
#endif
#if NEOPICO_ENABLE_SELFTEST
    if (idx == i++) {
        return MENU_SCREEN_SELFTEST;
    }
#endif
    return MENU_SCREEN_ROOT;
}

static void root_menu_render_entry(uint8_t idx)
{
    const bool selected = (s_root_sel == idx);
    const uint8_t row = (uint8_t)(ROOT_FIRST_ENTRY_ROW + 2U * idx);
    const uint16_t color = selected ? OSD_COLOR_YELLOW : OSD_COLOR_FG;
    fast_osd_putc_color(row, 3, selected ? '>' : ' ', color);
    fast_osd_puts_color(row, 5, s_root_entry_labels[idx], color);
}

static void root_menu_draw(void)
{
    fast_osd_clear();
    fast_osd_puts_color(ROOT_TITLE_ROW, 2, "NeoPico-HD", OSD_COLOR_YELLOW);
    for (uint8_t i = 0; i < (uint8_t)ROOT_ENTRY_COUNT; i++) {
        root_menu_render_entry(i);
    }
    fast_osd_puts_color(ROOT_HINT_ROW, 2, "BACK move  MENU enter", OSD_COLOR_GRAY);
}

static void root_menu_enter_root(uint32_t now_ms)
{
    root_menu_draw();
    s_screen = MENU_SCREEN_ROOT;
    s_root_last_input_ms = now_ms;
}

static void root_menu_enter_leaf(void)
{
    const menu_screen_t leaf = root_entry_screen(s_root_sel);
    switch (leaf) {
#if NEOPICO_REBOOT_SELECTOR_UI
        case MENU_SCREEN_RESOLUTION:
            s_selected_mode = video_pipeline_reboot_requested_mode();
            resolution_selector_render_full();
            s_screen = MENU_SCREEN_RESOLUTION;
            break;
#endif
#if NEOPICO_ENABLE_SELFTEST
        case MENU_SCREEN_SELFTEST:
            selftest_layout_reset();
            s_last_update_frame = video_frame_count;
            s_video_hi = 0;
            s_video_lo = 0;
            s_video_samples = 0;
            s_audio_hi = 0;
            s_audio_lo = 0;
            s_audio_samples = 0;
            s_shadow_hold_updates = 0;
            s_screen = MENU_SCREEN_SELFTEST;
            break;
#endif
        default:
            break;
    }
}

static void root_menu_buttons_tick(void)
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool menu_pressed = !gpio_get(PIN_OSD_BTN_MENU); // active low
    const bool back_pressed = !gpio_get(PIN_OSD_BTN_BACK); // active low
    const bool menu_edge = menu_pressed && !s_btn_was_pressed && (now_ms - s_last_press_ms) >= 200U;
    const bool back_edge = back_pressed && !s_back_was_pressed && (now_ms - s_last_back_press_ms) >= 200U;
    if (menu_edge) {
        s_last_press_ms = now_ms;
    }
    if (back_edge) {
        s_last_back_press_ms = now_ms;
    }
    s_btn_was_pressed = menu_pressed;
    s_back_was_pressed = back_pressed;

    switch (s_screen) {
        case MENU_SCREEN_HIDDEN:
            if (menu_edge) {
                root_menu_enter_root(now_ms);
                osd_show();
            }
            break;

        case MENU_SCREEN_ROOT:
            if (back_edge) {
                const uint8_t prev = s_root_sel;
                s_root_sel = (uint8_t)((s_root_sel + 1U) % ROOT_ENTRY_COUNT);
                root_menu_render_entry(prev);
                root_menu_render_entry(s_root_sel);
                s_root_last_input_ms = now_ms;
            } else if (menu_edge) {
                root_menu_enter_leaf();
            } else if ((now_ms - s_root_last_input_ms) >= ROOT_IDLE_HIDE_MS) {
                osd_hide();
                s_screen = MENU_SCREEN_HIDDEN;
            }
            break;

#if NEOPICO_REBOOT_SELECTOR_UI
        case MENU_SCREEN_RESOLUTION:
            if (back_edge) {
                const video_pipeline_reboot_mode_t previous_mode = s_selected_mode;
                s_selected_mode = resolution_next(s_selected_mode);
                resolution_selector_update_selection(previous_mode);
            } else if (menu_edge) {
                if (s_selected_mode == video_pipeline_reboot_requested_mode()) {
                    root_menu_enter_root(now_ms);
                } else {
                    osd_hide();
                    s_screen = MENU_SCREEN_HIDDEN;
                    video_pipeline_request_reboot_mode(s_selected_mode);
                }
            }
            break;
#endif

#if NEOPICO_ENABLE_SELFTEST
        case MENU_SCREEN_SELFTEST:
            if (menu_edge) {
                root_menu_enter_root(now_ms);
            }
            break;
#endif

        default:
            break;
    }
}
#endif // NEOPICO_OSD_ROOT_MENU

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

void SELECTOR_UI_RAM(menu_diag_experiment_tick_background)(void)
{
#if NEOPICO_OSD_ROOT_MENU
    root_menu_buttons_tick();
#endif
#if !NEOPICO_OSD_ROOT_MENU &&                                                                                          \
    (NEOPICO_STATIC_OSD_TOGGLE_ONLY || NEOPICO_STATIC_OSD_SELECT_ONLY || NEOPICO_STATIC_OSD_APPLY)
    {
        const bool btn_pressed = !gpio_get(PIN_OSD_BTN_MENU); // active low
        if (!osd_visible) {
            if (btn_pressed && !s_btn_was_pressed) {
                s_btn_was_pressed = true;
                s_back_was_pressed = false;
                s_last_press_ms = to_ms_since_boot(get_absolute_time());
                osd_show();
            } else {
                s_btn_was_pressed = btn_pressed;
                s_back_was_pressed = false;
            }
            return;
        }

        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (btn_pressed && !s_btn_was_pressed && (now_ms - s_last_press_ms) >= 200U) {
            s_last_press_ms = now_ms;
#if NEOPICO_STATIC_OSD_APPLY
            resolution_selector_apply();
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

#if !NEOPICO_OSD_ROOT_MENU
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
#endif // !NEOPICO_OSD_ROOT_MENU

#if NEOPICO_ENABLE_SELFTEST
    if (osd_visible
#if NEOPICO_OSD_ROOT_MENU
        && s_screen == MENU_SCREEN_SELFTEST
#endif
    ) {
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
#if NEOPICO_EXP_PRECOMPOSED_HDMI
        selftest_draw_resync_count();
#endif
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

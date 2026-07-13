#include "menu_diag_experiment.h"

#include <stdio.h>
#include <string.h>

#include "audio_source.h"

#if (NEOPICO_SETTINGS_FLASH && NEOPICO_RESOLUTION_MENU) || NEOPICO_OSD_RES_CONFIRM ||                                  \
    NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
#include "settings.h"
#endif

#if NEOPICO_ENABLE_OSD

#include "pico/time.h"

#include "hardware/gpio.h"

#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
#include "audio_subsystem.h"
#endif
#include "capture_pins.h"
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

#ifndef NEOPICO_RESOLUTION_MENU
#define NEOPICO_RESOLUTION_MENU 0
#endif

#ifndef NEOPICO_RESOLUTION_MENU_720P
#define NEOPICO_RESOLUTION_MENU_720P 0
#endif

#ifndef NEOPICO_OSD_ROOT_MENU
#define NEOPICO_OSD_ROOT_MENU 0
#endif

#ifndef NEOPICO_VERSION
#define NEOPICO_VERSION "dev"
#endif

#define SELECTOR_UI_RAM(name) name
#define SELECTOR_UI_APPLY_RAM(name) SELECTOR_UI_RAM(name)

#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
#define AUDIO_SELECTOR_TITLE_ROW 1
#define AUDIO_SELECTOR_FIRST_OPTION_ROW 7
#define AUDIO_SELECTOR_HINT_ROW 13

static audio_source_t s_selected_audio_source;

static const char *audio_source_label(audio_source_t source)
{
    return (source == AUDIO_SOURCE_PCM1802_I2S) ? "PCM1802 I2S" : "MV1C Digital";
}

static const char *audio_source_description(audio_source_t source)
{
    return (source == AUDIO_SOURCE_PCM1802_I2S) ? "External ADC" : "NEO-YSA2 bus";
}

static audio_source_t audio_source_next(audio_source_t source)
{
    return (source == AUDIO_SOURCE_PCM1802_I2S) ? AUDIO_SOURCE_MV1C_DIGITAL : AUDIO_SOURCE_PCM1802_I2S;
}

static uint8_t audio_selector_option_row(audio_source_t source)
{
    return (source == AUDIO_SOURCE_PCM1802_I2S) ? AUDIO_SELECTOR_FIRST_OPTION_ROW + 2 : AUDIO_SELECTOR_FIRST_OPTION_ROW;
}

static void audio_selector_render_option(audio_source_t source)
{
    const bool selected = (s_selected_audio_source == source);
    const bool current = (audio_subsystem_get_source() == source);
    const uint16_t color = selected ? OSD_COLOR_YELLOW : current ? OSD_COLOR_GREEN : OSD_COLOR_FG;
    const uint8_t row = audio_selector_option_row(source);
    const char *label = audio_source_label(source);
    fast_osd_putc_color(row, 3, selected ? '>' : ' ', color);
    fast_osd_puts_color(row, 5, label, color);
    if (current) {
        fast_osd_putc_color(row, (uint8_t)(5 + strlen(label)), '*', color);
    }
}

static void audio_selector_render_description(void)
{
    fast_osd_puts_color(AUDIO_SELECTOR_HINT_ROW, 2, "                    ", OSD_COLOR_GRAY);
    fast_osd_puts_color(AUDIO_SELECTOR_HINT_ROW, 2, audio_source_description(s_selected_audio_source), OSD_COLOR_GRAY);
}

static void audio_selector_update_selection(audio_source_t previous_source)
{
    if (previous_source == s_selected_audio_source) {
        return;
    }
    audio_selector_render_option(previous_source);
    audio_selector_render_option(s_selected_audio_source);
    audio_selector_render_description();
}

static void audio_selector_render_full(void)
{
    fast_osd_clear();
    fast_osd_puts_color(AUDIO_SELECTOR_TITLE_ROW, 2, "NeoPico-HD Audio", OSD_COLOR_YELLOW);
    fast_osd_puts_color(AUDIO_SELECTOR_FIRST_OPTION_ROW - 2, 2, "Audio", OSD_COLOR_FG);
    audio_selector_render_option(AUDIO_SOURCE_MV1C_DIGITAL);
    audio_selector_render_option(AUDIO_SOURCE_PCM1802_I2S);
    audio_selector_render_description();
}
#endif

#if NEOPICO_OSD_ROOT_MENU
// Root menu hosts both leaf screens, so the selector UI no longer excludes
// the selftest layout.
#define NEOPICO_REBOOT_SELECTOR_UI NEOPICO_RESOLUTION_MENU
#else
#define NEOPICO_REBOOT_SELECTOR_UI (NEOPICO_RESOLUTION_MENU && !NEOPICO_ENABLE_SELFTEST)
#endif

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
#if NEOPICO_RESOLUTION_MENU || NEOPICO_OSD_ROOT_MENU
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

static inline bool osd_physical_menu_pressed(void)
{
    return !gpio_get(PIN_OSD_BTN_MENU);
}

static inline bool osd_physical_back_pressed(void)
{
    return !gpio_get(PIN_OSD_BTN_BACK);
}

#if NEOPICO_SETTINGS_FLASH && NEOPICO_RESOLUTION_MENU
#define FACTORY_RESET_HOLD_MS 5000U
static bool s_factory_reset_chord_active;
static uint32_t s_factory_reset_hold_start_ms;

static void factory_reset_buttons_tick(void)
{
    const bool chord_pressed = osd_physical_menu_pressed() && osd_physical_back_pressed();
    if (!chord_pressed) {
        s_factory_reset_chord_active = false;
        return;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (!s_factory_reset_chord_active) {
        s_factory_reset_chord_active = true;
        s_factory_reset_hold_start_ms = now_ms;
        return;
    }

    if ((now_ms - s_factory_reset_hold_start_ms) >= FACTORY_RESET_HOLD_MS) {
        // Persist the recovery defaults before rebooting. This path is
        // intentionally independent of OSD visibility and the current screen.
        settings_factory_reset();
        video_pipeline_request_reboot_mode(VIDEO_PIPELINE_REBOOT_MODE_480P);
    }
}
#endif

#if NEOPICO_OSD_CONTROLLER_INPUTS
typedef struct {
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    bool press_event;
    uint32_t raw_changed_ms;
} osd_controller_button_t;

static osd_controller_button_t s_controller_start;
static osd_controller_button_t s_controller_select;
static osd_controller_button_t s_controller_up;
static osd_controller_button_t s_controller_down;

static void osd_controller_button_init(osd_controller_button_t *button, bool pressed, uint32_t now_ms)
{
    button->raw_pressed = pressed;
    button->stable_pressed = pressed;
    button->armed = !pressed;
    button->press_event = false;
    button->raw_changed_ms = now_ms;
}

static void osd_controller_button_update(osd_controller_button_t *button, bool pressed, uint32_t now_ms)
{
    button->press_event = false;
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->raw_changed_ms = now_ms;
    }

    if (pressed) {
        // Capture the first active-low edge immediately so short taps are not
        // lost between Core 1 background polls. Bounce cannot repeat because
        // the button stays disarmed until a stable release.
        if (button->armed) {
            button->stable_pressed = true;
            button->armed = false;
            button->press_event = true;
        }
    } else if (button->stable_pressed && (now_ms - button->raw_changed_ms) >= NEOPICO_OSD_CONTROLLER_DEBOUNCE_MS) {
        button->stable_pressed = false;
        button->armed = true;
    }
}

static void osd_controller_buttons_update(uint32_t now_ms)
{
    osd_controller_button_update(&s_controller_start, !gpio_get(NEOPICO_OSD_CONTROLLER_MENU_PIN), now_ms);
    osd_controller_button_update(&s_controller_select, !gpio_get(NEOPICO_OSD_CONTROLLER_BACK_PIN), now_ms);
    osd_controller_button_update(&s_controller_up, !gpio_get(NEOPICO_OSD_CONTROLLER_UP_PIN), now_ms);
    osd_controller_button_update(&s_controller_down, !gpio_get(NEOPICO_OSD_CONTROLLER_DOWN_PIN), now_ms);
}
#endif

static inline bool osd_menu_pressed(void)
{
    bool pressed = osd_physical_menu_pressed();
#if NEOPICO_OSD_CONTROLLER_INPUTS
    pressed |= s_controller_start.stable_pressed;
#endif
    return pressed;
}

static inline bool osd_back_pressed(void)
{
    bool pressed = osd_physical_back_pressed();
#if NEOPICO_OSD_CONTROLLER_INPUTS
    pressed |= s_controller_select.stable_pressed;
#endif
    return pressed;
}

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

// Shown for the hovered entry in the description row (where MENU/BACK hints were).
static const char *SELECTOR_UI_RAM(resolution_description)(video_pipeline_reboot_mode_t mode)
{
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return "Direct Mode";
        case VIDEO_PIPELINE_REBOOT_MODE_720P:
            return "Experimental (3x)"; // 720p has a rare Game-Mode glitch; see docs/720P_PURPLE_GLITCH.md
        default:
            return "2x Integer Scaling";
    }
}

static void SELECTOR_UI_RAM(resolution_selector_render_description)(void)
{
    // Clear the row (longest description is 18 chars) then draw the hovered one.
    fast_osd_puts_color(RES_SELECTOR_HINT_ROW, 2, "                    ", OSD_COLOR_GRAY);
    fast_osd_puts_color(RES_SELECTOR_HINT_ROW, 2, resolution_description(s_selected_mode), OSD_COLOR_GRAY);
}

static video_pipeline_reboot_mode_t SELECTOR_UI_RAM(resolution_next)(video_pipeline_reboot_mode_t mode)
{
    // Cycle in display order: 240p -> 480p -> 720p -> 240p.
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
#if NEOPICO_RESOLUTION_MENU_720P
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            return VIDEO_PIPELINE_REBOOT_MODE_720P;
#endif
        default:
            return VIDEO_PIPELINE_REBOOT_MODE_240P;
    }
}

static video_pipeline_reboot_mode_t SELECTOR_UI_RAM(resolution_previous)(video_pipeline_reboot_mode_t mode)
{
    // Cycle opposite the display order: 240p -> 720p/480p -> 480p -> 240p.
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            return VIDEO_PIPELINE_REBOOT_MODE_240P;
#if NEOPICO_RESOLUTION_MENU_720P
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return VIDEO_PIPELINE_REBOOT_MODE_720P;
        case VIDEO_PIPELINE_REBOOT_MODE_720P:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
#endif
        default:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
}

static bool SELECTOR_UI_RAM(resolution_selector_option_row)(video_pipeline_reboot_mode_t mode, uint8_t *row)
{
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            *row = RES_SELECTOR_FIRST_OPTION_ROW;
            return true;
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            *row = RES_SELECTOR_FIRST_OPTION_ROW + 2;
            return true;
#if NEOPICO_RESOLUTION_MENU_720P
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
    const char *label = resolution_label(mode);
    fast_osd_putc_color(row, 3, selected ? '>' : ' ', color);
    fast_osd_puts_color(row, 5, label, color);
    if (current) {
        fast_osd_putc_color(row, (uint8_t)(5 + strlen(label)), '*', color);
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
    resolution_selector_render_description();
}

static void SELECTOR_UI_RAM(resolution_selector_render_full)(void)
{
    fast_osd_clear();
    fast_osd_puts_color(RES_SELECTOR_TITLE_ROW, 2, "NeoPico-HD Output", OSD_COLOR_YELLOW);
    fast_osd_puts_color(RES_SELECTOR_FIRST_OPTION_ROW - 2, 2, "Resolution", OSD_COLOR_FG);

    resolution_selector_render_option(RES_SELECTOR_FIRST_OPTION_ROW, VIDEO_PIPELINE_REBOOT_MODE_240P);
    resolution_selector_render_option(RES_SELECTOR_FIRST_OPTION_ROW + 2, VIDEO_PIPELINE_REBOOT_MODE_480P);
#if NEOPICO_RESOLUTION_MENU_720P
    resolution_selector_render_option(RES_SELECTOR_FIRST_OPTION_ROW + 4, VIDEO_PIPELINE_REBOOT_MODE_720P);
#endif
    resolution_selector_render_description();
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
// Root OSD menu: each entry is present only if its feature is compiled in.
// Controller UP/DOWN moves, START confirms, and SELECT returns. The two
// physical buttons retain their legacy MENU-confirm/BACK-cycle behavior. The
// root auto-hides after 8 s of inactivity. All
// logic and drawing run on the Core 1 background tick -- nothing here may
// ever touch the capture path or the scratch_x/scratch_y sections (see
// SCRATCHBOOK: code presence in those areas has caused sync drops).
// ===========================================================================

typedef enum {
    MENU_SCREEN_HIDDEN = 0,
    MENU_SCREEN_ROOT,
    MENU_SCREEN_RESOLUTION,
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    MENU_SCREEN_AUDIO,
#endif
    MENU_SCREEN_SELFTEST,
#if NEOPICO_OSD_RES_CONFIRM
    MENU_SCREEN_RES_CONFIRM,
#endif
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    MENU_SCREEN_GENLOCK,
#endif
} menu_screen_t;

#define ROOT_TITLE_ROW 1
#define ROOT_FIRST_ENTRY_ROW 5
#define ROOT_HINT_ROW 13
#define ROOT_IDLE_HIDE_MS 8000U

static menu_screen_t s_screen = MENU_SCREEN_HIDDEN;
static uint8_t s_root_sel = 0;
static uint32_t s_root_last_input_ms = 0;

#if NEOPICO_OSD_RES_CONFIRM
// Resolution-change safety net. Armed at boot (by main) when this boot is a
// PENDING confirmation: show a countdown; MENU keeps (persists to flash), BACK
// or timeout reverts (reboots to the previous mode). Re-add the MENU/BACK hint
// here only (the global hints stay removed).
#define RES_CONFIRM_TIMEOUT_MS 10000U
#define RES_CONFIRM_HINT_ROW 13
static bool s_res_confirm_armed = false;
static video_pipeline_reboot_mode_t s_res_confirm_new;
static video_pipeline_reboot_mode_t s_res_confirm_prev;
static uint32_t s_res_confirm_deadline_ms;
static int32_t s_res_confirm_last_secs = -1;
#endif

static const char *const s_root_entry_labels[] = {
#if NEOPICO_REBOOT_SELECTOR_UI
    "Resolution",
#endif
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    "Audio",
#endif
#if NEOPICO_ENABLE_SELFTEST
    "Self Test",
#endif
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    "Genlock",
#endif
};
#define ROOT_ENTRY_COUNT (sizeof(s_root_entry_labels) / sizeof(s_root_entry_labels[0]))

#if NEOPICO_REBOOT_SELECTOR_UI || NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE || NEOPICO_ENABLE_SELFTEST
#else
#error NEOPICO_OSD_ROOT_MENU needs at least one enabled root-menu feature
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
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    if (idx == i++) {
        return MENU_SCREEN_AUDIO;
    }
#endif
#if NEOPICO_ENABLE_SELFTEST
    if (idx == i++) {
        return MENU_SCREEN_SELFTEST;
    }
#endif
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    if (idx == i++) {
        return MENU_SCREEN_GENLOCK;
    }
#endif
    return MENU_SCREEN_ROOT;
}

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Dedicated genlock telemetry screen (full draw on entry, value-only 1 Hz
// updates, per the OSD render rules).
static uint32_t s_genlock_update_frame;

static void genlock_screen_update_values(void)
{
    extern volatile uint32_t g_genlock_phase_us;
    extern uint16_t rt_v_total_lines;
    int video_output_get_vblank_htrim_slots(void);
    int video_output_get_vblank_htrim_px(void);
    char buf[14];
    snprintf(buf, sizeof buf, "%5lu us", (unsigned long)g_genlock_phase_us);
    fast_osd_puts_color(4, 11, buf, OSD_COLOR_YELLOW);
    snprintf(buf, sizeof buf, "%+4d px", video_output_get_vblank_htrim_px());
    fast_osd_puts_color(6, 11, buf, OSD_COLOR_YELLOW);
    snprintf(buf, sizeof buf, "%2d", video_output_get_vblank_htrim_slots());
    fast_osd_puts_color(8, 11, buf, OSD_COLOR_YELLOW);
    snprintf(buf, sizeof buf, "%3u", (unsigned)rt_v_total_lines);
    fast_osd_puts_color(10, 11, buf, OSD_COLOR_YELLOW);
    snprintf(buf, sizeof buf, "%6lu s", (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000U));
    fast_osd_puts_color(12, 11, buf, OSD_COLOR_YELLOW);
    {
        void video_output_perf_probe_read(uint32_t *fifo_min, uint32_t *irq_gap_max_us);
        uint32_t fifo_min, gap_max;
        video_output_perf_probe_read(&fifo_min, &gap_max);
        extern volatile uint32_t hstx_di_queue_silence_count;
        char probe[24];
        snprintf(probe, sizeof probe, "F%2lu G%3lu U%6lu", (unsigned long)(fifo_min > 99 ? 99 : fifo_min),
                 (unsigned long)(gap_max > 999 ? 999 : gap_max), (unsigned long)hstx_di_queue_silence_count);
        fast_osd_puts_color(13, 2, probe, OSD_COLOR_YELLOW);
    }
}

static void genlock_screen_draw(void)
{
    fast_osd_clear();
    fast_osd_puts_color(1, 2, "Genlock", OSD_COLOR_YELLOW);
#if NEOPICO_TRIPLE_ASM
    {
        extern volatile bool g_scale_asm_selftest_ok;
        fast_osd_puts_color(1, 12, g_scale_asm_selftest_ok ? "ASM:OK" : "ASM:BAD",
                            g_scale_asm_selftest_ok ? OSD_COLOR_GREEN : OSD_COLOR_RED);
    }
#endif
    fast_osd_puts_color(4, 2, "PHASE", OSD_COLOR_GRAY);
    fast_osd_puts_color(6, 2, "TRIM", OSD_COLOR_GRAY);
    fast_osd_puts_color(8, 2, "SLOTS", OSD_COLOR_GRAY);
    fast_osd_puts_color(10, 2, "VTOTAL", OSD_COLOR_GRAY);
    fast_osd_puts_color(12, 2, "UPTIME", OSD_COLOR_GRAY);
    genlock_screen_update_values();
}
#endif

static void root_menu_render_entry(uint8_t idx)
{
    const bool selected = (s_root_sel == idx);
    const uint8_t row = (uint8_t)(ROOT_FIRST_ENTRY_ROW + (2U * idx));
    const uint16_t color = selected ? OSD_COLOR_YELLOW : OSD_COLOR_FG;
    fast_osd_putc_color(row, 3, selected ? '>' : ' ', color);
    fast_osd_puts_color(row, 5, s_root_entry_labels[idx], color);
}

static void root_menu_draw(void)
{
    fast_osd_clear();
    fast_osd_puts_color(ROOT_TITLE_ROW, 2, "NeoPico-HD v" NEOPICO_VERSION, OSD_COLOR_YELLOW);
    for (uint8_t i = 0; i < (uint8_t)ROOT_ENTRY_COUNT; i++) {
        root_menu_render_entry(i);
    }
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
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
        case MENU_SCREEN_AUDIO:
            s_selected_audio_source = audio_subsystem_get_source();
            audio_selector_render_full();
            s_screen = MENU_SCREEN_AUDIO;
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
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        case MENU_SCREEN_GENLOCK:
            genlock_screen_draw();
            s_genlock_update_frame = video_frame_count;
            s_screen = MENU_SCREEN_GENLOCK;
            break;
#endif
        default:
            break;
    }
}

#if NEOPICO_OSD_RES_CONFIRM
static void res_confirm_render_static(void)
{
    fast_osd_clear();
    fast_osd_puts_color(1, 2, "Keep this resolution?", OSD_COLOR_YELLOW);
    fast_osd_puts_color(4, 4, resolution_label(s_res_confirm_new), OSD_COLOR_GREEN);
    fast_osd_puts_color(7, 2, "Reverting in   s", OSD_COLOR_FG);
    fast_osd_puts_color(RES_CONFIRM_HINT_ROW, 2, "MENU keep   BACK revert", OSD_COLOR_GRAY);
}

// Draw the countdown digits at the "Reverting in __s" gap (no snprintf in the
// Core 1 background path).
static void res_confirm_render_secs(int32_t secs)
{
    const uint8_t col = 2 + 13; // after "Reverting in "
    fast_osd_putc_color(7, col, (secs >= 10) ? (char)('0' + (secs / 10)) : ' ', OSD_COLOR_FG);
    fast_osd_putc_color(7, (uint8_t)(col + 1), (char)('0' + (secs % 10)), OSD_COLOR_FG);
}

static void res_confirm_enter(uint32_t now_ms)
{
    s_screen = MENU_SCREEN_RES_CONFIRM;
    s_res_confirm_deadline_ms = now_ms + RES_CONFIRM_TIMEOUT_MS;
    s_res_confirm_last_secs = -1;
    res_confirm_render_static();
    osd_show();
}

static void res_confirm_keep(void)
{
    // The new mode was already persisted optimistically at select-time (at the
    // reboot point, where the flash stall is masked). Keeping just dismisses the
    // prompt -- NO live flash write, so the HDMI link is never stalled.
    s_res_confirm_armed = false;
    osd_hide();
    s_screen = MENU_SCREEN_HIDDEN;
}

static void res_confirm_revert(void)
{
#if NEOPICO_SETTINGS_FLASH
    // Roll back the optimistic save to the previous (confirmed) mode, then
    // reboot into it. The flash write happens at the reboot point (masked).
    neopico_settings_t persisted;
    settings_load(&persisted);
    persisted.resolution = (uint8_t)s_res_confirm_prev;
    settings_save(&persisted);
#endif
    video_pipeline_request_reboot_mode(s_res_confirm_prev);
}

// Called by main at boot when this boot is a PENDING resolution confirmation.
void menu_diag_experiment_arm_res_confirm(video_pipeline_reboot_mode_t new_mode,
                                          video_pipeline_reboot_mode_t previous_mode)
{
    s_res_confirm_armed = true;
    s_res_confirm_new = new_mode;
    s_res_confirm_prev = previous_mode;
}
#endif // NEOPICO_OSD_RES_CONFIRM

static void root_menu_buttons_tick(void)
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool menu_pressed = osd_physical_menu_pressed();
    const bool back_pressed = osd_physical_back_pressed();
    bool menu_edge = menu_pressed && !s_btn_was_pressed && (now_ms - s_last_press_ms) >= 200U;
    bool back_edge = back_pressed && !s_back_was_pressed && (now_ms - s_last_back_press_ms) >= 200U;
    bool controller_select_edge = false;
    bool up_edge = false;
    bool down_edge = false;
#if NEOPICO_OSD_CONTROLLER_INPUTS
    if (s_screen == MENU_SCREEN_HIDDEN) {
        // Controller START alone must not open the OSD. Open once when the
        // second half of a debounced START+SELECT chord becomes active.
        menu_edge |= s_controller_start.stable_pressed && s_controller_select.stable_pressed &&
                     (s_controller_start.press_event || s_controller_select.press_event);
    } else {
        // Once visible, controller navigation is distinct from the legacy
        // two-button physical scheme. Each press_event is one-shot.
        menu_edge |= s_controller_start.press_event;
        controller_select_edge = s_controller_select.press_event;
        up_edge = s_controller_up.press_event;
        down_edge = s_controller_down.press_event;
    }
#endif
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
            if (controller_select_edge) {
                osd_hide();
                s_screen = MENU_SCREEN_HIDDEN;
            } else if (up_edge != down_edge) {
                const uint8_t prev = s_root_sel;
                if (up_edge) {
                    s_root_sel = (uint8_t)((s_root_sel + ROOT_ENTRY_COUNT - 1U) % ROOT_ENTRY_COUNT);
                } else {
                    s_root_sel = (uint8_t)((s_root_sel + 1U) % ROOT_ENTRY_COUNT);
                }
                root_menu_render_entry(prev);
                root_menu_render_entry(s_root_sel);
                s_root_last_input_ms = now_ms;
            } else if (back_edge) {
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
            if (controller_select_edge) {
                root_menu_enter_root(now_ms);
            } else if (up_edge != down_edge) {
                const video_pipeline_reboot_mode_t previous_mode = s_selected_mode;
                s_selected_mode = up_edge ? resolution_previous(s_selected_mode) : resolution_next(s_selected_mode);
                resolution_selector_update_selection(previous_mode);
            } else if (back_edge) {
                const video_pipeline_reboot_mode_t previous_mode = s_selected_mode;
                s_selected_mode = resolution_next(s_selected_mode);
                resolution_selector_update_selection(previous_mode);
            } else if (menu_edge) {
                if (s_selected_mode == video_pipeline_reboot_requested_mode()) {
                    root_menu_enter_root(now_ms);
                } else {
                    osd_hide();
                    s_screen = MENU_SCREEN_HIDDEN;
#if NEOPICO_OSD_RES_CONFIRM
#if NEOPICO_SETTINGS_FLASH
                    // Optimistically persist the new mode now (at the reboot
                    // point, where the flash stall is masked). Confirm then just
                    // dismisses; revert/timeout rolls flash back to the previous.
                    {
                        neopico_settings_t persisted;
                        settings_load(&persisted);
                        persisted.resolution = (uint8_t)s_selected_mode;
                        settings_save(&persisted);
                    }
#endif
                    // Reboot into the new mode PENDING confirmation, carrying the
                    // previous (revert-to) mode across the reboot.
                    video_pipeline_request_reboot_mode_pending(s_selected_mode, video_pipeline_reboot_requested_mode());
#else
#if NEOPICO_SETTINGS_FLASH
                    {
                        neopico_settings_t persisted;
                        settings_load(&persisted);
                        persisted.resolution = (uint8_t)s_selected_mode;
                        settings_save(&persisted);
                    }
#endif
                    video_pipeline_request_reboot_mode(s_selected_mode);
#endif
                }
            }
            break;
#endif

#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
        case MENU_SCREEN_AUDIO:
            if (controller_select_edge) {
                root_menu_enter_root(now_ms);
            } else if ((up_edge != down_edge) || back_edge) {
                const audio_source_t previous_source = s_selected_audio_source;
                s_selected_audio_source = audio_source_next(s_selected_audio_source);
                audio_selector_update_selection(previous_source);
            } else if (menu_edge) {
                neopico_settings_t persisted;
                settings_load(&persisted);
                const bool already_persisted = persisted.audio_source_valid == NEOPICO_SETTINGS_AUDIO_SOURCE_VALID &&
                                               persisted.audio_source == (uint8_t)s_selected_audio_source;
                if (s_selected_audio_source == audio_subsystem_get_source() && already_persisted) {
                    root_menu_enter_root(now_ms);
                } else {
                    persisted.resolution = (uint8_t)video_pipeline_reboot_requested_mode();
                    persisted.audio_source = (uint8_t)s_selected_audio_source;
                    persisted.audio_source_valid = NEOPICO_SETTINGS_AUDIO_SOURCE_VALID;
                    osd_hide();
                    s_screen = MENU_SCREEN_HIDDEN;
                    settings_save(&persisted);
                    video_pipeline_request_reboot_mode(video_pipeline_reboot_requested_mode());
                }
            }
            break;
#endif

#if NEOPICO_OSD_RES_CONFIRM
        case MENU_SCREEN_RES_CONFIRM: {
            if (menu_edge) {
                res_confirm_keep();
            } else if (controller_select_edge || back_edge || (int32_t)(now_ms - s_res_confirm_deadline_ms) >= 0) {
                res_confirm_revert(); // reboots; does not return
            } else {
                int32_t secs = (int32_t)((s_res_confirm_deadline_ms - now_ms + 999U) / 1000U);
                if (secs > 99) {
                    secs = 99;
                }
                if (secs != s_res_confirm_last_secs) {
                    s_res_confirm_last_secs = secs;
                    res_confirm_render_secs(secs);
                }
            }
            break;
        }
#endif

#if NEOPICO_ENABLE_SELFTEST
        case MENU_SCREEN_SELFTEST:
            if (controller_select_edge || menu_edge) {
                root_menu_enter_root(now_ms);
            }
            break;
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC
        case MENU_SCREEN_GENLOCK:
            if (controller_select_edge || menu_edge) {
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
#if NEOPICO_SETTINGS_FLASH && NEOPICO_RESOLUTION_MENU
    s_factory_reset_chord_active = false;
    s_factory_reset_hold_start_ms = 0;
#endif
#if NEOPICO_OSD_CONTROLLER_INPUTS
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    osd_controller_button_init(&s_controller_start, !gpio_get(NEOPICO_OSD_CONTROLLER_MENU_PIN), now_ms);
    osd_controller_button_init(&s_controller_select, !gpio_get(NEOPICO_OSD_CONTROLLER_BACK_PIN), now_ms);
    osd_controller_button_init(&s_controller_up, !gpio_get(NEOPICO_OSD_CONTROLLER_UP_PIN), now_ms);
    osd_controller_button_init(&s_controller_down, !gpio_get(NEOPICO_OSD_CONTROLLER_DOWN_PIN), now_ms);
#endif
#if NEOPICO_RESOLUTION_MENU
    s_back_was_pressed = false;
    s_last_back_press_ms = 0;
#if NEOPICO_REBOOT_SELECTOR_UI
    s_selected_mode = video_pipeline_reboot_requested_mode();
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
#if NEOPICO_OSD_RES_CONFIRM
    // This boot is awaiting resolution confirmation: open the countdown prompt.
    if (s_res_confirm_armed) {
        res_confirm_enter(to_ms_since_boot(get_absolute_time()));
        return;
    }
#endif
}

void menu_diag_experiment_on_menu_open(void)
{
#if NEOPICO_REBOOT_SELECTOR_UI
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
#if NEOPICO_SETTINGS_FLASH && NEOPICO_RESOLUTION_MENU
    factory_reset_buttons_tick();
#endif
#if NEOPICO_OSD_CONTROLLER_INPUTS
    osd_controller_buttons_update(to_ms_since_boot(get_absolute_time()));
#endif
#if NEOPICO_OSD_ROOT_MENU
    root_menu_buttons_tick();
#endif
#if !NEOPICO_OSD_ROOT_MENU
#if NEOPICO_REBOOT_SELECTOR_UI
    const bool btn_pressed = osd_menu_pressed();
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
    const bool btn_pressed = osd_menu_pressed();
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

#if NEOPICO_RESOLUTION_MENU
    const bool back_pressed = osd_back_pressed();
    if (back_pressed && !s_back_was_pressed && (now_ms - s_last_back_press_ms) >= 200U) {
        s_last_back_press_ms = now_ms;
#if NEOPICO_REBOOT_SELECTOR_UI
        if (osd_visible) {
            const video_pipeline_reboot_mode_t previous_mode = s_selected_mode;
            s_selected_mode = resolution_next(s_selected_mode);
            resolution_selector_update_selection(previous_mode);
        }
#else
#if NEOPICO_RESOLUTION_MENU_720P
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
#if NEOPICO_CAPTURE_TARGET == NEOPICO_CAPTURE_TARGET_MVS
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
#endif
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

    if (osd_visible
#if NEOPICO_OSD_ROOT_MENU
        && s_screen == MENU_SCREEN_SELFTEST
#endif
        && (video_frame_count - s_last_update_frame) >= 60U) {
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
#if NEOPICO_DIAG_AUDIO_OSD
        {
            extern volatile uint32_t hstx_di_queue_silence_count;
            char buf[10];
            snprintf(buf, sizeof buf, "AU%6lu", (unsigned long)hstx_di_queue_silence_count);
            fast_osd_puts_color(14, 2, buf, OSD_COLOR_YELLOW);
        }
#endif
    }

#if NEOPICO_OSD_ROOT_MENU && NEOPICO_EXP_GENLOCK_DYNAMIC
    if (osd_visible && s_screen == MENU_SCREEN_GENLOCK && (video_frame_count - s_genlock_update_frame) >= 60U) {
        s_genlock_update_frame = video_frame_count;
        genlock_screen_update_values();
    }
#endif
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

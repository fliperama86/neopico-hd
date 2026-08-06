#include "menu_diag_experiment.h"

#include "pico/time.h"

#include "hardware/gpio.h"

#include <stdio.h>
#include <string.h>

#include "audio_source.h"
#include "settings.h"

#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
#include "audio_subsystem.h"
#endif
#include "capture_pins.h"
#include "osd/fast_osd.h"
#include "video_pipeline.h"
#if NEOPICO_MVS_COLOR_MODEL_MENU
#include "video_capture.h"
#endif
#include "osd/selftest_layout.h"

#define SELFTEST_SHADOW_HOLD_UPDATES 30U

#ifndef NEOPICO_VERSION
#define NEOPICO_VERSION "dev"
#endif

#define SELECTOR_UI_RAM(name) name
#define SELECTOR_UI_APPLY_RAM(name) SELECTOR_UI_RAM(name)

#if NEOPICO_MVS_COLOR_MODEL_MENU
// Colors: previews LIVE (video_capture_set_color_model() is called as soon as
// the value changes) but is only PERSISTED on the Video screen's Apply.
// s_selected_color_model is the live-previewed value; s_committed_color_model
// is the last value actually persisted to flash, used both for the
// green/yellow value colouring and to restore the live preview on Cancel.
static mvs_color_model_t s_selected_color_model;
static mvs_color_model_t s_committed_color_model;

static const char *color_model_label(mvs_color_model_t model)
{
    return model == MVS_COLOR_MODEL_ANALOG ? "Analog" : "Digital";
}

static const char *color_model_description(mvs_color_model_t model)
{
    return model == MVS_COLOR_MODEL_ANALOG ? "Models NEOGEO DAC levels" : "Exact RGB555 mapping";
}

static mvs_color_model_t color_model_next(mvs_color_model_t model)
{
    return model == MVS_COLOR_MODEL_ANALOG ? MVS_COLOR_MODEL_DIGITAL : MVS_COLOR_MODEL_ANALOG;
}
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Refresh (labeled "Refresh" in the Video screen; the underlying setting is
// still the genlock on/off flag): 60Hz = genlock OFF, 59.19Hz = genlock ON.
// STAGED like Resolution -- only applied to the live pipeline on Apply (via a
// reboot), never previewed live.
static const char *genlock_toggle_label(bool enabled)
{
    return enabled ? "59.19Hz" : "60Hz";
}

static const char *genlock_toggle_description(bool enabled)
{
    return enabled ? "~59.18 Hz; may reject" : "Standard rate (default)";
}

static bool genlock_toggle_next(bool enabled)
{
    return !enabled;
}
#endif

// Scanlines: LIVE, not staged -- unlike every other row on the Video screen
// (see that screen's top comment and video_row_change_value() below).
// s_video_scanline_level mirrors the pipeline's current level (there is no
// separate staged/committed pair to reconcile), but it MUST be resynced from
// video_pipeline_get_scanline_level() when the Video screen is entered: boot
// restores the persisted level into the pipeline without going through this
// file, so trusting the initializer below showed a stale value after a reboot.
static const char *scanline_level_label(uint8_t level)
{
    switch (level) {
        case VIDEO_PIPELINE_SCANLINE_25:
            return "25%";
        case VIDEO_PIPELINE_SCANLINE_50:
            return "50%";
        case VIDEO_PIPELINE_SCANLINE_75:
            return "75%";
        case VIDEO_PIPELINE_SCANLINE_100:
            return "100%";
        default:
            return "OFF";
    }
}

static uint8_t scanline_level_next(uint8_t level)
{
    return (level >= VIDEO_PIPELINE_SCANLINE_100) ? VIDEO_PIPELINE_SCANLINE_OFF : (uint8_t)(level + 1U);
}

static uint8_t scanline_level_previous(uint8_t level)
{
    return (level == VIDEO_PIPELINE_SCANLINE_OFF) ? VIDEO_PIPELINE_SCANLINE_100 : (uint8_t)(level - 1U);
}

static bool scanline_level_has_prev(uint8_t level)
{
    return level != VIDEO_PIPELINE_SCANLINE_OFF;
}

static bool scanline_level_has_next(uint8_t level)
{
    return level != VIDEO_PIPELINE_SCANLINE_100;
}

// Global frame counter from video output runtime.
extern volatile uint32_t video_frame_count;

static bool s_btn_was_pressed = false;
static uint32_t s_last_press_ms = 0;
static bool s_back_was_pressed = false;
static uint32_t s_last_back_press_ms = 0;
static uint32_t s_last_update_frame = 0;
static uint32_t s_video_hi = 0;
static uint32_t s_video_lo = 0;
static uint32_t s_video_samples = 0;
static uint32_t s_audio_hi = 0;
static uint32_t s_audio_lo = 0;
static uint32_t s_audio_samples = 0;
static uint32_t s_shadow_hold_updates = 0;
// Last drawn GP0-GP7 pressed bitmap on the Self Test screen; 0xFF forces the
// first draw after the screen is (re)entered and cleared.
static uint8_t s_gp_state_last = 0xFFU;

static inline bool osd_physical_menu_pressed(void)
{
    return !gpio_get(PIN_OSD_BTN_MENU);
}

static inline bool osd_physical_back_pressed(void)
{
    return !gpio_get(PIN_OSD_BTN_BACK);
}

#define FACTORY_RESET_HOLD_MS 5000U
static bool s_factory_reset_chord_active;
static uint32_t s_factory_reset_hold_start_ms;

static void factory_reset_buttons_tick(void)
{
    // Unconditional: any queued deferred save owns the flash record, and
    // settings_factory_reset() below does a BLOCKING write. Letting both run
    // would put two writers on the flash at once. This used to be gated on the
    // Colors menu because that was the only deferred-save producer; Scanlines
    // now queues saves in builds where Colors is compiled out.
    if (settings_save_pending()) {
        s_factory_reset_chord_active = false;
        return;
    }
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
static osd_controller_button_t s_controller_left;
static osd_controller_button_t s_controller_right;
// A activates (same role as START/MENU), B backs out (same role as SELECT).
static osd_controller_button_t s_controller_a;
static osd_controller_button_t s_controller_b;

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
    osd_controller_button_update(&s_controller_left, !gpio_get(NEOPICO_OSD_CONTROLLER_LEFT_PIN), now_ms);
    osd_controller_button_update(&s_controller_right, !gpio_get(NEOPICO_OSD_CONTROLLER_RIGHT_PIN), now_ms);
    osd_controller_button_update(&s_controller_a, !gpio_get(NEOPICO_OSD_CONTROLLER_A_PIN), now_ms);
    osd_controller_button_update(&s_controller_b, !gpio_get(NEOPICO_OSD_CONTROLLER_B_PIN), now_ms);
}

static inline bool osd_menu_pressed(void)
{
    bool pressed = osd_physical_menu_pressed();
    pressed |= s_controller_start.stable_pressed;
    return pressed;
}

static inline bool osd_back_pressed(void)
{
    bool pressed = osd_physical_back_pressed();
    pressed |= s_controller_select.stable_pressed;
    return pressed;
}

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
            return "Experimental (3x)"; // Rare Game-Mode glitch; see docs/720P_PURPLE_GLITCH.md.
        default:
            return "2x Integer Scaling";
    }
}

static video_pipeline_reboot_mode_t SELECTOR_UI_RAM(resolution_next)(video_pipeline_reboot_mode_t mode)
{
    // Cycle in display order: 240p -> 480p -> 720p.
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            return VIDEO_PIPELINE_REBOOT_MODE_720P;
        default:
            return VIDEO_PIPELINE_REBOOT_MODE_240P;
    }
}

static video_pipeline_reboot_mode_t SELECTOR_UI_RAM(resolution_previous)(video_pipeline_reboot_mode_t mode)
{
    // Cycle opposite the display order.
    switch (mode) {
        case VIDEO_PIPELINE_REBOOT_MODE_480P:
            return VIDEO_PIPELINE_REBOOT_MODE_240P;
        case VIDEO_PIPELINE_REBOOT_MODE_240P:
            return VIDEO_PIPELINE_REBOOT_MODE_720P;
        case VIDEO_PIPELINE_REBOOT_MODE_720P:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
        default:
            return VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
}

// Display order is 240p -> 480p -> 720p, non-wrapping at either end. Used to
// gate both the arrow glyphs (below) and LEFT/RIGHT navigation so neither
// implies the list wraps around.
static bool SELECTOR_UI_RAM(resolution_has_prev)(video_pipeline_reboot_mode_t mode)
{
    return mode != VIDEO_PIPELINE_REBOOT_MODE_240P;
}

static bool SELECTOR_UI_RAM(resolution_has_next)(video_pipeline_reboot_mode_t mode)
{
    return mode != VIDEO_PIPELINE_REBOOT_MODE_720P;
}

// ---------------------------------------------------------------------------
// Reusable single-line "<label>  <arrowL> <value> <arrowR>" selector row
// widget: "Resolution  <arrowL> 480p <arrowR>". Draws an arrow glyph only
// when has_prev/has_next says a neighboring option exists, blanking it with a
// space otherwise so the row never implies wrap-around. Resolution is first;
// Frequency, Colors, and Audio adopt the same widget once they move to
// single-row selectors. Cheap (a few putc/puts calls, no snprintf) so it is
// safe to call from the Core 1 background tick.
// ---------------------------------------------------------------------------
#define SELECTOR_ROW_LABEL_COL 2
// The whole arrow-value-arrow group is RIGHT-ALIGNED: the right arrow sits at a
// fixed column, the value ends just before it, and the left arrow hugs the
// value, so it shifts with the value's length rather than floating away from it
// at a fixed column. The OSD grid is FAST_OSD_COLS (28) wide.
//
// Fixed columns also fix a stale-glyph bug: values on a row differ in length
// ("59.19Hz" vs "60Hz", "100%" vs "OFF"), so redrawing in place used to leave
// the tail of a longer previous value on screen. The whole field is blanked
// first. fast_osd_putc_color bounds-checks, so an over-wide field is safe.
#define SELECTOR_ROW_VALUE_END_COL 24 // last column the value may occupy
#define SELECTOR_ROW_ARROW_RIGHT_COL 26
// Leftmost column the group can reach, with the longest value in play: the
// blanked field starts here so no stale glyph can survive outside it.
#define SELECTOR_ROW_FIELD_START_COL 16
#define SELECTOR_ROW_FIELD_COLS (FAST_OSD_COLS - SELECTOR_ROW_FIELD_START_COL)
// Longest value that still fits without pushing the left arrow past the field
// start (currently 7: "59.19Hz").
#define SELECTOR_ROW_VALUE_MAX_LEN (SELECTOR_ROW_VALUE_END_COL - SELECTOR_ROW_FIELD_START_COL - 1U)

static void SELECTOR_UI_RAM(selector_row_render)(uint8_t row, const char *label, const char *value, bool has_prev,
                                                 bool has_next, uint16_t value_color)
{
    fast_osd_puts_color(row, SELECTOR_ROW_LABEL_COL, label, OSD_COLOR_FG);
    for (uint8_t i = 0; i < SELECTOR_ROW_FIELD_COLS; i++) {
        fast_osd_putc_color(row, (uint8_t)(SELECTOR_ROW_FIELD_START_COL + i), ' ', value_color);
    }
    size_t len = strlen(value);
    if (len > SELECTOR_ROW_VALUE_MAX_LEN) {
        len = SELECTOR_ROW_VALUE_MAX_LEN; // never spill past the field start
    }
    const uint8_t value_col = (uint8_t)(SELECTOR_ROW_VALUE_END_COL + 1U - len);
    // One blank column between the left arrow and the value.
    const uint8_t arrow_left_col = (uint8_t)(value_col - 2U);
    fast_osd_putc_color(row, arrow_left_col, has_prev ? FAST_OSD_GLYPH_ARROW_LEFT : ' ', value_color);
    fast_osd_puts_color(row, value_col, value, value_color);
    fast_osd_putc_color(row, SELECTOR_ROW_ARROW_RIGHT_COL, has_next ? FAST_OSD_GLYPH_ARROW_RIGHT : ' ', value_color);
}

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
    MENU_SCREEN_VIDEO,
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    MENU_SCREEN_AUDIO,
#endif
    MENU_SCREEN_SELFTEST,
    MENU_SCREEN_REVERT_CONFIRM,
} menu_screen_t;

#define ROOT_TITLE_ROW 1
#define ROOT_FIRST_ENTRY_ROW 5
#define ROOT_HINT_ROW 13
#define ROOT_IDLE_HIDE_MS 8000U

static menu_screen_t s_screen = MENU_SCREEN_HIDDEN;
static uint8_t s_root_sel = 0;
static uint32_t s_root_last_input_ms = 0;

static const char *const s_root_entry_labels[] = {
    "Video",
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    "Audio",
#endif
    "Self Test",
};
#define ROOT_ENTRY_COUNT (sizeof(s_root_entry_labels) / sizeof(s_root_entry_labels[0]))

static menu_screen_t root_entry_screen(uint8_t idx)
{
    uint8_t i = 0;
    (void)i;
    if (idx == i++) {
        return MENU_SCREEN_VIDEO;
    }
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    if (idx == i++) {
        return MENU_SCREEN_AUDIO;
    }
#endif
    if (idx == i++) {
        return MENU_SCREEN_SELFTEST;
    }
    return MENU_SCREEN_ROOT;
}

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

// ===========================================================================
// Video screen: a multi-row form (Resolution / Refresh / Colors / Scanlines,
// plus Apply and Cancel action rows) with a single cursor, reusing the
// selector_row_render widget above for each setting row. Resolution and
// Refresh are STAGED -- changing them only updates the on-screen value, never
// the live pipeline -- until a batched Apply persists them together (at most
// one reboot). Colors keeps its existing LIVE preview
// (video_capture_set_color_model() on every change) but is likewise only
// PERSISTED on Apply. See menu_diag_experiment_arm_revert_confirm() for the
// post-Apply-reboot keep/revert safety net.
//
// Scanlines is the one EXCEPTION to all of the above, deliberate and
// user-specified: it is LIVE and fully exempt from the Apply/Cancel
// transaction (video_pipeline_set_scanline_level() applies it immediately),
// not staged, not persisted here, and untouched by both Apply and Cancel.
// Every other row on this screen is strictly all-or-nothing; this one row is
// not.
// ===========================================================================
#define VIDEO_TITLE_ROW 1
#define VIDEO_FIRST_ROW 4
#define VIDEO_ROW_STEP 2
// 14 leaves >= 2 blank rows below Cancel for every row count this screen has
// shipped with (<= 5 when Refresh or Colors is absent: Resolution/Scanlines/
// Apply/Cancel plus at most one of Refresh/Colors). Scanlines is a permanent
// row; Refresh+Colors both compiled in at once is the only remaining
// combination that reaches 6 rows, which would otherwise collide with row 14
// (Cancel would land there too) -- bump the hint down one step in that case
// only, so every other build keeps the exact existing value.
#if NEOPICO_EXP_GENLOCK_DYNAMIC && NEOPICO_MVS_COLOR_MODEL_MENU
#define VIDEO_HINT_ROW 15
#else
#define VIDEO_HINT_ROW 14
#endif

typedef enum {
    VIDEO_ROW_RESOLUTION = 0,
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    VIDEO_ROW_REFRESH,
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
    VIDEO_ROW_COLORS,
#endif
    VIDEO_ROW_SCANLINES,
    VIDEO_ROW_APPLY,
    VIDEO_ROW_CANCEL,
    VIDEO_ROW_COUNT
} video_row_t;

static video_pipeline_reboot_mode_t s_video_resolution; // staged
#if NEOPICO_EXP_GENLOCK_DYNAMIC
static bool s_video_genlock; // staged
#endif
// LIVE (see the screen comment above), not staged: always mirrors the
// pipeline's actual current level.
static uint8_t s_video_scanline_level = VIDEO_PIPELINE_SCANLINE_50;
static uint8_t s_video_row;

static inline uint8_t video_row_y(uint8_t idx)
{
    return (uint8_t)(VIDEO_FIRST_ROW + (VIDEO_ROW_STEP * idx));
}

static const char *SELECTOR_UI_RAM(video_row_description)(uint8_t idx)
{
    switch (idx) {
        case VIDEO_ROW_RESOLUTION:
            return resolution_description(s_video_resolution);
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        case VIDEO_ROW_REFRESH:
            return genlock_toggle_description(s_video_genlock);
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
        case VIDEO_ROW_COLORS:
            return color_model_description(s_selected_color_model);
#endif
        case VIDEO_ROW_SCANLINES:
            return (video_pipeline_reboot_requested_mode() == VIDEO_PIPELINE_REBOOT_MODE_240P)
                       ? "Unavailable at 240p"
                       : "Dims alternating lines";
        case VIDEO_ROW_APPLY:
            return "Save and reboot if needed";
        case VIDEO_ROW_CANCEL:
            return "Discard changes";
        default:
            return "";
    }
}

static void SELECTOR_UI_RAM(video_render_description)(void)
{
    fast_osd_puts_color(VIDEO_HINT_ROW, 2, "                          ", OSD_COLOR_GRAY);
    fast_osd_puts_color(VIDEO_HINT_ROW, 2, video_row_description(s_video_row), OSD_COLOR_GRAY);
}

// Draws both the cursor ('>' in yellow when focused, matching the root menu's
// convention) and the row's label/value/action text for one row index.
static void SELECTOR_UI_RAM(video_row_render)(uint8_t idx)
{
    const bool selected = (s_video_row == idx);
    const uint8_t row = video_row_y(idx);
    const uint16_t cursor_color = selected ? OSD_COLOR_YELLOW : OSD_COLOR_FG;
    fast_osd_putc_color(row, 0, selected ? '>' : ' ', cursor_color);
    switch (idx) {
        case VIDEO_ROW_RESOLUTION: {
            const bool active = (video_pipeline_reboot_requested_mode() == s_video_resolution);
            selector_row_render(row, "Resolution", resolution_label(s_video_resolution),
                                resolution_has_prev(s_video_resolution), resolution_has_next(s_video_resolution),
                                active ? OSD_COLOR_GREEN : OSD_COLOR_YELLOW);
            break;
        }
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        case VIDEO_ROW_REFRESH: {
            const bool active = (video_pipeline_genlock_enabled() == s_video_genlock);
            selector_row_render(row, "Refresh", genlock_toggle_label(s_video_genlock), true, true,
                                active ? OSD_COLOR_GREEN : OSD_COLOR_YELLOW);
            break;
        }
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
        case VIDEO_ROW_COLORS: {
            const bool active = (s_selected_color_model == s_committed_color_model);
            selector_row_render(row, "Colors", color_model_label(s_selected_color_model), true, true,
                                active ? OSD_COLOR_GREEN : OSD_COLOR_YELLOW);
            break;
        }
#endif
        case VIDEO_ROW_SCANLINES: {
            // Disabled at 240p (no vertical scaling, so scanlines cannot be
            // shown): no arrows, greyed out, based on the ACTIVE resolution
            // (not s_video_resolution, which may be a staged, not-yet-applied
            // change -- see the screen comment above).
            const bool disabled = (video_pipeline_reboot_requested_mode() == VIDEO_PIPELINE_REBOOT_MODE_240P);
            selector_row_render(row, "Scanlines", scanline_level_label(s_video_scanline_level),
                                !disabled && scanline_level_has_prev(s_video_scanline_level),
                                !disabled && scanline_level_has_next(s_video_scanline_level),
                                disabled ? OSD_COLOR_GRAY : OSD_COLOR_GREEN);
            break;
        }
        case VIDEO_ROW_APPLY:
            fast_osd_puts_color(row, SELECTOR_ROW_LABEL_COL, "Apply", cursor_color);
            break;
        case VIDEO_ROW_CANCEL:
            fast_osd_puts_color(row, SELECTOR_ROW_LABEL_COL, "Cancel", cursor_color);
            break;
        default:
            break;
    }
}

static void SELECTOR_UI_RAM(video_screen_render_full)(void)
{
    fast_osd_clear();
    fast_osd_puts_color(VIDEO_TITLE_ROW, 2, "NeoPico-HD Video", OSD_COLOR_YELLOW);
    for (uint8_t i = 0; i < (uint8_t)VIDEO_ROW_COUNT; i++) {
        video_row_render(i);
    }
    video_render_description();
}

// UP/DOWN: move the cursor between rows, wrapping.
static void SELECTOR_UI_RAM(video_row_move)(bool up)
{
    const uint8_t prev = s_video_row;
    if (up) {
        s_video_row = (uint8_t)((s_video_row + VIDEO_ROW_COUNT - 1U) % VIDEO_ROW_COUNT);
    } else {
        s_video_row = (uint8_t)((s_video_row + 1U) % VIDEO_ROW_COUNT);
    }
    video_row_render(prev);
    video_row_render(s_video_row);
    video_render_description();
}

// LEFT/RIGHT (wrap == false, non-wrapping, matching the arrow glyphs) or the
// physical BACK button (wrap == true, cycles forward) change the focused
// setting row's value. No effect on the Apply/Cancel rows.
static void SELECTOR_UI_RAM(video_row_change_value)(bool forward, bool wrap)
{
    switch (s_video_row) {
        case VIDEO_ROW_RESOLUTION:
            if (wrap) {
                s_video_resolution =
                    forward ? resolution_next(s_video_resolution) : resolution_previous(s_video_resolution);
            } else if (forward) {
                if (resolution_has_next(s_video_resolution)) {
                    s_video_resolution = resolution_next(s_video_resolution);
                }
            } else if (resolution_has_prev(s_video_resolution)) {
                s_video_resolution = resolution_previous(s_video_resolution);
            }
            break;
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        case VIDEO_ROW_REFRESH:
            // Two-state toggle: LEFT/RIGHT/BACK all just flip it.
            s_video_genlock = genlock_toggle_next(s_video_genlock);
            break;
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
        case VIDEO_ROW_COLORS:
            // Two-state toggle, live preview (existing Colors behavior).
            s_selected_color_model = color_model_next(s_selected_color_model);
            video_capture_set_color_model(s_selected_color_model);
            break;
#endif
        case VIDEO_ROW_SCANLINES:
            // LIVE and EXEMPT from Apply/Cancel (deliberate, user-specified;
            // see the screen comment above): applies immediately via
            // video_pipeline_set_scanline_level(), never staged. Persistence
            // is likewise immediate, not batched with Apply: it queues onto
            // the same deferred settings_request_save() path Colors uses, so
            // Core 0 performs the actual flash write at a frame boundary
            // (settings_service_pending_save() in video_capture_mvs.c /
            // video_capture_snes.c) instead of blocking here on Core 1.
            // Disabled at 240p -- LEFT/RIGHT/BACK are all ignored there,
            // matching the greyed-out, arrow-less render above.
            if (video_pipeline_reboot_requested_mode() == VIDEO_PIPELINE_REBOOT_MODE_240P) {
                return;
            }
            if (wrap) {
                s_video_scanline_level = forward ? scanline_level_next(s_video_scanline_level)
                                                 : scanline_level_previous(s_video_scanline_level);
            } else if (forward) {
                if (scanline_level_has_next(s_video_scanline_level)) {
                    s_video_scanline_level = scanline_level_next(s_video_scanline_level);
                }
            } else if (scanline_level_has_prev(s_video_scanline_level)) {
                s_video_scanline_level = scanline_level_previous(s_video_scanline_level);
            }
            video_pipeline_set_scanline_level(s_video_scanline_level);
            {
                neopico_settings_t persisted;
                settings_load(&persisted);
                persisted.scanline_level = s_video_scanline_level;
                settings_request_save(&persisted);
            }
            break;
        default:
            return; // Apply/Cancel: no effect.
    }
    video_row_render(s_video_row);
    video_render_description();
}

static void SELECTOR_UI_APPLY_RAM(video_cancel)(uint32_t now_ms)
{
#if NEOPICO_MVS_COLOR_MODEL_MENU
    // Restore the live Colors preview to the committed value. Resolution and
    // Refresh were only ever staged locally, so simply discarding them (by
    // leaving the Video screen) is enough.
    video_capture_set_color_model(s_committed_color_model);
#endif
    root_menu_enter_root(now_ms);
}

static void SELECTOR_UI_APPLY_RAM(video_apply)(uint32_t now_ms)
{
    const video_pipeline_reboot_mode_t active_resolution = video_pipeline_reboot_requested_mode();
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    const bool active_genlock = video_pipeline_genlock_enabled();
    const bool reboot_needed = (s_video_resolution != active_resolution) || (s_video_genlock != active_genlock);
#else
    const bool reboot_needed = (s_video_resolution != active_resolution);
#endif

    if (!reboot_needed) {
#if NEOPICO_MVS_COLOR_MODEL_MENU
        if (s_selected_color_model != s_committed_color_model) {
            neopico_settings_t persisted;
            settings_load(&persisted);
            persisted.resolution = (uint8_t)active_resolution;
            persisted.color_model = (uint8_t)s_selected_color_model;
            persisted.color_model_valid = NEOPICO_SETTINGS_COLOR_MODEL_VALID;
            // Deferred Core-1-safe save (Core 0 performs the actual flash
            // write at a frame boundary): no reboot follows here, so a
            // blocking write on Core 1 would stall HSTX mid-stream.
            if (settings_request_save(&persisted)) {
                s_committed_color_model = s_selected_color_model;
                root_menu_enter_root(now_ms);
            }
            return;
        }
#endif
        root_menu_enter_root(now_ms);
        return;
    }

    // Resolution and/or Refresh changed: persist the new values plus the
    // revert genlock byte (see settings.h) in the SAME write, then reboot
    // once into a PENDING-confirmation boot. The pending flag itself and the
    // revert resolution are NOT written here -- they travel in watchdog
    // scratch via video_pipeline_request_reboot_mode_pending() below, not
    // flash, so Keep never has to erase them. Only the revert genlock bit
    // still needs flash: genlock has no scratch carrier and is applied at
    // boot by reading this persisted record. The flash stall here is masked
    // by the reboot that immediately follows (same pattern the old
    // per-setting optimistic saves used).
    osd_hide();
    s_screen = MENU_SCREEN_HIDDEN;
    {
        neopico_settings_t persisted;
        settings_load(&persisted);
        persisted.resolution = (uint8_t)s_video_resolution;
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        persisted.genlock_enabled = s_video_genlock ? 1U : 0U;
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
        // All-or-nothing Video-screen Apply: stash the color model that was
        // active BEFORE this Apply (the last-committed value, about to be
        // overwritten below) so a later Revert can roll Colors back too,
        // mirroring pending_revert_genlock above.
        persisted.pending_revert_color = (uint8_t)s_committed_color_model;
        persisted.color_model = (uint8_t)s_selected_color_model;
        persisted.color_model_valid = NEOPICO_SETTINGS_COLOR_MODEL_VALID;
        s_committed_color_model = s_selected_color_model;
#endif
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        persisted.pending_revert_genlock = active_genlock ? 1U : 0U;
#else
        persisted.pending_revert_genlock = 0U;
#endif
        settings_save(&persisted);
    }
    video_pipeline_request_reboot_mode_pending(s_video_resolution, active_resolution);
}

// ===========================================================================
// Audio screen: a single horizontal selector_row_render row (order: MVS
// Digital, then AES/Other MVS), consistent with the Video screen's rows.
// s_selected_audio_source is the staged value; the option becomes active only
// once MENU commits it (unchanged commit/persist/reboot logic below). LEFT/
// RIGHT move the row non-wrapping, matching its arrow glyphs; the physical
// BACK button is the board-button fallback and wraps, matching the Video
// screen's BACK-as-fallback convention.
// ===========================================================================
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
// Audio stays a FLAT list (one row per option, cursor + committed marker),
// deliberately unlike the Video screen's horizontal selector rows.
#define AUDIO_SELECTOR_TITLE_ROW 1
#define AUDIO_SELECTOR_FIRST_OPTION_ROW 7
#define AUDIO_SELECTOR_HINT_ROW 13

static audio_source_t s_selected_audio_source;

static const char *audio_source_label(audio_source_t source)
{
    return (source == AUDIO_SOURCE_PCM1802_I2S) ? "AES/Other MVS" : "MV1C Digital";
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

static void root_menu_enter_leaf(void)
{
    const menu_screen_t leaf = root_entry_screen(s_root_sel);
    switch (leaf) {
        case MENU_SCREEN_VIDEO:
            s_video_resolution = video_pipeline_reboot_requested_mode();
#if NEOPICO_EXP_GENLOCK_DYNAMIC
            s_video_genlock = video_pipeline_genlock_enabled();
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
            s_committed_color_model = video_capture_get_color_model();
            s_selected_color_model = s_committed_color_model;
#endif
            // Read the live level back rather than trusting this file's own
            // initializer: boot restores the persisted level into the pipeline
            // (see main.c), so without this the row showed its compile-time
            // default after every reboot no matter what was saved.
            s_video_scanline_level = video_pipeline_get_scanline_level();
            s_video_row = 0;
            video_screen_render_full();
            s_screen = MENU_SCREEN_VIDEO;
            break;
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
        case MENU_SCREEN_AUDIO:
            s_selected_audio_source = audio_subsystem_get_source();
            audio_selector_render_full();
            s_screen = MENU_SCREEN_AUDIO;
            break;
#endif
        case MENU_SCREEN_SELFTEST:
            selftest_layout_reset();
            s_gp_state_last = 0xFFU;
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
        default:
            break;
    }
}

// ===========================================================================
// Batched-Apply keep/revert confirmation: reuses the same countdown UI the
// pre-rework standalone Resolution/Genlock confirms used, but now armed from
// a single revert record split across two carriers: the pending flag and the
// revert resolution live in watchdog scratch (video_pipeline.c's
// request_reboot_mode_pending()/take_pending_confirmation(), volatile by
// design so Keep costs nothing), while the revert genlock bit lives in flash
// (settings.h neopico_settings_t.pending_revert_genlock, since genlock has no
// scratch carrier). A Video-screen Apply writes both when it changes
// Resolution and/or Refresh together. See
// menu_diag_experiment_arm_revert_confirm().
// ===========================================================================
#define REVERT_CONFIRM_TIMEOUT_MS 10000U
#define REVERT_CONFIRM_HINT_ROW 13
static bool s_revert_confirm_armed = false;
static video_pipeline_reboot_mode_t s_revert_confirm_new_resolution;
static video_pipeline_reboot_mode_t s_revert_confirm_prev_resolution;
static bool s_revert_confirm_prev_genlock;
static uint32_t s_revert_confirm_deadline_ms;
static int32_t s_revert_confirm_last_secs = -1;

static void revert_confirm_render_static(void)
{
    fast_osd_clear();
    fast_osd_puts_color(1, 2, "Keep this resolution?", OSD_COLOR_YELLOW);
    fast_osd_puts_color(4, 4, resolution_label(s_revert_confirm_new_resolution), OSD_COLOR_GREEN);
    fast_osd_puts_color(7, 2, "Reverting in   s", OSD_COLOR_FG);
    fast_osd_puts_color(REVERT_CONFIRM_HINT_ROW, 2, "MENU keep   BACK revert", OSD_COLOR_GRAY);
}

// Draw the countdown digits at the "Reverting in __s" gap (no snprintf in the
// Core 1 background path).
static void revert_confirm_render_secs(int32_t secs)
{
    const uint8_t col = 2 + 13; // after "Reverting in "
    fast_osd_putc_color(7, col, (secs >= 10) ? (char)('0' + (secs / 10)) : ' ', OSD_COLOR_FG);
    fast_osd_putc_color(7, (uint8_t)(col + 1), (char)('0' + (secs % 10)), OSD_COLOR_FG);
}

static void revert_confirm_enter(uint32_t now_ms)
{
    s_screen = MENU_SCREEN_REVERT_CONFIRM;
    s_revert_confirm_deadline_ms = now_ms + REVERT_CONFIRM_TIMEOUT_MS;
    s_revert_confirm_last_secs = -1;
    revert_confirm_render_static();
    osd_show();
}

static void revert_confirm_keep(void)
{
    // Pure dismiss: nothing left to do. Apply already persisted the new
    // resolution/genlock/colors to flash, and the pending marker lived only
    // in watchdog scratch, already consumed by
    // video_pipeline_take_pending_confirmation() at boot (see main.c) -- so
    // there is no flash record to clear here. Deliberately no settings_save,
    // settings_request_save, or any other flash access: a blocking Core 1
    // flash write in this path was measured on hardware to make the HDMI
    // output lose sync.
    s_revert_confirm_armed = false;
    osd_hide();
    s_screen = MENU_SCREEN_HIDDEN;
}

static void revert_confirm_revert(void)
{
    // Roll back to the previous (confirmed) resolution, genlock setting, and
    // (when compiled) Colors, then reboot into the reverted resolution. This
    // is the all-or-nothing half of the Video screen's Apply: anything Apply
    // changed together, Revert undoes together. No pending marker to clear
    // here (it lives in scratch and was already consumed at boot); the flash
    // stall is masked by the reboot that immediately follows.
    neopico_settings_t persisted;
    settings_load(&persisted);
    persisted.resolution = (uint8_t)s_revert_confirm_prev_resolution;
    persisted.genlock_enabled = s_revert_confirm_prev_genlock ? 1U : 0U;
#if NEOPICO_MVS_COLOR_MODEL_MENU
    persisted.color_model = persisted.pending_revert_color;
    persisted.color_model_valid = NEOPICO_SETTINGS_COLOR_MODEL_VALID;
#endif
    settings_save(&persisted);
    video_pipeline_request_reboot_mode(s_revert_confirm_prev_resolution);
}

// Called by main at boot when this boot is a pending revert confirmation
// (video_pipeline_take_pending_confirmation() returned true).
void menu_diag_experiment_arm_revert_confirm(video_pipeline_reboot_mode_t new_mode,
                                             video_pipeline_reboot_mode_t revert_resolution, bool revert_genlock)
{
    s_revert_confirm_armed = true;
    s_revert_confirm_new_resolution = new_mode;
    s_revert_confirm_prev_resolution = revert_resolution;
    s_revert_confirm_prev_genlock = revert_genlock;
}

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
    bool left_edge = false;
    bool right_edge = false;
    if (s_screen == MENU_SCREEN_HIDDEN) {
        // Controller UP or SELECT alone must not open the OSD. Open once when
        // the second half of a debounced UP+SELECT chord becomes active.
        menu_edge |= s_controller_up.stable_pressed && s_controller_select.stable_pressed &&
                     (s_controller_up.press_event || s_controller_select.press_event);
    } else {
        // Once visible, controller navigation is distinct from the legacy
        // two-button physical scheme. Each press_event is one-shot.
        // A shares START's "activate" role and B shares SELECT's "back" role,
        // so either the face buttons or START/SELECT can drive the menu.
        menu_edge |= s_controller_start.press_event || s_controller_a.press_event;
        controller_select_edge = s_controller_select.press_event || s_controller_b.press_event;
        up_edge = s_controller_up.press_event;
        down_edge = s_controller_down.press_event;
        left_edge = s_controller_left.press_event;
        right_edge = s_controller_right.press_event;
    }
    if (menu_edge) {
        s_last_press_ms = now_ms;
    }
    if (back_edge) {
        s_last_back_press_ms = now_ms;
    }
    s_btn_was_pressed = menu_pressed;
    s_back_was_pressed = back_pressed;

    // A queued live save (Colors, or Scanlines) owns the flash settings record.
    // Continue sampling buttons, but ignore actions until Core 0 finishes the
    // write so no Core 1 path can read XIP while flash is unavailable.
    // Unconditional: Scanlines queues saves even when Colors is compiled out.
    if (settings_save_pending()) {
        return;
    }

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

        case MENU_SCREEN_VIDEO:
            if (controller_select_edge) {
                video_cancel(now_ms);
            } else if (up_edge != down_edge) {
                video_row_move(up_edge);
            } else if (left_edge != right_edge) {
                video_row_change_value(right_edge, false);
            } else if (back_edge) {
                // Board-button fallback: UP/DOWN/LEFT/RIGHT come from controller
                // taps that may not be wired, so the two physical buttons alone
                // must reach every row. BACK moves the cursor (matching the root
                // menu) and MENU acts on whatever row it lands on.
                video_row_move(false);
            } else if (menu_edge) {
                if (s_video_row == VIDEO_ROW_APPLY) {
                    video_apply(now_ms);
                } else if (s_video_row == VIDEO_ROW_CANCEL) {
                    video_cancel(now_ms);
                } else {
                    video_row_change_value(true, true);
                }
            }
            break;

#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
        case MENU_SCREEN_AUDIO:
            if (controller_select_edge) {
                root_menu_enter_root(now_ms);
            } else if ((up_edge != down_edge) || back_edge) {
                // Flat list: UP/DOWN (or physical BACK as the board-button
                // fallback) move the cursor between the two options. LEFT/RIGHT
                // are deliberately unbound here, since there is no horizontal
                // selector on this screen.
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

        case MENU_SCREEN_SELFTEST:
            if (controller_select_edge || menu_edge) {
                root_menu_enter_root(now_ms);
            }
            break;

        case MENU_SCREEN_REVERT_CONFIRM: {
            if (menu_edge) {
                revert_confirm_keep();
            } else if (controller_select_edge || back_edge || (int32_t)(now_ms - s_revert_confirm_deadline_ms) >= 0) {
                revert_confirm_revert(); // reboots; does not return
            } else {
                int32_t secs = (int32_t)((s_revert_confirm_deadline_ms - now_ms + 999U) / 1000U);
                if (secs > 99) {
                    secs = 99;
                }
                if (secs != s_revert_confirm_last_secs) {
                    s_revert_confirm_last_secs = secs;
                    revert_confirm_render_secs(secs);
                }
            }
            break;
        }

        default:
            break;
    }
}

void menu_diag_experiment_init(void)
{
    s_btn_was_pressed = false;
    s_last_press_ms = 0;
    s_factory_reset_chord_active = false;
    s_factory_reset_hold_start_ms = 0;
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    osd_controller_button_init(&s_controller_start, !gpio_get(NEOPICO_OSD_CONTROLLER_MENU_PIN), now_ms);
    osd_controller_button_init(&s_controller_select, !gpio_get(NEOPICO_OSD_CONTROLLER_BACK_PIN), now_ms);
    osd_controller_button_init(&s_controller_up, !gpio_get(NEOPICO_OSD_CONTROLLER_UP_PIN), now_ms);
    osd_controller_button_init(&s_controller_down, !gpio_get(NEOPICO_OSD_CONTROLLER_DOWN_PIN), now_ms);
    osd_controller_button_init(&s_controller_left, !gpio_get(NEOPICO_OSD_CONTROLLER_LEFT_PIN), now_ms);
    osd_controller_button_init(&s_controller_right, !gpio_get(NEOPICO_OSD_CONTROLLER_RIGHT_PIN), now_ms);
    osd_controller_button_init(&s_controller_a, !gpio_get(NEOPICO_OSD_CONTROLLER_A_PIN), now_ms);
    osd_controller_button_init(&s_controller_b, !gpio_get(NEOPICO_OSD_CONTROLLER_B_PIN), now_ms);
    s_back_was_pressed = false;
    s_last_back_press_ms = 0;
    s_last_update_frame = video_frame_count;
    s_video_hi = 0;
    s_video_lo = 0;
    s_video_samples = 0;
    s_audio_hi = 0;
    s_audio_lo = 0;
    s_audio_samples = 0;
    s_shadow_hold_updates = 0;
    if (osd_visible) {
        menu_diag_experiment_on_menu_open();
    }
    // This boot is awaiting a batched Apply's keep/revert confirmation: open
    // the countdown prompt.
    if (s_revert_confirm_armed) {
        revert_confirm_enter(to_ms_since_boot(get_absolute_time()));
        return;
    }
}

void menu_diag_experiment_on_menu_open(void)
{
    root_menu_draw();
    selftest_layout_reset();
    s_last_update_frame = video_frame_count;
    s_video_hi = 0;
    s_video_lo = 0;
    s_video_samples = 0;
    s_audio_hi = 0;
    s_audio_lo = 0;
    s_audio_samples = 0;
    s_shadow_hold_updates = 0;
}

void menu_diag_experiment_on_menu_close(void)
{
    // Keep existing OSD buffer contents; visibility controls display.
}

void SELECTOR_UI_RAM(menu_diag_experiment_tick_background)(void)
{
    factory_reset_buttons_tick();
    osd_controller_buttons_update(to_ms_since_boot(get_absolute_time()));
    root_menu_buttons_tick();

    if (osd_visible && s_screen == MENU_SCREEN_SELFTEST) {
        // Live controller-tap readout: shows which of GP0-GP7 is pulled low
        // right now, so a wiring map can be verified by pressing buttons
        // instead of being assumed. Redrawn only when the state changes.
        {
            uint8_t state = 0;
            for (uint8_t i = 0; i < 8U; i++) {
                if (!gpio_get(i)) {
                    state |= (uint8_t)(1U << i);
                }
            }
            if (state != s_gp_state_last) {
                s_gp_state_last = state;
                fast_osd_puts_color(15, 1, "GP", OSD_COLOR_GRAY);
                for (uint8_t i = 0; i < 8U; i++) {
                    const bool pressed = (state & (1U << i)) != 0U;
                    fast_osd_putc_color(15, (uint8_t)(4 + i), pressed ? (char)('0' + i) : '-',
                                        pressed ? OSD_COLOR_GREEN : OSD_COLOR_GRAY);
                }
            }
        }

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

    if (osd_visible && s_screen == MENU_SCREEN_SELFTEST && (video_frame_count - s_last_update_frame) >= 60U) {
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
#if NEOPICO_DIAG_AUDIO_OSD
        {
            extern volatile uint32_t hstx_di_queue_silence_count;
            char buf[10];
            snprintf(buf, sizeof buf, "AU%6lu", (unsigned long)hstx_di_queue_silence_count);
            fast_osd_puts_color(14, 2, buf, OSD_COLOR_YELLOW);
        }
#endif
    }
}

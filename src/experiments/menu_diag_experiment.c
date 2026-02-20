#include "menu_diag_experiment.h"

#include "pico/time.h"

#include "hardware/gpio.h"

#include "mvs_pins.h"
#include "osd/fast_osd.h"
#include "osd/selftest_layout.h"

#define SELFTEST_SHADOW_HOLD_UPDATES 30U

// Global frame counter from video output runtime.
extern volatile uint32_t video_frame_count;

static bool s_btn_was_pressed = false;
static uint32_t s_last_press_ms = 0;
static uint32_t s_last_update_frame = 0;
static uint32_t s_video_hi = 0;
static uint32_t s_video_lo = 0;
static uint32_t s_video_samples = 0;
static uint32_t s_audio_hi = 0;
static uint32_t s_audio_lo = 0;
static uint32_t s_audio_samples = 0;
static uint32_t s_shadow_hold_updates = 0;

void menu_diag_experiment_init(void)
{
    s_btn_was_pressed = false;
    s_last_press_ms = 0;
    s_last_update_frame = video_frame_count;
    s_video_hi = 0;
    s_video_lo = 0;
    s_video_samples = 0;
    s_audio_hi = 0;
    s_audio_lo = 0;
    s_audio_samples = 0;
    s_shadow_hold_updates = 0;
}

void menu_diag_experiment_on_menu_open(void)
{
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

void menu_diag_experiment_tick_background(void)
{
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
}

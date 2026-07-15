#!/usr/bin/env python3
"""Static source contract for the live MVS Colors selector."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def require(text: str, needle: str, context: str) -> None:
    if needle not in text:
        raise SystemExit(f"FAIL: {context} is missing {needle!r}")


def forbid(text: str, needle: str, context: str) -> None:
    if needle in text:
        raise SystemExit(f"FAIL: {context} unexpectedly contains {needle!r}")


menu_source = (REPO / "src/experiments/menu_diag_experiment.c").read_text()
buttons_start = menu_source.index("static void root_menu_buttons_tick(void)")
color_case_start = menu_source.index("case MENU_SCREEN_COLOR_MODEL:", buttons_start)
color_case_end = menu_source.index("\n            break;", color_case_start)
color_case = menu_source[color_case_start:color_case_end]

require(menu_source[buttons_start:color_case_start], "menu_edge |= s_controller_start.press_event", "menu input mapping")
require(menu_source, '"Models NEOGEO DAC levels"', "Analog Colors description")
forbid(menu_source, '"Models Neo Geo DAC levels"', "Analog Colors description")
require(menu_source, "s_committed_color_model", "committed Colors state")
require(color_case, "video_capture_set_color_model(s_committed_color_model)", "Colors SELECT revert")
require(color_case, "video_capture_set_color_model(s_selected_color_model)", "Colors live preview")
require(color_case, "s_selected_color_model == s_committed_color_model", "Colors commit comparison")
require(color_case, "settings_request_save(&persisted)", "Colors confirm case")
require(color_case, "root_menu_enter_root(now_ms)", "Colors confirm case")
forbidden_confirm_calls = (
    "settings_save(",
    "video_pipeline_request_reboot",
    "watchdog_reboot",
    "osd_hide()",
)
for call in forbidden_confirm_calls:
    forbid(color_case, call, "Colors confirm case")

capture_source = (REPO / "src/video/video_capture_mvs.c").read_text()
require(
    capture_source,
    "g_color_correct_lut[MVS_COLOR_MODEL_COUNT][32768]",
    "dual color LUT declaration",
)
require(
    capture_source,
    "frame_color_model = __atomic_load_n(&g_requested_color_model, __ATOMIC_ACQUIRE)",
    "frame-boundary selection",
)
require(
    capture_source,
    "frame_color_lut = g_color_correct_lut[frame_color_model]",
    "frame-boundary selection",
)
require(
    capture_source,
    "convert_active_pixels(dst, src, g_active_words, frame_color_lut)",
    "active conversion",
)
require(capture_source, "settings_service_pending_save()", "deferred persistence")
require(capture_source, "video_capture_resync_after_settings_save()", "post-save capture resynchronization")

print("PASS: Colors previews live, SELECT reverts, START commits, and no reboot call is present.")

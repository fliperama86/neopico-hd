#!/usr/bin/env python3
"""Static source contract for the live MVS Colors selector.

Colors moved from its own root screen onto the batched Video screen, so the
contract now tracks the Video screen's value-change / Cancel / Apply paths.
The guarantees being locked in are unchanged in spirit: Colors previews live,
backing out restores the committed value, and committing Colors on its own
never performs a blocking flash write on Core 1.
"""

from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def require(text: str, needle: str, context: str) -> None:
    if needle not in text:
        raise SystemExit(f"FAIL: {context} is missing {needle!r}")


def forbid(text: str, needle: str, context: str) -> None:
    if needle in text:
        raise SystemExit(f"FAIL: {context} unexpectedly contains {needle!r}")


def function_body(text: str, signature: str, context: str) -> str:
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"FAIL: {context} is missing {signature!r}")
    end = text.index("\n}", start)
    return text[start:end]


menu_source = (REPO / "src/experiments/menu_diag_experiment.c").read_text()
buttons_start = menu_source.index("static void root_menu_buttons_tick(void)")
video_case_start = menu_source.index("case MENU_SCREEN_VIDEO:", buttons_start)

require(menu_source[buttons_start:video_case_start], "menu_edge |= s_controller_start.press_event", "menu input mapping")
require(menu_source, '"Models NEOGEO DAC levels"', "Analog Colors description")
forbid(menu_source, '"Models Neo Geo DAC levels"', "Analog Colors description")
require(menu_source, "s_committed_color_model", "committed Colors state")

# Colors previews live the moment the value changes, and Cancel puts the
# preview back to whatever was last committed.
change_value = function_body(menu_source, "video_row_change_value)(bool forward, bool wrap)", "Video row value change")
require(change_value, "video_capture_set_color_model(s_selected_color_model)", "Colors live preview")
cancel = function_body(menu_source, "video_cancel)(uint32_t now_ms)", "Video screen Cancel")
require(cancel, "video_capture_set_color_model(s_committed_color_model)", "Colors Cancel revert")

# Applying a Colors-only change must not reboot and must not block Core 1 on
# flash: it goes through the deferred queue that Core 0 drains at a frame
# boundary. The reboot branch of the same function legitimately does call
# settings_save, so scope this check to the no-reboot branch.
apply_body = function_body(menu_source, "video_apply)(uint32_t now_ms)", "Video screen Apply")
colors_only = apply_body[: apply_body.index("// Resolution and/or Refresh changed")]
require(colors_only, "settings_request_save(&persisted)", "Colors-only Apply")
require(colors_only, "root_menu_enter_root(now_ms)", "Colors-only Apply")
for call in ("settings_save(", "video_pipeline_request_reboot", "watchdog_reboot", "osd_hide()"):
    forbid(colors_only, call, "Colors-only Apply")

# Hardware-measured invariant: a blocking Core 1 flash write in the post-Apply
# Keep path makes the HDMI output lose sync. Keep must stay a pure dismiss.
keep = function_body(menu_source, "static void revert_confirm_keep(void)", "post-Apply Keep")
for call in ("settings_save(", "settings_request_save(", "flash_range_", "video_pipeline_request_reboot"):
    forbid(keep, call, "post-Apply Keep")

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

print("PASS: Colors previews live, Cancel reverts, Colors-only Apply defers its write, and Keep touches no flash.")

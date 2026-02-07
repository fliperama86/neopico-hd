# OSD Diagnostics Screen

This document describes the design for a diagnostics / self-test screen in the OSD. The diagnostics screen shows system health (clocks, PLL lock, HDMI status, sync, input signal info) and must be decoupled from the OSD renderer, update at most once per second, and work even when video capture is broken.

## Design Goals

- **Decoupled**: The diagnostics **function** only produces lines of text; the OSD only accepts cols/lines and renders them. No diagnostics logic inside the OSD.
- **Update rate**: No real-time requirement; updating every second is sufficient.
- **Robustness**: Must work when capture is broken (no input signal, line ring empty, etc.).
- **Timing**: No processing during the Core 1 DMA ISR; the ISR only copies from the pre-rendered OSD buffer.

## Answers to Key Questions

### Can we keep video capture and output while the diagnostics screen is open?

**Yes.** The existing pipeline already supports this:

- The scanline callback uses **loop splitting**: before OSD region → OSD box region → after OSD region.
- For the “before” and “after” regions it still reads from the capture line ring (`src`) when available and doubles those pixels.
- Only the OSD rectangle is taken from `osd_framebuffer`.
- So capture continues to fill the ring buffer, HDMI output continues, and only the OSD box area shows the overlay; the rest remains live video (or blue if there is no signal).

### How can we implement this without processing during the ISR?

**By doing zero work in the ISR.** The ISR must only **read** the pre-rendered `osd_framebuffer` and double pixels. It already does exactly that: e.g. `video_pipeline_double_pixels_fast(dst + OSD_BOX_X, osd_src, OSD_BOX_W)` with no per-pixel branching.

Therefore:

- **Diagnostics** (and any other OSD “screen”) only **produce** text lines (e.g. an array of strings).
- **OSD** only **renders** those lines into `osd_framebuffer` using the existing 8×8 font (`osd_clear()` + `osd_puts()` per line). This rendering runs **outside** the ISR (e.g. in the Core 1 background task).
- The ISR never does formatting, `snprintf`, or branching on content; it only copies from `osd_framebuffer`. This is the required approach to avoid breaking HDMI timing.

## Decoupled Architecture

### OSD (rendering only)

- **Role**: Accept lines (or a text buffer with cols/lines) and draw them into the OSD framebuffer.
- **API** (conceptual):
    - `osd_render_lines(const char *lines[], int num_lines)`
      or
    - `osd_render_text(int cols, int rows, const char *text)`
      with a single flat string and line length = cols.
- **Implementation**: Clear `osd_framebuffer`, then for each line call the existing `osd_puts(x, y, line)` (or equivalent using the 8×8 font). No knowledge of “diagnostics” or “debug”.
- **Location**: All OSD buffer writes happen on **Core 1**, in non-ISR code (e.g. from the background task), per `AGENTS.md` and `docs/OSD_IMPLEMENTATION.md`.

### Diagnostics (data only)

- **Role**: Fill a line buffer with the current diagnostic strings (clock source, PLL lock, HDMI lock, sync status, input resolution, lag, etc.). No direct dependency on the OSD.
- **API** (conceptual):
    - `diagnostics_get_lines(char (*lines)[MAX_COLS], int max_lines)`
      or
    - `diagnostics_get_lines(char *buf, int buf_size, int cols)`
      so the caller gets a consistent snapshot of lines to render.
- **Data sources**: Hardware registers, shared state (e.g. frame count, sync loss flag), and capture/audio status. When capture is broken, diagnostics can still report “no signal”, PLL, HDMI, etc.
- **Location**: Can run on Core 0 (e.g. in the capture loop when diagnostics screen is active) or on Core 1 in the background task. If it needs capture-related state, that can live in shared variables or a small shared struct; diagnostics only reads and formats.

### Coordinator (ties diagnostics and OSD)

- **Role**: At ~1 Hz, when the diagnostics screen is active, get lines from diagnostics and ask the OSD to draw them.
- **Location**: Core 1, inside the existing background task or a wrapper around it.
- **Flow**:
    1. If diagnostics screen is not active → do nothing for OSD content.
    2. If active → call diagnostics to fill the line buffer (or read from a shared buffer updated by Core 0), then call `osd_render_lines(...)` so that all writes to `osd_framebuffer` occur on Core 1, outside the ISR.

This keeps the OSD a dumb renderer (cols/lines only) and allows the diagnostics screen to work even when capture is broken.

## Behavior When Capture Is Broken

- When there is no input signal, the capture loop no longer gets vsync and follows the no-signal path (e.g. timeouts, `video_capture_reset_hardware()`, `tud_task()`). The line ring may be stale or empty, so `line_ring_ready(mvs_line)` can be false and `src` in the scanline callback is NULL.
- The scanline callback **already** handles `src == NULL`: it outputs blue for non-OSD regions and still outputs **OSD from `osd_framebuffer`** for the OSD box. So the diagnostics overlay remains visible with no input signal.
- The only requirement is that the **diagnostics content** is refreshed without depending on a healthy capture loop. Options:
    - Drive the 1 Hz refresh from **Core 1** (e.g. background task using `video_frame_count` or a timestamp), so it runs regardless of capture state; diagnostics then uses hardware/registers and shared state that Core 0 updates when it can.
    - Or have Core 0 update a shared “diagnostics line buffer” and a “refresh” flag in both the normal and no-signal paths, and Core 1 only reads and renders when the flag is set.

## Where the 1 Hz Update Runs

- Core 1 has a single registered `background_task` (used for audio). So either:
    - **Wrap** that task: the registered task calls `audio_background_task()` and then runs the diagnostics OSD refresh step; or
    - **Extend** the code that registers the background task so the same task also runs a 1 Hz diagnostics refresh (e.g. using `video_frame_count` or a timestamp to run “every ~60 frames” or “every 1 s”).
- The 1 Hz logic:
    - If the diagnostics screen is not active → skip OSD refresh.
    - If active → call diagnostics to fill the line buffer (or read shared buffer), then `osd_render_lines(...)`.

All rendering into `osd_framebuffer` stays on Core 1 and outside the ISR.

## Summary

| Concern                   | Approach                                                                                  |
| ------------------------- | ----------------------------------------------------------------------------------------- |
| Video capture/output      | Kept; only OSD box is overlaid; pipeline already uses loop splitting.                     |
| No ISR processing         | ISR only copies from pre-rendered `osd_framebuffer`; no formatting.                       |
| Decoupling                | Diagnostics produces lines; OSD renders lines; coordinator connects them.                 |
| Works when capture broken | Scanline path already draws OSD when `src == NULL`; refresh from Core 1 or shared buffer. |
| 1 Hz update               | In Core 1 background task (or wrapper), using frame count or timestamp.                   |

## References

- `docs/OSD_IMPLEMENTATION.md` – OSD buffer, injection during scanline doubling, timing constraints.
- `docs/ARCHITECTURE.md` – Core roles, memory map, critical constraints.
- `AGENTS.md` – OSD on Core 1, 8×8 font, overlay during scanline doubling.

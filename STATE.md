# NeoPico-HD — Live State

**Contract:** this document is LIVE — edited in place to always reflect current
reality. Any agent or human picking up the project starts here. The
append-only history stays in `SCRATCHBOOK.md`; architecture rules in
`AGENTS.md`; protocol details in `docs/`.

_Last updated: 2026-06-12 (morning; baseline established)._

## Current state

- `main` HEAD includes:
  - `981efab` — pico_hdmi upgraded to **2.0-beta** (`7fa6dd5`, branch
    `2.0-beta` on fliperama86/pico_hdmi). Behaviorally inert: all new
    mechanisms opt-in and OFF. Free fix included: audio underruns no longer
    assert the IEC block-start flag.
  - `3d1cb91` — **experimental precomposed/native scanout path** behind
    `NEOPICO_EXP_PRECOMPOSED_HDMI` (OFF default; requires
    `NEOPICO_USE_NONRT_HDMI=ON`, 480p only). Zero-copy video rows (hardware
    2x doubling), static color lines for letterbox/no-signal, OSD rows via
    native-width ping/pong scratch, audio islands patched by the library ISR
    (starvation-proof).
- **Uncommitted work, parked as a patch**: root OSD menu (Resolution / Self
  Test entries) + desync watchdog + RS counter —
  `/tmp/neopico-rootmenu-watchdog.patch` (394 lines). Reverted from the tree
  because its mere presence made the firmware glitch (see Layout
  Sensitivity below). Re-introduce only after that mystery is understood.
- Build dirs: `build/` (480p RT, flags off), `build-720p-nonrt/`,
  `build-precomp/` (NONRT + PRECOMPOSED + OSD + SELFTEST — config of the
  current soak).

## Run log (keep this updated; one line per soak)

| # | Date | Firmware (sha + flags) | Full cold-boot ritual | OSD opened | Duration | Outcome |
|---|------|------------------------|----------------------|------------|----------|---------|
| 1 | 06-11 | v0.6.0 GitHub release | yes | n/a | >1 h | clean |
| 2 | 06-11 | 981efab+3d1cb91, all flags OFF (`build/`) | yes | n/a (no OSD) | ~1 h | clean |
| 3 | earlier | 3d1cb91 state, PRECOMP+OSD+SELFTEST | **no** (probably warm boot, cables attached) | opened a few times, mostly closed | ~20–30 min | **sync drop, no recovery** |
| 4 | 06-11 | same as #3, committed source only | yes | opened at least twice (early + late) | ~45-50 min | **sync drop** (press-correlation suspected at the time) |
| 6 | 06-12 | PURE PHASE 2 baseline (sha 76c1233d, NONRT+PRECOMP, OSD/selftest OFF) | yes | impossible (not compiled) | ~8 h overnight (00:14 ->morning) | **CLEAN — pure phase 2 exonerated; OSD/selftest-compiled implicated (3/3 drops vs 0/1 over 8 h)** |
| 5 | 06-12 | same fw, cold boot, OSD opened/closed ~20x AT START (mash test) | yes | heavily, at start only | ~30-60 min | **sync drop, long after the presses** — refutes INSTANT-trigger only; the snowball variant (one open seeds slow failure) remains live |

## Open threads

1. **Precomposed sync drops (runs #3 AND #4)** — both failures occurred in
   OSD-compiled builds with the OSD opened during the run; run #4's drop may
   have immediately followed a button press (user observation). Working
   hypothesis: the OSD-OPEN transition (or OSD-row compose while visible)
   triggers the failure — the instant-trigger version is refuted by run #5
   (mashing at start, drop much later), but the SNOWBALL variant (a single
   open seeds a slow-developing failure) is consistent with all three drops
   and remains untested. Discriminator: same OSD build, never press the
   button. Current shape: 3 drops in 3 long runs of
   OSD+selftest-compiled precomposed builds, varied boot ritual and OSD
   usage; looks like a rare stochastic event (~once per 20-60 min). The
   no-OSD baseline is now the critical discriminator: if IT also drops,
   OSD/selftest are exonerated and the library mode (ctrl-swap race) is
   prime; if it holds for hours, OSD/selftest-compiled is implicated
   (code presence/layout, ISR compose, or background sampling). Cross-project note: rp2350-doom (same lib mode)
   logs rare desyncs too (its issue #1), auto-recovered by a watchdog there.
   Library-side suspect if real: the per-post 16/32-bit `al1_ctrl` swap in
   native pixel mode racing a late/coalesced DMA IRQ.
2. **Layout sensitivity (REPRODUCED 2026-06-11)** — adding 394 lines of
   benign flash-resident background-path code (zero scratch, zero capture
   path) made the firmware immediately glitchy; reverting restored calm,
   same bench, same session. Execution was ruled out (fixing a real bug in
   the added watchdog changed nothing). Prime suspect: flash/XIP
   placement/alignment shifts of hot non-scratch code. Reproducer: apply
   the parked patch. Next probes: re-add in halves; forced alignment;
   pin hot symbols to RAM; diff `.map` addresses calm-vs-glitchy.
   This may also be the underlying mechanism of thread 1.
3. **Watchdog (parked)** — rate-based formula required
   (`frames*100 > elapsed_ms*12`); a fixed count threshold false-fires
   because this Core 1 background loop legitimately stalls 100s of ms
   (audio SRC bursts). Re-enter via pico_hdmi itself, not app code.

## Canonical baseline (ESTABLISHED 2026-06-12)

**Pure Phase 2** passed an ~8 h overnight soak (run #6): commits
`981efab`+`3d1cb91`, flags `NEOPICO_USE_NONRT_HDMI=ON
NEOPICO_EXP_PRECOMPOSED_HDMI=ON NEOPICO_ENABLE_OSD=OFF`, UF2 sha
`76c1233dc3b0a6f2e46ffa1bc6fef402d3c1c5de` (build dir
`build-precomp-baseline/`; regenerable from commit+flags; keep its
`.elf.map` for structural diffs). All future additions are measured
against this: behaviorally (soak) and structurally (map symbol diffs).

## Next experiment (#7): presence vs. opening

OSD+selftest build (same as runs #3-#5), full ritual, **never press the
button**. Clean for hours -> opening seeds the failure (user's snowball
hypothesis confirmed; focus on first-open effects: osd_visible_latched
path, first ISR compose, fast_osd_clear burst, selftest sampling start).
Drops anyway -> mere code PRESENCE is enough, merging this thread with
the Layout Sensitivity thread (flash/XIP alignment becomes prime).

## Standing constraints (digest; full rules in AGENTS.md + SCRATCHBOOK)

- scratch_x hard boundary 0x800; nothing new in scratch sections, ever,
  without measuring (`grep -A1 '^\.scratch_x' src/neopico_hd.elf.map`).
- Capture path: no additions, even dormant code.
- OSD render pattern: full draw on screen entry, glyph-only updates, ~1 Hz.
- No printf/blocking I/O on the Core 1 background path (rp2350-doom lesson:
  one UART line = 7 ms stall = audible/visible artifacts).
- Validate audio transports with a pure sine + spectrogram, not square
  waves or SFX (they mask dropped-sample artifacts).
- Flag-gate everything experimental, default OFF, verify flag-off UF2 is
  byte-identical (it was, sha-verified, for both phase 2 and the menu).

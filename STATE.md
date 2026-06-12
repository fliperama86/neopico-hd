# NeoPico-HD — Live State

**Contract:** this document is LIVE — edited in place to always reflect current
reality. Any agent or human picking up the project starts here. The
append-only history stays in `SCRATCHBOOK.md`; architecture rules in
`AGENTS.md`; protocol details in `docs/`.

_Last updated: 2026-06-12 (evening; Scenario B ratified: precomposed into runtime-modes path)._

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
- Root OSD menu + desync watchdog + RS counter: EXONERATED (the glitch was
  XIP, not the code) and committed alongside the copy-to-RAM fix. The
  root menu (`NEOPICO_OSD_ROOT_MENU`) and watchdog ship with v0.7.1.
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
| 8 | 06-12 | ORACLE: glitch-patch code + OSD+selftest + COPY_TO_RAM | yes | open for most of the run (RS visible) | 12:30 -> 15:06+, ongoing | **CLEAN, RS=0 — every previously fatal ingredient active, from RAM; XIP conviction CONFIRMED for thread 1** |
| 7 | 06-12 | same OSD+selftest fw as #3-#5, EXPERIMENT 7 | yes | **NEVER (hands-off protocol)** | 11:27 -> ~12:28 | **sync drop with ZERO opens — Theory B (open-seeded snowball) REFUTED; Theory A (code presence / XIP layout) now PRIME** |
| 5 | 06-12 | same fw, cold boot, OSD opened/closed ~20x AT START (mash test) | yes | heavily, at start only | ~30-60 min | **sync drop, long after the presses** — refutes INSTANT-trigger only; the snowball variant (one open seeds slow failure) remains live |

## Open threads

1. **Precomposed sync drops (runs #3 AND #4)** — both failures occurred in
   OSD-compiled builds with the OSD opened during the run; run #4's drop may
   have immediately followed a button press (user observation). Working
   hypothesis: the OSD-OPEN transition (or OSD-row compose while visible)
   triggers the failure — fully REFUTED by run #7 (zero opens, dropped
   anyway). VERDICT: compiled PRESENCE of OSD+selftest code is sufficient
   (4/4 drops; 0/1 over 8 h without). Prime theory: flash/XIP layout
   sensitivity (merges with thread 2). Counterattack: copy-to-RAM
   (pico_set_binary_type copy_to_ram) — text is only ~43 KB, fits SRAM
   with ~120 KB spare, eliminates XIP at runtime BY CONSTRUCTION. Fast
   oracle: the 394-line patch glitches INSTANTLY from flash; if it is calm
   from RAM, the mechanism is proven in minutes. Current shape: 3 drops in 3 long runs of
   OSD+selftest-compiled precomposed builds, varied boot ritual and OSD
   usage; looks like a rare stochastic event (~once per 20-60 min). The
   no-OSD baseline is now the critical discriminator: if IT also drops,
   OSD/selftest are exonerated and the library mode (ctrl-swap race) is
   prime; if it holds for hours, OSD/selftest-compiled is implicated
   (code presence/layout, ISR compose, or background sampling). Cross-project note: rp2350-doom (same lib mode)
   logs rare desyncs too (its issue #1), auto-recovered by a watchdog there.
   Library-side suspect if real: the per-post 16/32-bit `al1_ctrl` swap in
   native pixel mode racing a late/coalesced DMA IRQ.
2. **Layout sensitivity — SOLVED 2026-06-12: XIP CONVICTED.** The oracle
   experiment: the exact instant-glitch code (394-line patch) built with
   `copy_to_ram` (one CMake line; text ~47 KB fits SRAM easily since both
   LUTs are runtime-built BSS) runs CALM. Flash-vs-RAM execution was the
   only variable. Mechanism: XIP fetch stalls at timing-critical instants,
   with per-build flash layout deciding where they land. This also explains
   the historical "dead code perturbs sync" project folklore. STRUCTURAL
   FIX: `NEOPICO_COPY_TO_RAM=ON` (new CMake option, wired via
   pico_set_binary_type). Thread 1 presumed same mechanism — one confirming
   long soak of the copy-to-RAM OSD build pending (running now, includes
   watchdog + RS counter for instrumentation).
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

## RT path × copy-to-RAM: TOXIC (2026-06-12 bisect)

Rung-1 bisect: RT library path + COPY_TO_RAM alone (no OSD/menu/switch)
drops sync within SECONDS. RT from flash is fine (run #2, 1 h). Working
theory: with the whole binary in striped SRAM, Core 0 + background code
fetches contend on the same banks the heavyweight RT copy-model ISR and
its DMA feeds depend on; the RT per-line deadline has no margin for it.
The precomposed (tiny-ISR) path is unaffected (hours clean from RAM).
POLICY: copy_to_ram ONLY for precomposed/non-RT builds; RT variants stay
on flash (with their historical XIP layout lottery — the strategic exit
is migrating RT modes to the precomposed architecture). CI matrix
corrected in v0.7.2; v0.7.1's default artifacts shipped broken (RT+RAM)
and are superseded.

## ARCHITECTURE DECISION (2026-06-12, user-ratified): Scenario B

RT-with-features bisect ABANDONED: user context confirms only vanilla 480p
was ever rock-solid pre-2.0 (even 720p dropped in a demo); RT+features is
unconquered territory, not a regression. The destination is **rung 3: port
the precomposed tiny-ISR architecture into the runtime-modes path** (one
firmware, all resolutions, robust ISR; endgame deletes the copy-model ISR
and converges the lib = pico_hdmi 2.1). Mode plan: 480p zero-copy (done),
720p = 3x via pre-expanded line ring outside the ISR + hardware 2x,
240p = 4x via pre-doubled ring + hardware 2x. The OSD composes at native
resolution before any expansion (resolution-independent cost).
Currently on bench: precomposed + root menu (Self Test entry) +
copy-to-RAM — first execution of the menu code on the healthy
architecture (build-precomp-menu/).

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

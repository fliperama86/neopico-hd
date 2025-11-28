# MVS Video Capture - Project Status

**Last Updated**: 2025-11-28

## Quick Summary

🎮 **Project**: Neo Geo MVS video capture using Raspberry Pi Pico
📊 **Progress**: 85% complete - deterministic capture working!
🚀 **Current Phase**: Single-bit R4 capture validated, ready for RGB expansion
⏱️ **Time Invested**: ~12 hours including CSYNC research and jitter elimination
🎯 **Major Achievement**: Hardware IRQ synchronization eliminates timing jitter completely
🔨 **Current Build**: `main_dma.c` (DMA-based R4 single-bit capture)

## Development Methodology

**Approach**: Baby steps with validation at each stage

- ✅ Validate assumptions before proceeding
- ✅ Test with known signals (6MHz clock, color test pattern)
- ✅ Build complexity incrementally
- ✅ Document learnings as we go

## Progress Tracker

### Phase 1: Signal Validation ✅ COMPLETE

| Step | Status | Description              | Outcome                       |
| ---- | ------ | ------------------------ | ----------------------------- |
| 1.1  | ✅     | Capture 6MHz pixel clock | 6.001 MHz measured (perfect!) |
| 1.2  | ✅     | Measure CSYNC frequency  | ~506 kHz (all edges)          |
| 1.3  | ✅     | Derive frame parameters  | 59.20 fps, 15.6 kHz HSYNC ✓   |
| 1.4  | ✅     | Validate timing math     | All calculations match spec   |

**Key Achievement**: Confirmed MVS signals match cps2_digiav specifications

---

### Phase 2: Sync Decoding ✅ COMPLETE

| Step | Status | Description                | Outcome                               |
| ---- | ------ | -------------------------- | ------------------------------------- |
| 2.1  | ✅     | Understand CSYNC structure | Contains HSYNC + VSYNC + equalization |
| 2.2  | ✅     | Count scanlines            | 264 lines/frame detected              |
| 2.3  | ✅     | Detect frame boundaries    | VSYNC identification working          |
| 2.4  | ✅     | Calculate derived metrics  | Frame rate, line rate accurate        |

**Key Achievement**: Successfully decoded composite sync without FPGA

---

### Phase 3: Pixel Data Capture ✅ COMPLETE

| Step | Status | Description              | Outcome                         |
| ---- | ------ | ------------------------ | ------------------------------- |
| 3.1  | ✅     | Create PIO pixel sampler | mvs_sync.pio - mvs_pixel_capture program |
| 3.2  | ✅     | Capture R4 (Red MSB)     | 302 kHz edge rate measured      |
| 3.3  | ✅     | Validate with gameplay   | Activity changes with content ✓ |
| 3.4  | ✅     | Verify timing            | 5% pixel activity makes sense   |

**Key Achievement**: Proven that we're capturing real video data!

**Test Case**: Metal Slug X

- R4 edges: 5,110 per frame
- Changes with explosions/movement
- Low baseline during static scenes

---

### Phase 4: Frame Visualization ✅ COMPLETE

| Step | Status | Description                  | Outcome                                |
| ---- | ------ | ---------------------------- | -------------------------------------- |
| 4.1  | ✅     | Design frame buffer          | DMA + post-processing architecture     |
| 4.2  | ✅     | Implement capture loop       | Hardware IRQ sync eliminates jitter    |
| 4.3  | ✅     | Output PBM format            | Clean 320×224 frame output             |
| 4.4  | ✅     | Test with color test pattern | Test pattern clearly visible           |
| 4.5  | ✅     | Verify image quality         | Text readable, blocks recognizable     |
| 4.6  | ✅     | Eliminate timing jitter      | ±0 pixels with HSYNC synchronization   |
| 4.7  | ✅     | Calibrate frame offsets      | H=6 pixels, V=20 lines (setup-tuned)   |

**Key Achievement**: Deterministic, zero-jitter frame capture

**Breakthrough**: Hardware IRQ synchronization using PIO `wait 1 irq 4` + `pio_sm_exec()` trigger eliminates all C code latency in capture start timing

---

### Phase 5: Full RGB Capture ⏳ NOT STARTED

| Step | Status | Description               | Notes                          |
| ---- | ------ | ------------------------- | ------------------------------ |
| 5.1  | ⏳     | Wire G4, B4 pins          | GP3, GP4 (R4 already connected)|
| 5.2  | ✅     | Create 3-bit RGB PIO      | mvs_rgb_capture ready in mvs_sync.pio |
| 5.3  | ⏳     | Create main_rgb.c         | Integrate RGB PIO program      |
| 5.4  | ⏳     | Handle packed RGB data    | 3 bits/pixel post-processing   |
| 5.5  | ⏳     | Output PPM format         | Color bitmap                   |
| 5.6  | ⏳     | Test with test pattern    | Verify colors                  |
| 5.7  | ⏳     | Expand to full 15-bit RGB | All 5 bits per channel         |

**Status**: PIO program ready (mvs_rgb_capture in mvs_sync.pio), C code integration pending
**Note**: Requires hardware wiring of G4 (GP3) and B4 (GP4) pins before testing

---

## Milestone Timeline

```
Day 1 (2025-10-13):
├─ 00:00 ✅ Started with frequency counter
├─ 00:30 ✅ Measured 6MHz pixel clock
├─ 01:00 ✅ Added CSYNC monitoring
├─ 01:30 ✅ Understood CSYNC structure
├─ 02:00 ✅ Validated frame parameters
├─ 02:30 ✅ Captured R4 pixel data
├─ 03:00 ✅ Saw activity in Metal Slug X
└─ 03:30 ✅ Implemented basic frame capture

Day 2 (2025-10-14):
├─ 00:00 ✅ Researched CSYNC implementations (cps2_digiav, PicoDVI-N64)
├─ 01:00 ✅ Created CSYNC_IMPLEMENTATION_PLAN.md
├─ 02:00 ✅ Phase 4a: Horizontal counter PIO
├─ 03:00 ✅ Phase 4b: Equalization detection
├─ 04:00 ✅ Phase 4c: Frame counter
├─ 05:00 ✅ Phase 4d: Timing signals
├─ 06:00 ✅ R4 pixel capture attempts (multiple approaches)
├─ 07:00 ✅ DMA-based full frame capture working
├─ 08:00 🔄 Fixed random alignment jitter
├─ 09:00 ✅ Hardware IRQ synchronization implemented
├─ 10:00 ✅ Frame alignment calibrated (H=6, V=20)
├─ 11:00 ✅ Verified with test pattern - readable image! [YOU ARE HERE]
└─ 12:00 ⏳ Next: 3-bit RGB color capture
```

---

## Technical Achievements

### ✅ Working Features

1. **Signal Monitoring**

   - 6 MHz pixel clock: ±0.017% accuracy
   - CSYNC edge counting: 506 kHz
   - Frame rate calculation: 59.19 fps

2. **Sync Decoding**

   - Horizontal counter PIO (h_ctr via pixel clock counting)
   - Equalization pulse detection (h_ctr < 288 threshold)
   - VSYNC detection via 18 equalization pulses
   - Line counter (0-263) with frame boundary tracking

3. **Hardware IRQ Synchronization** 🎯

   - PIO-to-PIO communication via IRQ flags
   - Pixel PIO waits at deterministic `wait 1 irq 4` instruction
   - C code triggers via `pio_sm_exec(pio, sm_sync, pio_encode_irq_set(false, 4))`
   - Synchronizes to first HSYNC after second VSYNC
   - **Result**: ±0 pixels jitter (was ~5 pixels with time-based delays)

4. **DMA-Based Capture**

   - Zero CPU overhead during capture
   - Captures ~109K pixels per frame (full frame + blanking)
   - Paced by PIO DREQ signal
   - Post-processes to extract 320×224 active region

5. **Frame Output**
   - Calibrated frame offsets (H=6, V=20 pixels)
   - Clean 320×224 frame extraction
   - PBM format generation
   - Test pattern validation: text readable, blocks recognizable

### 🔨 In Progress

1. **3-bit RGB Capture**
   - PIO program complete (mvs_rgb_capture)
   - Ready for C code integration

### ⏳ Planned

1. **Full 15-bit RGB Capture**
2. **Frame Statistics** (brightness, activity)
3. **Multi-frame Capture** (animations)
4. **Continuous streaming** (if bandwidth allows)

---

## Resource Usage

### Memory

| Component            | Size       | % of 264KB RAM |
| -------------------- | ---------- | -------------- |
| Frame buffer (1-bit) | 8,960 B    | 3.3%           |
| PIO programs         | ~200 B     | 0.1%           |
| Stack/variables      | ~1 KB      | 0.4%           |
| **Total Used**       | **~10 KB** | **3.8%**       |
| **Available**        | **254 KB** | **96.2%**      |

**Capacity for future**:

- Full RGB (15-bit): 134 KB (51%)
- Double buffering RGB: 268 KB (would need optimization)
- 3x 1-bit frames: Fits easily

### GPIO Pins

| Usage                      | Pins   | Available     |
| -------------------------- | ------ | ------------- |
| Currently used             | 3      | GP0-GP2       |
| Full RGB needed            | 15     | GP2-GP16      |
| DARK/SHADOW                | 2      | GP17-GP18     |
| **Total for full capture** | **20** | **GP0-GP19**  |
| **Pico total pins**        | **26** | -             |
| **Remaining**              | **6**  | For expansion |

### PIO State Machines

| PIO Block           | SM Used           | SM Free       |
| ------------------- | ----------------- | ------------- |
| pio0                | 1 (pixel capture) | 3             |
| pio1                | 0                 | 4             |
| **Total available** | **7**             | Can add more! |

---

## Known Issues & Solutions

### Issue 1: VSYNC Detection Timing

**Problem**: Simple GPIO polling for VSYNC may miss transitions
**Impact**: Low - frame sync still works
**Solution**: Move VSYNC detection to PIO (future enhancement)
**Priority**: Low

### Issue 2: Horizontal Blanking Estimation

**Problem**: Using `sleep_us(10)` to skip blanking
**Impact**: May capture some blanking pixels
**Solution**: Count pixel clocks in PIO, or calibrate delay
**Priority**: Medium - test current approach first

### Issue 3: USB Buffer Overflow

**Problem**: Large PBM output may overflow USB buffer
**Impact**: Not observed yet
**Solution**: Binary PBM (P4) format, or chunk output
**Priority**: Low - monitor during testing

---

## Next Actions

### Immediate (Next session)

1. ⏳ Wire G4 (GP3) and B4 (GP4) pins to Pico
2. ⏳ Verify hardware connections with continuity test
3. ⏳ Test mvs_rgb_capture PIO program with known good signal

### Short Term

1. ⏳ Integrate mvs_rgb_capture into C code (create main_rgb.c or extend main_dma.c)
2. ⏳ Implement PPM (color) format output
3. ⏳ Test 3-bit color capture with test pattern
4. ⏳ Adjust frame offsets if needed for RGB channels

### Medium Term (Future sessions)

1. ⏳ Expand to full 15-bit RGB (all 5 bits per channel)
2. ⏳ Add DARK/SHADOW signal support
3. ⏳ Implement frame statistics engine
4. ⏳ Motion detection / frame differencing

---

## Success Criteria

### Phase 4 - Frame Capture ✅ COMPLETE

- [x] Capture complete 320×224 frame
- [x] Output valid PBM format
- [x] **Image shows recognizable pattern** (text readable, blocks visible)
- [x] No timing jitter (±0 pixels with hardware IRQ sync)
- [x] Repeatable captures (deterministic alignment)

### Phase 5 (Current) - RGB Capture

- [x] Create 3-bit RGB PIO program
- [ ] Wire G4, B4 pins to Pico
- [ ] Integrate RGB PIO into main_dma.c
- [ ] Output valid PPM format (3-bit color)
- [ ] Verify colors with test pattern
- [ ] Expand to full 15-bit RGB (future)

---

## Questions for Future Investigation

1. **Can we capture at full 59 fps to USB?**

   - Current: One frame per 5 seconds
   - Full RGB: ~8 MB/s → would need compression

2. **Should we use DMA instead of polling?**

   - Pros: Zero CPU, more reliable timing
   - Cons: More complex setup
   - Decision: Try after Phase 5

3. **Can we detect specific game events?**

   - Frame differencing for motion
   - Brightness analysis for explosions
   - Could enable automated testing

4. **Could we add sprite extraction?**
   - Identify sprite boundaries
   - Extract character graphics
   - Build asset library

---

## References & Related Work

### Our Work

- [README.md](README.md) - Original frequency counter docs
- [MVS_CAPTURE.md](MVS_CAPTURE.md) - Technical deep dive
- This file - Project tracking

### External Projects

- **cps2_digiav**: FPGA-based MVS→HDMI converter (reference implementation)
- **PicoDVI-N64**: N64 video capture with Pico (inspiration)
- **PicoJoybus**: N64 controller via PIO (sync decoding techniques)

### Documentation

- RP2040 Datasheet: PIO chapter
- Pico SDK: hardware_pio API
- MVS Hardware Wiki: Video timing specs

---

## Development Tips (Learned)

### What Worked Well ✅

1. **Baby steps approach**: Validate each piece before next
2. **Known test signals**: Color bars make debugging easy
3. **Math validation**: Derive expected values, measure, compare
4. **Progressive complexity**: 1-bit → full RGB gradually
5. **Documentation**: Write down findings immediately

### What to Avoid ⚠️

1. ❌ Don't capture full RGB first (too complex to debug)
2. ❌ Don't assume timing (always measure)
3. ❌ Don't skip validation steps (leads to confusion)
4. ❌ Don't over-engineer early (PIO edge counter worked great)
5. ❌ Don't forget test patterns (color bars saved us)

### Tools That Helped 🛠️

- `screen` for serial monitoring
- `printf` for debugging (stdio USB)
- `convert` (ImageMagick) for PBM viewing
- cps2_digiav Verilog as reference
- Spreadsheet for timing calculations

---

## Version History

| Version | Date       | Changes                                           |
| ------- | ---------- | ------------------------------------------------- |
| 0.1     | 2025-10-13 | Initial frequency counter                         |
| 0.2     | 2025-10-13 | Added CSYNC monitoring                            |
| 0.3     | 2025-10-13 | Frame parameter validation                        |
| 0.4     | 2025-10-13 | R4 pixel capture working                          |
| 0.5     | 2025-10-13 | Basic frame PBM output                            |
| 0.6     | 2025-10-14 | CSYNC research & implementation plan              |
| 0.7     | 2025-10-14 | DMA-based full frame capture                      |
| 0.8     | 2025-10-14 | Hardware IRQ sync - zero jitter!                  |
| 0.9     | 2025-11-28 | Documentation review & cleanup (current)          |
| 1.0     | TBD        | 3-bit RGB color capture (next milestone)          |
| 1.1     | TBD        | Full 15-bit RGB capture (goal)                    |

---

**Status Summary**: 🟢 Major breakthrough achieved! Hardware IRQ synchronization provides deterministic, jitter-free capture. Ready for RGB expansion.

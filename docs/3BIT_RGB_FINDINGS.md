# 3-Bit RGB Capture Investigation - Key Findings

## Final Solution ✅
- **4-bit R+G+B+GND capture**: ✅ Perfect alignment, crystal clear output
- **Reason**: 32-bit autopush ÷ 4 bits/pixel = 8 pixels (exact alignment)
- **Key Insight**: GPIO 5 tied to ground provides perfect dummy bit for alignment!

## Why 3-Bit Failed
- **Status**: ❌ Consistent glitching/row-wise distortion
- **Root Cause**: PIO autopush boundary mismatch
  - 32-bit autopush ÷ 3 bits/pixel = 10.67 pixels (NOT aligned)
  - Causes data to span word boundaries unpredictably
  - **Unfixable without architectural changes**

## Issues Found & Fixed
1. **PIO shift direction**: Was RIGHT (LSB), needed LEFT (MSB) for proper bit packing
2. **Capture offset**: Different offsets for 2-bit vs 3-bit (line calculation bug)
3. **Horizontal offset**: First non-zero data at pixel 96 of line 24
4. **Extraction logic**: Must account for bits-per-pixel in all offset calculations

## 4-Bit Solution Details
- **Captures**: R4 (GPIO 2), G4 (GPIO 3), B4 (GPIO 4), GND (GPIO 5)
- **PIO program**: `in pins, 4` with autopush every 32 bits
- **Post-processing**: Extract 4 bits, mask to 3 bits (discard GND), output as RGB
- **Output format**: PPM (P3) with 8 colors (3-bit RGB)
- **Result**: Perfectly sharp, no artifacts, deterministic frame capture

## Alignment Mathematics
| Bits/Pixel | 32÷Bits | Pixels | Status |
| --- | --- | --- | --- |
| 1-bit | 32÷1 | 32.00 | ✅ Would work |
| 2-bit | 32÷2 | 16.00 | ✅ Works (proven) |
| 3-bit | 32÷3 | 10.67 | ❌ Broken |
| 4-bit | 32÷4 | 8.00 | ✅ Works (proven) |
| 5-bit | 32÷5 | 6.40 | ❌ Broken |
| 8-bit | 32÷8 | 4.00 | ✅ Would work |

## Conclusion
- **3-bit capture mathematically incompatible** with 32-bit PIO autopush
- **4-bit solution elegant and robust** - uses existing unused GPIO (5 = GND)
- **Recommendation**: Use 4-bit R+G+B capture for production (Phase 5 complete!)
- **Next steps**: Full 15-bit RGB (would need multiple captures), or streaming optimization

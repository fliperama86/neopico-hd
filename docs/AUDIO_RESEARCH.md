# Audio Implementation Research

## Summary

**PicoDVI DOES support HDMI audio** - via the PicoDVI-N64 fork in `reference/PicoDVI-N64/`. This makes HDMI audio output feasible for NeoPico-HD.

## Reference Projects

### 1. PicoDVI-N64 (reference/PicoDVI-N64/)
The N64 HDMI mod project has full HDMI audio support added to PicoDVI.

**Key Audio Files:**
- `software/libdvi/audio_ring.c/h` - Ring buffer for audio samples
- `software/libdvi/data_packet.c/h` - HDMI data island encoding
- `software/libdvi/dvi.h` - Audio API functions

**Audio API:**
```c
void dvi_set_audio_freq(struct dvi_inst *inst, int audio_freq, int cts, int n);
void dvi_audio_sample_dma_set_chan(struct dvi_inst *inst, int chan, audio_sample_t *buf, ...);
```

**Supported Sample Rates:** 32kHz, 44.1kHz, 48kHz, 96kHz

**How N64 Captures Audio:**
- N64 outputs digital I2S audio (LRCLK, SDAT, BCLK)
- PIO state machine captures I2S data bit-by-bit
- DMA continuously fills a ring buffer
- DVI code reads from ring buffer during blanking

### 2. cps2_digiav (reference/cps2_digiav/)
The CPS2/Neo Geo digital AV project uses a different approach.

**Hardware Solution:**
- **WM8782** - Dedicated stereo ADC chip (24-bit, 192kHz capable)
- **TLV70033** - Clean 3.3V LDO regulator
- Heavy filtering: 10µF + 0.1µF on all power rails
- RC low-pass filters on analog inputs (5k + 10µF)
- Outputs clean I2S to FPGA

**Why External ADC:**
- Much lower noise than MCU internal ADC
- Dedicated analog power supply filtering
- Professional-grade signal conditioning

## MVS Audio Challenges

### Current Approach (Internal ADC)
- Using RP2350B internal ADC on GPIO 40
- Bias circuit: 10k/10k voltage divider + 10µF coupling cap
- Software low-pass filter in Python

**Issues Found:**
1. Pico 3.3V rail is very noisy (switching regulator)
2. Internal ADC has limited SNR (~10-bit effective)
3. No hardware anti-aliasing filter

### Recommended Solutions

#### Option A: External ADC (Best Quality)
Use a dedicated audio ADC like:
- **PCM1808** - 24-bit stereo ADC, I2S output (~$3)
- **WM8782** - Same as cps2_digiav uses
- **CS5343** - Low-cost 24-bit ADC

Wiring:
```
MVS L Audio → ADC AINL
MVS R Audio → ADC AINR
MVS GND     → ADC GND
ADC I2S     → Pico PIO (3 pins: LRCLK, SDAT, BCLK)
```

#### Option B: Improved Internal ADC
If sticking with internal ADC:
1. **Use battery/external LDO** for bias circuit (not Pico 3.3V)
2. **Add RC low-pass filter** before ADC: 1k resistor + 10nF cap
3. **Hardware anti-aliasing**: 2nd order RC filter ~20kHz cutoff
4. **Better shielding** of analog traces

#### Option C: PWM DAC Trick
- Capture at higher rate, downsample in software
- Use oversampling + decimation for better SNR

## Integration with PicoDVI

To add HDMI audio to NeoPico-HD:

1. **Replace lib/PicoDVI** with PicoDVI-N64 fork
2. **Add audio capture** (ADC DMA or I2S PIO)
3. **Feed audio ring buffer** to DVI during VBLANK
4. **Set CTS/N values** for audio clock regeneration

Example from N64 main.c:
```c
// Setup audio
dvi_get_blank_settings(&dvi0)->top = 0;
dvi_get_blank_settings(&dvi0)->bottom = 0;
dvi_set_audio_freq(&dvi0, 48000, 25200, 6144);  // 48kHz

// Feed audio samples via DMA
dvi_audio_sample_dma_set_chan(&dvi0, dma_chan, audio_buffer, 0, 0, AUDIO_BUFFER_SIZE);
```

## Hardware Considerations

### MVS Audio Specs
- Native sample rate: ~55.5kHz (YM2610)
- Output: Analog stereo (AC-coupled)
- Typical level: ~1-2Vpp

### Recommended Capture Settings
- Sample rate: 48kHz (standard HDMI)
- Resolution: 16-bit
- Channels: 2 (stereo)

## Next Steps

1. **Short-term**: Continue testing with internal ADC, optimize filtering
2. **Medium-term**: Order PCM1808 breakout, test I2S capture via PIO
3. **Long-term**: Design PCB with proper audio ADC section

## Online Research Findings

### Pico ADC Noise Sources
The RP2040/RP2350 ADC is negatively affected by the onboard SMPS (switching power supply):
- **Regular mode**: ~60 units of noise
- **PWM mode** (GPIO23 high): ~20 units of noise
- **External ADC_VREF + PWM mode**: ~10 units of noise

**Mitigation options:**
1. Drive GPIO23 high to force PWM mode (reduces SMPS ripple)
2. Use LM4040 3V shunt voltage reference on ADC_VREF
3. Bypass SMPS entirely with clean external power

The RP2350 claims improved ADC, but the SMPS noise issue likely persists.

### PCM1808 I2S Requirements
The PCM1808 requires an **external master clock** (MCLK):
- 12.288MHz for 48kHz sample rate (256×Fs)
- 22.5792MHz or 24.576MHz for higher rates

**Configuration for Pico:**
- Set PCM1808 as master (MD1:MD0 = 0b11, tied to 3.3V)
- PCM1808 generates BCK and LRCK from MCLK
- Pico PIO captures I2S in slave mode

A full I2S PIO implementation exists: [rp2040_i2s_example](https://github.com/malacalypse/rp2040_i2s_example)

### NeoGeoHDMI Approach
The [NeoGeoHDMI project](https://github.com/charcole/NeoGeoHDMI) taps digital signals before the DACs for true digital-to-digital HDMI. However:
- The YM2610 SSG (Simple Sound Generator) outputs **analog** after an internal DAC
- This analog SSG output would still need an ADC to capture
- An FPGA recreation of SSG was considered but lacks documentation

This confirms that MVS audio capture requires handling analog signals.

## References

- [PicoDVI HDMI Audio Issue](https://github.com/Wren6991/PicoDVI/issues/5)
- [Raspberry Pi Forums - PicoDVI Audio](https://forums.raspberrypi.com/viewtopic.php?t=366899)
- [WM8782 Datasheet](https://www.mouser.com/datasheet/2/76/WM8782_v4.1-1141461.pdf)
- [PCM1808 Datasheet](https://www.ti.com/lit/ds/symlink/pcm1808.pdf)
- [Pico ADC Characterized](https://hackaday.com/2021/03/15/raspberry-pi-pico-adc-characterized/)
- [RP2040 I2S PIO Example](https://github.com/malacalypse/rp2040_i2s_example)
- [NeoGeoHDMI Project](https://github.com/charcole/NeoGeoHDMI)
- [PCM1808 on Pico Discussion](https://github.com/earlephilhower/arduino-pico/discussions/1897)

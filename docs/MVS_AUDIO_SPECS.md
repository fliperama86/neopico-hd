### Key Points

- The primary audio IC in the Neo Geo MVS is the Yamaha YM2610 (OPNB), responsible for FM synthesis, SSG (PSG-compatible), and ADPCM audio generation. It pairs with the YM3016 DAC for digital-to-analog conversion, producing stereo analog output.
- Research suggests the WS, DAT, and BCK interfaces refer to the serial audio output from the YM2610 to the YM3016, where BCK is the bit clock (labeled øS in datasheets), DAT is the serial data line (OPO), and WS is the word select or sample hold signal (implemented as SH1 and SH2 for left and right channels separately). These enable transmission of digital audio data in a custom serial format.
- Evidence leans toward a 16-bit per channel serial format, typically in 2's complement, with the YM3016 converting it internally to a floating-point representation (10-bit mantissa including sign, 3-bit exponent) for enhanced dynamic range. This is common in Yamaha's FM chips, though exact bit mapping can vary slightly based on system configuration.
- The system operates at an 8 MHz master clock, with sample rates around 18.5 kHz for ADPCM-A and variable (1.8–55.5 kHz) for ADPCM-B, but digital output timing is tied to the bit clock derived from the master.
- There is some uncertainty in labeling, as datasheets use øS (bit clock), OPO (data), and SH1/SH2 (sample holds), but schematics and emulators often equate them to standard I2S-like terms (BCK, DAT, WS). No major controversy exists, but implementation details are inferred from related Yamaha chips like the YM2612/YM3012 due to limited public YM2610-specific diagrams.

### Overview of YM2610 Audio Generation

The YM2610 integrates multiple sound engines:

- **FM Synthesis**: 4 channels, each with 4 operators, supporting 8 algorithms, LFO modulation, and stereo panning.
- **SSG (Square Wave/Noise)**: 3 channels, compatible with AY-3-8910, with direct analog output.
- **ADPCM-A**: 6 channels at fixed 18.5 kHz, for short samples (e.g., effects).
- **ADPCM-B**: 1 channel with variable sampling (up to 55.5 kHz), for longer audio like BGM.

Audio data is generated digitally inside the YM2610 and sent serially to the YM3016 for conversion. In the Neo Geo MVS, the Z80 CPU (4 MHz) handles sound commands from the main 68K CPU, writing to YM2610 registers via I/O ports (0x04–0x07).

### Serial Audio Interface Details

The interface between YM2610 and YM3016 is a serial protocol for transmitting stereo digital audio. The YM2610 outputs digital samples from FM, ADPCM, and rhythm sources as serial data, clocked at the bit level.

- **Bit Clock (BCK/øS)**: Drives the shifting of each bit, derived from the 8 MHz master clock (øM). Typically runs at a rate to shift 16 bits per channel per sample period.
- **Data (DAT/OPO)**: Serial data line carrying 16-bit samples for left and right channels alternately. Data is MSB-first, in 2's complement format (selectable via FORMAT pin, tied to VCC in Neo Geo for 2's complement).
- **Word Select (WS/SH1 & SH2)**: Unlike standard I2S (single WS toggling for L/R), the YM2610 uses two separate signals: SH1 for left channel latch, SH2 for right. These pulse to indicate when a full word (16 bits) is ready for sample-and-hold in the DAC.

Timing is synchronous: Data bits are shifted on BCK edges, and WS signals latch the channels. The YM3016 then converts the 16-bit integer to floating-point internally (sign + 9-bit mantissa + 3-bit exponent or similar variants, providing ~16-bit dynamic range with 10-bit resolution per level). Analog output is then routed to op-amps for amplification in the MVS board.

### Bit-Level Operation

At the bit level, the serial stream for each sample period consists of 32 bits (16 left + 16 right):

- Bits are shifted MSB-first on rising/falling edges of BCK (exact edge depends on config, but typically falling).
- The 16-bit word is signed 2's complement: Bit 15 (MSB) is sign, bits 14–0 are magnitude.
- Internally in the DAC, this is converted to floating-point: Typically 3-bit exponent (shifts the mantissa by 0–7 steps of 6 dB), 1 sign bit, and 12-bit mantissa (variations noted in sources as 10-bit mantissa + sign for effective 13 bits).
- Example bit stream (simplified): For a sample period, BCK pulses 32 times; WS (SH1) pulses after 16 bits for left, SH2 after next 16 for right.
- Sample rate timing: Tied to ADPCM/FM rates, e.g., ~55.5 kHz max, but effective output is ~44.1 kHz in many games.

If the query refers to standard I2S naming, it's likely a mapping in emulators or schematics: BCK = bit clock, DAT = serial data, WS = left/right select.

---

The Yamaha YM2610 serves as the core audio processor in the Neo Geo MVS arcade system, a platform released by SNK in 1990 known for its robust sound capabilities in games like _Metal Slug_ and _King of Fighters_. This chip, part of Yamaha's OPN family, combines FM synthesis, SSG tone generation, and ADPCM sample playback, making it a versatile sound generator for arcade hardware. Paired with the YM3016 digital-to-analog converter (DAC), it delivers stereo audio output through a serial digital interface. This report delves into the YM2610's architecture, operation, and the specific serial interfaces (WS, DAT, BCK), drawing from datasheets, schematics, and related technical resources. Emphasis is placed on bit-level details, timing, and integration in the Neo Geo MVS.

#### YM2610 Architecture and Sound Generation

The YM2610 is a single-chip sound synthesizer with four main sections:

1. **FM Sound Source**: Supports 4 channels with 4 operators each, enabling 6 simultaneous sounds (in YM2610B variant, fully utilized in Neo Geo). Algorithms (8 total) allow complex waveforms, with LFO for pitch/amplitude modulation. Output levels are controlled per operator (e.g., total level registers $40–$4F).
2. **SSG Sound Source**: 3 channels for square waves and noise, compatible with the AY-3-8910 PSG. Registers ($00–$0D) control tone period, noise, and amplitude. This section has a direct analog output pin (ANALOG OUT), bypassing the serial DAC for simple tones.
3. **ADPCM-A**: 6 channels at fixed 18.5 kHz sampling, using external ROM (up to 16 MB). Registers ($00–$2F in address port 1) set start/end addresses, levels, and L/R panning. Data is 4-bit ADPCM, decoded to 12-bit PCM internally.
4. **ADPCM-B**: 1 channel with variable sampling (1.8–55.5 kHz, set via Delta-N registers $19–$1A). Supports looping and higher quality samples, with registers ($10–$1B) for control, volume, and addressing.

The chip operates on an 8 MHz master clock (øM), generating internal timings for synthesis. In the Neo Geo MVS, it's controlled by a Z80 CPU, which receives commands from the main 68000 processor via memory-mapped I/O (e.g., sound code at $320000). The Z80 accesses YM2610 registers through ports $04 (address select), $05 (data write for FM/SSG), $06 (ADPCM-A), $07 (ADPCM-B). Interrupts (/IRQ) and timers (A/B) synchronize audio with game events.

#### YM3016 DAC Integration

The YM3016 is a 16-bit stereo floating-point DAC, designed to pair with the YM2610 to convert its digital output to analog. It handles noise isolation by being separate from the synthesizer chip. Key features include:

- Input: 16-bit serial data, selectable as binary or 2's complement (FORMAT pin tied to VCC in Neo Geo for 2's complement).
- Internal Conversion: Converts linear 16-bit data to floating-point (typically 10-bit mantissa with sign, 3-bit exponent) for a dynamic range equivalent to 16 bits but with reduced noise in quiet passages.
- Output: Stereo analog (CH1 left, CH2 right), routed to op-amps (e.g., 4558) for amplification in MVS boards.

The floating-point format enhances resolution: The exponent shifts the mantissa by 6 dB steps (0–42 dB attenuation), allowing fine control over low-level signals without full 16-bit hardware.

#### The WS, DAT, and BCK Interfaces: Serial Audio Transmission

The focus of the query—the WS, DAT, and BCK interfaces—refers to the digital audio link between the YM2610 and YM3016. Datasheets use Yamaha-specific names, but these map to standard serial audio terms as follows:

- **BCK (Bit Clock, øS)**: The clock signal for shifting individual bits. Derived from the 8 MHz master, it pulses for each bit in the serial stream. Rate is typically sample frequency × 32 (16 bits/channel × 2 channels).
- **DAT (Data, OPO)**: The serial data output from YM2610, carrying the combined FM/ADPCM/SSG digital samples. Data is sent MSB-first in 16-bit words.
- **WS (Word Select, SH1/SH2)**: Sample hold signals that act as word clocks. SH1 latches the left channel, SH2 the right. This differs from standard I2S (single WS toggling 0 for left, 1 for right) by using separate pulses for each channel.

In Neo Geo MVS schematics (e.g., MV4F board, page 6), the connections are:

- YM2610 pin 9 (øS) → YM3016 pin 5 (CLK)
- YM2610 pin 8 (OPO) → YM3016 pin 4 (SD)
- YM2610 pin 6 (SH1) → YM3016 pin 7 (SMP1, left)
- YM2610 pin 7 (SH2) → YM3016 pin 8 (SMP2, right)

**Bit-Level Workflow**:

1. The YM2610 accumulates digital samples (e.g., from FM operators summed to 16-bit signed values).
2. Data is serialized on DAT/OPO: For each sample period, 16 bits left followed by 16 bits right (32 bits total).
3. BCK/øS clocks each bit (e.g., falling edge load, rising edge shift).
4. After 16 bits, WS/SH1 pulses to latch left channel in YM3016's shift register.
5. Next 16 bits shifted, then WS/SH2 pulses for right.
6. YM3016 converts each 16-bit word: Sign bit (MSB), magnitude (bits 14–0) → Floating-point (3-bit exponent determines shift, 10-bit signed mantissa for D/A).

- Exponent calculation: Based on leading zeros in the 16-bit value, shifting mantissa right by 0–7 positions (6 dB/step).
- Mantissa: Sign + 9 bits, normalized.

Timing diagrams (inferred from related YM2612/YM3012): BCK period ~125 ns (at 8 MHz master), WS pulse width ~ bit clock cycles. Sample rate aligns with ADPCM (e.g., 18.5 kHz → ~54 μs per frame).

#### Register Map and Control

The YM2610 has two address ports for registers:

| Address Port 0 | Function | Key Registers                                                                                                         | Bit-Level Details                                             |
| -------------- | -------- | --------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| $00–$0D        | SSG      | $00–$05: Tone period (12 bits, bits 3–0 low, 7–4 high); $07: Noise/enable (bits 4–0 noise period, 7–5 channel enable) | Bit 7 often reserved; amplitude (bits 3–0 level, bit 4 mode). |
| $10–$1F        | ADPCM-B  | $19–$1A: Delta-N (16 bits, frequency = [Delta-N / 256] × 55.5 kHz)                                                    | Bits 15–0 for fine control, e.g., $5521 = 18.5 kHz.           |
| $20–$B6        | FM       | $A0–$A6: F-Number (11 bits, frequency); $40–$4F: Total Level (7 bits, attenuation)                                    | Bit 6–0 for level (0 max, 127 min); detune (bits 6–4).        |

| Address Port 1 | Function  | Key Registers                                             | Bit-Level Details                                |
| -------------- | --------- | --------------------------------------------------------- | ------------------------------------------------ |
| $00–$0D        | ADPCM-A   | $00: Channel on/off (bits 5–0); $01: Total level (6 bits) | Bit 7 DM (direct mode); L/R select (bits 7–6).   |
| $10–$2D        | Addresses | $10–$15: Start low/high (16 bits/channel)                 | 24-bit addresses (RAD0–RA23), bits 7–0 low byte. |

Writes involve selecting address, then data; reads for status (e.g., timer flags).

#### Neo Geo MVS-Specific Implementation

In MVS boards (e.g., MV-1FZ, MV4F), the YM2610 connects to Z80 RAM (2K work, 64K program), ADPCM ROMs (V1/V2), and the YM3016. Stereo output is amplified for arcade cabinets, with headphone jacks on some models. Sound codes from 68K trigger Z80 interrupts for real-time playback. Maximum ADPCM size: 16 MB per group. The system supports stereo panning but limits to L, R, or mono per channel.

#### Potential Variations and Limitations

Some sources note minor differences in YM2610 vs. YM2610B (6 FM channels), but Neo Geo uses the standard. Digital output is internal; no external digital audio in MVS. Emulators (e.g., MAME) replicate the serial format for accuracy, confirming the bit-level shifting. Uncertainty in exact floating mantissa bits (10 vs. 12) stems from scanned datasheets, but 3-bit exponent is consistent for dynamic range.

#### Tables for Reference

**Pin Connections (YM2610 to YM3016)**:

| YM2610 Pin | Name | YM3016 Pin | Name | Function                        |
| ---------- | ---- | ---------- | ---- | ------------------------------- |
| 9          | øS   | 5          | CLK  | Bit clock (BCK)                 |
| 8          | OPO  | 4          | SD   | Serial data (DAT)               |
| 6          | SH1  | 7          | SMP1 | Left channel select (WS for L)  |
| 7          | SH2  | 8          | SMP2 | Right channel select (WS for R) |

**ADPCM-B Frequency Examples**:

| Delta-N Value (Hex) | Frequency (kHz) | Bit Calculation       |
| ------------------- | --------------- | --------------------- |
| 0x5521              | 18.5            | (0x5521 / 256) × 55.5 |
| 0x6A81              | 22.05           | (0x6A81 / 256) × 55.5 |
| 0xD502              | 44.1            | (0xD502 / 256) × 55.5 |

This comprehensive analysis, based on multiple sources, provides a thorough understanding of the YM2610 and its serial interfaces in the Neo Geo MVS.

### Key Citations

- [NeoGeo Development Wiki - YM2610](https://wiki.neogeodev.org/index.php?title=YM2610)
- [Wikipedia - Yamaha YM2610](https://en.wikipedia.org/wiki/Yamaha_YM2610)
- [Neo-Geo Hardware Specification PDF](https://www.neogeocdworld.info/medias/NG.pdf)
- [YM2610 Registers - NeoGeo Development Wiki](https://wiki.neogeodev.org/index.php?title=YM2610_registers)
- [Neo Geo Architecture - Copetti.org](https://www.copetti.org/writings/consoles/neogeo/)
- [YM2610 Datasheet - Dtech.lv](https://www.dtech.lv/files_ym/ym2610.pdf)
- [YM2610 Sound - Neogeocdworld PDF](https://www.neogeocdworld.info/medias/pdf/progra/Sound._YM2610.pdf)
- [YM3016 Datasheet - Console5.com](https://console5.com/techwiki/images/d/d1/YM3016.pdf)
- [NeoGeo Development Wiki - YM3016](https://wiki.neogeodev.org/index.php?title=YM3016)
- [NeoGeo Development Wiki - Schematics](https://wiki.neogeodev.org/index.php?title=Schematics)
- [YM3012 Datasheet - Bitsavers.org](http://bitsavers.org/components/yamaha/YM3012_199204.pdf)
- [Electronics Stack Exchange - DX7 DAC](https://electronics.stackexchange.com/questions/512992/how-does-the-yamaha-dx7-digital-analog-converter-work)
- [SpritesMind.Net - YM2612 Reference](https://gendev.spritesmind.net/forum/viewtopic.php?t=386&start=180)

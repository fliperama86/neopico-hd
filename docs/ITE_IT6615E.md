# Ad-Hoc Documentation for ITE IT6615E HDMI 2.0 Transmitter

**Disclaimer**: This is an unofficial, reverse-engineered compilation based on publicly available sources, primarily open-source driver code from GitHub (zhaodehua1988/AoIP repository), the official ITE product page, and patterns from similar ITE chips (e.g., IT66121). No official datasheet is publicly available—ITE restricts full documentation to customers/partners. This document aims to be as comprehensive as possible for hobbyist or development use. Accuracy is not guaranteed; test thoroughly. For production, contact ITE Tech directly.

## Overview
The **IT6615E** (128-pin LQFP package) is a single-port HDMI 2.0b transmitter from ITE Tech. Inc., supporting:
- Up to 6 Gbps per TMDS channel (4K@60Hz 4:4:4 or 4K@60Hz 4:2:0 with BT.2020)
- HDMI 2.0b, 1.4b; HDCP 2.2, 1.4; DVI 1.0 backward compatible
- 36-bit Deep Color (12-bit/component)
- Video inputs: TTL parallel RGB/YCbCr (24/30/36-bit 4:4:4, 16-24-bit 4:2:2, 24-36-bit 4:2:0), single/dual pixel, DDR, sync/DE modes, BTA-T1004
- LVDS input support (with integrated RX)
- Advanced audio: Up to 8-channel I²S, S/PDIF, HBR, 3D audio, DTS-HD/Dolby TrueHD
- HDR (InfoFrame), extensive 3D formats (including Dual/Independent View)
- Integrated HDCP keys (in IT6615E variant), hardware CEC PHY
- Bi-directional color space conversion (RGB ↔ YCbCr)

Variants: IT6615E (with pre-programmed HDCP keys), IT66159E (without).

## Package and Pinout
- **Package**: 128-pin LQFP (14x14 mm body)
- **Pinout**: Not publicly available. Similar ITE HDMI transmitters (e.g., IT66121) have TMDS outputs (3 data + clock pairs), parallel video input bus (24-36 bits), clocks (PCLK, etc.), I²C (SDA/SCL), CEC, HPD, power rails (1.1V core, 3.3V I/O), and reset pins. Expect similar layout with added pins for HDMI 2.0 features (e.g., SCDC support via DDC). Reverse from evaluation boards or contact ITE.

## Control Interface
- **I²C Slave Addresses** (7-bit, write/read):
  - HDMI TX main: 0x4C (write 0x98, read 0x99)
  - LVDS RX (if used): 0x16 (write 0x2C)
  - EDID DDC: 0x50 (0xA0/0xA1, standard)
  - CEC: 0x4E (0x9C)
- **Register Banks**: Multi-bank access. Write password to Reg 0xFF to switch banks:
  - 0xC3 or 0xA5 (common passwords observed)
  - Bank 0: Default/main registers
  - Bank 1: Timer/RCLK
  - Bank 2: PLL/AFE advanced
- **Access Functions** (from driver code patterns):
  - Write: hdmitxwr(reg, value)
  - Read: hdmitxrd(reg)
  - Set bits: hdmitxset(reg, mask, value)
  - Burst via DDC FIFO for EDID/SCDC/HDCP

## Power Management
### Power-On Sequence (`iTE6615_SetTX_Power(POWER_ON)` pattern)
1. Reg0x0C = 0x00 (disable IDDQ mode)
2. Reg0xFB = ADDR_LVDSRX | 0x00 (LVDS RX control)
3. Reg0x25 = 0x00 (enable GRCLK)
4. Reg0x25 = 0x00 (enable ICLK/IACLK/TXCLK)
5. Reg0x88 = 0x00 (DRV power on)
6. Reg0x84 = 0x00 (XPLL on)
7. Reg0x81 = 0x00 (IPLL on)
8. Reg0x80 = 0x80 (release IP_RESETB)
9. Reg0x84 = 0x80 (release XP_RESETB)
10. Reg0xFF = 0xFF (disable password protection if needed)

### Power-Off Sequence
Reverse the above, setting power-down bits (e.g., Reg0x0C=0x01 for IDDQ).

## Chip Reset and Initial Configuration (`iTE6615_SetTX_Reset` pattern)
Typical full reset/init:
- Reg0x05/0x06: Soft resets
- Reg0x80/0x84: PLL resets
- Reg0x35: DDC master reset
- Reg0xFF: Password 0xC3/A5
- Reg0x0C: Force Rx sense
- Reg0x20: Manual PR/lock
- Reg0x34: 0x80
- Reg0x90-0x94: Input format (color depth, bus, sync)
- Interrupt enables (Reg0x19,0xC0, etc.)
- AFE/drive settings (see below)

## Video Configuration
- **Input Settings** (Reg0x90-0x96 typical):
  - Color depth, RGB/YUV, sync mode, DDR/half-bus, swaps
- **Output Settings**:
  - Color depth: 8/10/12-bit (Reg related to 0xDB etc.)
  - CSC Matrix: Write coefficients to 0xA4-0xB9
- **AFE/Drive Settings** (`iTE6615_SetTX_AFE`):
  - Based on pixel clock (VCLK):
    - Reg0x81/0x84/0x88: Speed/select
    - Reg0x87-0x8D: Current, termination, pre-emphasis
    - Bank-2 PLL: 0x40,0x44-0x47
- **AV Mute**: Reg0xC1 bit 0
- **Timestamp**: Reg0x09 bit 7

## Audio Configuration (`iTE6615_SetTX_AudioFormat/Option`)
- Interface: I²S or S/PDIF (Reg0xDB, 0xD8)
- Channels: Up to 8 (Reg0xE2 for TDMA)
- Format: Word length (Reg0xDA), N/CTS values (Reg0x33-0x35)
- Channel Status/InfoFrame: Reg0x68-0x6D (AVI), 0xF0-0xF4
- 3D/MS Audio: Reg0xC8-0xDD
- Clock/Filter: Reg0xC4,0xC5,0xE3-0xE4

## InfoFrames
- AVI: Reg0x68-0x6D
- Audio: Similar
- DRM/HDR: Configurable
- 3D: Enable via flags

## HDCP Support
### HDCP 1.x
- Reg0x40-0x4F: Keys/An
- Reg0x41: Desired/enable
- Reg0x42: Fire auth
### HDCP 2.x (Repeater capable)
- Reg0x42 = 0x10 (select 2.x)
- Reg0x6B: Repeater options
- Stream manage/EKS: Specific bits in 0x6B

## HDMI 2.0 Features (SCDC)
- SCDC Read/Write via burst DDC (Regs 0x28-0x30)
- RR Enable: Write to offset 0x30

## EDID/DDC Handling
- Burst read via Reg0x28-0x30 (PC-DDC mode)
- FIFO clear: Reg0x2E=0x09

## CEC
- Optional, I²C addr 0x9C
- Hardware PHY support

## Known Key Registers (Partial Map from Driver)
| Reg (Hex) | Purpose (Inferred) |
|-----------|-------------------|
| 0x05/06   | Soft resets |
| 0x09      | Timestamp enable |
| 0x0C      | IDDQ/Force Rx |
| 0x19/1D   | Interrupt enables |
| 0x20-0x26 | Clock/power control |
| 0x28-0x35 | DDC/SCDC master/FIFO |
| 0x40-0x57 | HDCP 1.x keys/control |
| 0x68-0x6D | AVI InfoFrame |
| 0x80-0x8D | PLL/AFE/drive |
| 0x90-0x96 | Input format |
| 0xA4-0xB9 | CSC matrix |
| 0xC1      | AV Mute |
| 0xDA-0xDE | Audio format |
| 0xFB      | LVDS RX addr enable |
| 0xFF      | Bank password |

Bank 1: Timer (0x14-0x15)
Bank 2: Advanced PLL (0x40+)

## Configuration Options (Common #defines)
Many driver configs for input/output color space (RGB/YUV444/420), depth (8-12 bit), audio (I²S/SPDIF, channels, freq), HDCP, 3D, etc. See driver Config.h for defaults.

## Recommendations
- Start with power-on → reset → input config → video/audio → HDCP if needed.
- Monitor interrupts for HPD, auth status.
- For 4K@60: Ensure AFE tuned, YUV420 if needed, SCDC RR if sink requires.
- Test with known-good sink (TV/monitor).

This covers ~80-90% of functionality based on available code. For missing details (e.g., exact video timing setup, full pinout), probing I²C on a working board or contacting ITE is required. If you provide more specifics (e.g., your input format, target resolution), I can suggest a sample init sequence!
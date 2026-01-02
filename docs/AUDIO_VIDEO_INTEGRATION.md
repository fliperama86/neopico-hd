# NeoPico-HD Integration Status

## System Resource Allocation (Final)

| Peripheral | Instance | Allocation                                    |
| ---------- | -------- | --------------------------------------------- |
| PIO0       | Bank 1   | DVI / HDMI Output (3 State Machines)          |
| PIO1       | Bank 0   | MVS Video Capture (Sync + Pixel Capture)      |
| PIO2       | Bank 0   | I2S Audio Capture (1 State Machine)           |
| DMA        | Various  | DVI Stream, Video Capture (PP), Audio Capture |
| Core 0     | -        | Capture, Processing, OSD, Main Logic          |
| Core 1     | -        | PicoDVI Stack (Scanline Callback)             |

## Performance Milestones

### 1. Zero-Drift Video

By using PIO hardware synchronization for every line and offloading the transfer to DMA with ping-pong buffering, we have achieved a perfectly stable image with 0% CPU overhead for the raw capture itself.

### 2. High-Fidelity Digital Audio

The audio pipeline successfully captures the 55.5kHz stream from the MV1C ASIC.

- **Polling "Kick"**: Resolved a logic loop by ensuring hardware polling happens unconditionally every frame.
- **Clock Unification**: Unified the system at 252MHz to provide the precise timing required for HDMI Audio Data Islands.

### 3. OSD Foundation

Implemented a high-contrast 5x7 pixel font renderer that works directly on the $320\times240$ framebuffer. This is used for real-time diagnostics (LRCK MEAS, PIO PC, DMA ADDR) and forms the basis for the future menu system.

## Resolved Risks

- **PIO Conflicts**: Resolved by using the RP2350's 3rd PIO block.
- **Deadlocks**: Fixed an initialization hang caused by `PIO_FIFO_JOIN_RX` deactivating the TX FIFO during `pio_sm_put`.
- **Sync Loss**: Fixed horizontal scrolling by implementing explicit start-of-frame PIO resets and IRQ triggers.
- **Audio Overflows**: Solved by implementing high-frequency loop draining in `main.c`.

## Future Roadmap

- [ ] Implement full OSD menu system for runtime settings.
- [ ] Add SHADOW/DARK signal processing for authentic MVS brightness effects.
- [ ] Add button support for live audio filter toggling.

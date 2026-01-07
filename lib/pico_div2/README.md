# PicoDVI²

An HSTX-native DVI/HDMI output library for the RP2350 (Raspberry Pi Pico 2).

Unlike the original PicoDVI for RP2040 which required CPU-intensive PIO bit-banging, PicoDVI² leverages the RP2350's dedicated **HSTX (High-Speed Transmit)** peripheral with hardware TMDS encoding. This means near-zero CPU overhead for video output.

## Overview

`pico_div2` provides a high-performance video and audio output pipeline using the RP2350's HSTX hardware. It is designed to be decoupled from specific application logic, focusing strictly on the generation of stable TMDS signals and Data Island injection (e.g., for audio).

## Key Features

- **HSTX Hardware TMDS Encoding**: Uses the native TMDS encoder for zero-CPU video serialization.
- **Audio Data Islands**: Built-in support for TERC4 encoding and scheduled injection of audio samples.
- **Data Island Queue**: Lock-free queue for asynchronous packet posting from other cores.
- **Double-Buffered DMA**: Stable video output with minimal jitter.

## Directory Structure

- `include/pico_div2/`: Public headers. Use `#include <pico_div2/...>` in your project.
- `src/`: Implementation files.
- `CMakeLists.txt`: Build configuration.

## Usage

1. Add this directory to your project's `lib` folder.
2. Add `add_subdirectory(path/to/pico_div2)` to your `CMakeLists.txt`.
3. Link against `pico_div2`.
4. Initialize with `video_output_init()` and run the output loop on Core 1 with `video_output_core1_run()`.

## License

Unlicense

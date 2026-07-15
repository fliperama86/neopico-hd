# Host tests

## Exhaustive MVS color characterization

Run:

```sh
bash tests/run_mvs_color_exhaustive.sh
```

The test compiles with the host C compiler and writes its temporary binary
outside the repository. It does not build firmware or access Pico hardware.

It executes all 32,768 captured RGB555 values in all four DARK/SHADOW flag
combinations. Structural conversion errors fail the test. Known color-accuracy
differences are printed as characterization results so they can be corrected
and converted into strict expectations one change at a time.

The same run exhaustively checks both normal-color menu choices. `Digital`
must match the stable RGB555-to-RGB565 LUT for every source color. `Analog`
must match the independent normal-state DAC reference for every source color.
The normal-color API has no DARK/SHADOW input.

The runner also checks the live menu source contract: moving the selection
requests a preview, SELECT requests the committed model, START queues
persistence, both 64 KiB LUTs exist, and the Colors case contains no
save-and-reboot call.

The runner builds the production split-table code twice, once for each
compile-time model. Independent, test-only reference models are pinned to the
MiSTer Neo Geo implementation and MAME's Neo Geo resistor-network model. Every
production channel entry, split-table result, and synthesized raw capture word
must match its selected independent reference. MAME's resistor values are a
model, not a substitute for measurements from the target MV1C board under the
intended load.

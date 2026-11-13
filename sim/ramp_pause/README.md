# RAMP pause test

A pause must not change what the core draws. This test feeds the real `rtl/vectrex.vhd` (VIA, delay lines, analogue
frontend) one fixed VIA stream that draws a single vertical vector, holds `pause` for a while at a chosen point, and
compares the end point and the RAMP and beam tick counts (counted in emulated time) with a run without a pause.

Needs GHDL 5 (`--std=08 -frelaxed -fsynopsys`). The CPU, PSG, high scores, LFSR and cart EEPROM are stubbed
(`stubs.vhd`): only the 6809's E-phase enables are modelled, exactly as `rtl/mc6809.v` makes them.

## Versions

`make_variants.py` writes the comparison versions from the current `rtl/vectrex.vhd`:

| version       | what it is |
|---------------|------------|
| `cur`         | `rtl/vectrex.vhd` as it is |
| `before`      | the core before the fix, rebuilt byte for byte (13 Nov 2026) |
| `before_free` | `before` with `cpu_phase` running on during a pause, as in normal 6809 mode |
| `free`        | the fix for the 24 MHz paths only, with `cpu_phase` running on during a pause |

The stream comes in through the PiTrex port, but timed by the CPU's E enables just as a real 6809's bus cycles are, so
the `_free` versions show normal 6809 mode with the same stream. The fixed core freezes the phase in both modes, so
`cur` covers both.

## Running

    sim/ramp_pause/sweep.sh                  # all four versions, 14 pause lengths x 40 start points x 2 (about 27 min)
    sim/ramp_pause/sweep.sh cur              # one version
    LENS="3 5" STARTS="0 1 2" sim/ramp_pause/sweep.sh cur free     # a quick check
    sim/ramp_pause/run.sh                    # the first test: one 480-clock pause, start swept over 260 clocks

Pause lengths include short and odd ones (1, 2, 3, 5, 7, 15, 17, 31, 33...): a length that is a multiple of 4 clocks
lets a running `cpu_phase` come back to where it was, so it can't show a phase problem. Don't edit `rtl/` while a sweep
runs: GHDL (mcode) re-checks the sources at every run.

## Results (13 Nov 2026)

Reference (no pause): y=18176, ramp_ticks=284, lit_ticks=272. 1120 pauses per version:

| version       | runs that differ | largest Y error |
|---------------|------------------|-----------------|
| `before`      | 624              | +6528 / -7296 (35.9% / 40.1%) |
| `before_free` | 757              | as `before`, from a 1-clock pause up |
| `free`        | 323              | one or two 12 MHz ticks (64, 128), at lengths that aren't a multiple of 4 |
| `cur`         | 0                | none |

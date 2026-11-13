# Copilot instructions: Vectrex-PiTrex (UVMC2 and PiTrex on the MiSTer)

This branch adds PiTrex and UVMC2 support to the MiSTer Vectrex core. The ARM side lives in `pitrex/`:
`um2run/` runs UVMC2 cartridge images (`.um2`) on the DE10-Nano's Cortex-A9 under Linux, `src/` is the PiTrex stream layer,
`scripts/` holds the MiSTer install, launcher and card scripts. `um2run/DESIGN.md` explains how um2run works.

## Rules
- Base your work on the branch named in the task (usually `pitrex-speed`). Keep each pull request to the task's scope.
- **Never change what a game or PiTrex program writes on its way to the core** (VIA/PSG writes, zeroing and the zero reference,
  ramp/blank timing, the order or timing of the command stream), and never work around how the core draws. Those changes need
  the core maintainer's review first: if you think one is needed, describe it in the pull request instead of making it.
- Fix root causes. Don't hide a symptom with a filter, a special case for one game, or a changed default.
- No cartridge images, ROMs, firmware, boot ROMs, card images or sample sets in the repository.
- Keep the existing style: C99, tabs or spaces as the file already uses, its existing line endings, comments that explain why.
  Shell scripts must run under BusyBox ash as well as bash (the MiSTer's /bin/sh).
- Every behaviour change comes with a test that runs on a Linux PC (a script under `pitrex/tests/` or a self test), and the pull
  request says how to run it and what it showed.
- Docs: plain English, short sentences. Update the relevant `.md` when behaviour changes.

## Building and testing on a PC
- um2run for the MiSTer: `cd pitrex/um2run && ./build.sh` (needs `gcc-arm-linux-gnueabihf`).
- Host build of the device model: the last line of `build.sh` (`um2bus_replay`).
- Hardware testing on the MiSTer is done by the maintainers after review; say in the PR what should be checked there.

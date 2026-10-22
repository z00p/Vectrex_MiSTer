# UVMC2 mode

The [UVMC2](https://github.com/tullulah/uvmc2-sdk) ("Ultimate Vectrex Multicart 2") is an RP2350
cartridge whose games run on the cartridge's own processor and drive the Vectrex directly. UVMC2
mode runs those games on the DE10-Nano's ARM instead, through the same command ring as PiTrex
programs. **The core needs nothing extra.**

Published with the permission of the UVMC2 team, who wrote the runtime, the beam model, libvpy
and the games. Please credit them and link their SDK when you share builds.

## Playing
- **UVMC2 Hello** (main menu > PiTrex & UVMC2): the team's example
  game. Steer with the stick and thrust with a button, with a stick and button readout.
- **Tac/Scan** (Sega 1982), the team's port with sampled sound. It isn't in the release (see
  below), so you build it yourself and supply your own files:
  1. Build it: `tools/build_tacscan.sh` (needs `upstream/uvmc2-starter-kit`).
  2. Copy `build/arm/tacscan` to `/media/fat/pitrex/`.
  3. Put the MAME romset `tacscan.zip` in `/media/fat/pitrex/uvmc2/tacscan/`.
  4. For sound, make `TACSCAN.PCM` from the MAME `tacscan` sample set with the starter kit's
     `game/tacscan/tools/wav_to_pcm.py`, and put it in the same folder. A B6 core plays it
     through its PCM channel at full quality. For older cores, also make `TACSCAN.VSM` with
     `wav_to_vsmp.py`; it plays through the Vectrex's sound chip.
  5. Run Scripts > PiTrex_Setup again to add it to the menu.
  - Controls: 1 fire, 2 grab, 3 coin, 4 start.
  - The continuous ship roar is muted (`UVMC2_MUTE=0,1,2`); the shots and alien sounds play.

Tac/Scan isn't shipped as a binary because its Z80 core, mz80 by Neil Bradley, may not be
redistributed in modified form without the author's approval.

## How it works
A UVMC2 game calls the PiTrex-style contract (`v_directDraw32`, `v_writePSG`, `v_readButtons`,
`v_WaitRecal`, ...). The team's runtime (`uvm2-sdk`) and Rust beam model (`vectrex-draw`) turn
each frame into a list of 3-byte commands: {delay 12 bits, VIA register 4, data 8}. On the
cartridge, a PIO state machine replays that list one write per E period, then parks for `delay`
periods.

On MiSTer:
- **The bus layer:** `src/uvm2_bus_mister.c` replaces the cartridge's bus layer. `uvm2_exec()`
  turns each command into a ring WRITE to `$D000 + reg` followed by `delay` E cycles of stream
  time, which is the same timing, since a ring write takes one E period.
- **Frame boundaries:** after each list it reloads VIA Timer 2, because Videodr0me's renderer
  marks frames on T2 reloads and the UVMC2 runtime never touches T2.
- **The svc traps:** `tools/patch_uvmc2.py` makes Linux copies of three SDK files:
  - the game layer's `svc` traps become direct calls to the runtime's dispatcher;
  - the sample bundle loads from a file into RAM instead of the cartridge's PSRAM.
- **Program entry and stubs:** `src/uvm2_mister_stubs.c` holds the program entry (the game's
  `main` is renamed, as on the cartridge) and stubs for cartridge hardware (LED, PSRAM, audio
  jack, SD card). It also holds the MiSTer calibration:
  - **Beam scale (`DRAW_SCALE`):** per-console calibration on the real cartridge. At the
    cartridge's stock value of 160, the picture is about a third too large on this core, so
    the MiSTer default is 120. Override it in `/media/fat/pitrex/uvmc2.cfg` (`scale=125`) or
    with `UVMC2_SCALE`. On this core, larger means bigger.
  - **Sound:** on a B6 core the cartridge's audio jack is the core's PCM channel (16-bit,
    32 kHz, the jack's own rate). Older cores use the console's PSG and DAC.
    `UVMC2_AUDIO=psg` forces the PSG.
  - **Data files:** read from `$UVMC2_DIR`.

### Building
```
# once: the beam model, in Rust (rustup target add armv7-unknown-linux-gnueabihf)
cd upstream/uvmc2-sdk/vectrex-draw/cabi
cargo build --release --target armv7-unknown-linux-gnueabihf     # CARGO_TARGET_DIR=~/vxdraw-target
# then a game: libvpy + the runtime + our bridge, static ARM binary in build/arm/
tools/build_uvmc2_game.sh hello_uvmc2 upstream/uvmc2-starter-kit/examples/hello_uvmc2/src/main.c
```
`UVM2_HZ=0|50` picks free or fixed refresh. Games with samples want free refresh, because the
samples go into the gaps of the drawing list. `tests/uvmc2_lines.c` is a minimal drawing test
for the PC harness (`tools/build_uvmc2.sh pc ...`); full games are built for the ARM only.

## Credits
- **The UVMC2 team:** the UVMC2 SDK, runtime, beam model, libvpy and the starter kit games.
- **Tac/Scan:** © Sega 1982. Its port is built on AAE sources included in the starter kit.
- **mz80:** "Multi-Z80 CPU emulator by Neil Bradley (neil@synthcom.com)".
- **puff:** © Mark Adler.

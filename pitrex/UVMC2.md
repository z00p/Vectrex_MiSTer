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
- **UVMC2 Playroom**: the team's showcase. A wireframe figure with a 13-bone skeleton walks a
  hub; four doors lead to four rooms, each a small game built round part of the SDK: a crate
  tower to shoot apart, soft bodies, a shape that breaks up, and a flight over hills. Stereo
  sound is mixed to mono for the PCM channel.
- **UVMC2 FX demo**: the team's effects demo. Shoot a wireframe shape and it comes apart from
  where it was hit.
- **Tac/Scan** (Sega 1982), the team's port with sampled sound. It is included pre-built, so
  there is nothing to build; you only supply your own files:
  1. **Romset:** your MAME `tacscan.zip`. The menu entry looks for it in
     `/media/fat/games/mame/` (where most MiSTers already keep MAME sets) and then in
     `/media/fat/pitrex/uvmc2/tacscan/`.
  2. **Sound (optional):** put MAME's `tacscan` samples zip, as downloaded (for example from
     [Arcade@Home's sample page](https://www.arcadeathome.com/mamewav.phtml)), in
     `/media/fat/games/mame/samples/`. PiTrex_Setup, PiTrex_Update or the first start of
     Tac/Scan converts it to `TACSCAN.PCM` (`pitrex/ts_samples.py`, about 20 seconds), and a
     B6 core plays it through its PCM channel at full quality. Without it the game runs silent.
  - Controls: 1 fire, 2 grab, 3 coin, 4 start.
  - The continuous ship roar is muted (`UVMC2_MUTE=0,1,2`); the shots and alien sounds play.

The game's Z80 is Tim Cottrill's AAE Z80 core (see Credits), so the binary can be shipped; no
romset, sample or Sega code is included.

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

Tac/Scan is built with `tools/build_tacscan.sh` (needs `upstream/uvmc2-starter-kit` and
`g++-arm-linux-gnueabihf`); it compiles the Z80 core in `third_party/aae_z80/` and the adapter in
`games/uvmc2_tacscan/z80shim/`. The other programs are built like Hello above; only developers
need any of this.

## Credits
- **The UVMC2 team:** the UVMC2 SDK, runtime, beam model, libvpy and the starter kit games.
- **Daniel Ferrer** (tullulah): UVMC2 and the UVMC2 starter kit; also PureDOOM-pitrex (see
  [games/doom/README.md](games/doom/README.md)).
- **Ralf Kornmann:** the UVMC2 firmware. `um2run` runs cartridge images without it, and none of it is included here.
- **Jason Kopp:** the UVMC2 hardware.
- **Tac/Scan:** © Sega 1982. Its port is built on AAE sources included in the starter kit.
- **Z80 core:** [Tim Cottrill's AAE](https://github.com/tcottrill/AAE) `cpu_z80`, used with his permission
  ([issue 13](https://github.com/tcottrill/AAE/issues/13)); a modified port of **Edward Massey's** 1997
  Z80 emulator (GNU LGPL v2, distributed here under the GPL as the LGPL allows). His readme and
  documentation are in `third_party/aae_z80/`. This replaces the old mz80 core, so the earlier
  licence issue is gone.
- **puff:** © Mark Adler.

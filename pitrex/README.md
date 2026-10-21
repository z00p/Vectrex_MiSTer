# PiTrex mode for the MiSTer Vectrex core

Runs [PiTrex](https://github.com/gtoal/pitrex) software on the DE10-Nano's ARM (HPS), with this
Vectrex core standing in for a real Vectrex. It is the same split as a PiTrex cartridge: the
6809 is held, and a program on an ARM drives the VIA. Here the "bus" is a command ring in DDR3
that the core replays at exact E-clock timing.

Status: working on hardware (October 2026). The self-test, the PiTrex `hello`/`cube` demos and
VecBreak run at full speed through Videodr0me's renderer, with joystick, buttons and sound.

## How it works
- **ARM side** (this folder): a normal Linux program, started over SSH. The PiTrex library is
  compiled unchanged except for:
  - a drop-in replacement for `pitrexio-gpio.c` (`src/pitrexio-mister.c`);
  - about 14 timing functions patched (`tools/patch_lib.py`) so that delays advance *stream
    time* instead of busy-waiting.

  Every bus write becomes a 64-bit command (opcode, delay in E-cycles, address, data) in a ring
  at DDR byte address `0x34000000`, mapped through `/dev/mem` like Main's `shmem_map`. Reads
  never wait on the FPGA:
  - VIA timers and flags come from a shadow VIA on the ARM;
  - buttons and the joystick comparator come from a small input block the core writes back.
- **Core side** (`rtl/pitrex/`, plus small hooks):
  - `pitrex_ddr.sv` (clk_125) is a read/write client on `vfb_ddr_arbiter`, between
    composition and artwork upload. It polls the ring header, fetches 32 commands per
    256-byte burst into an async FIFO, and writes status and input back every 0.5 ms.
  - `pitrex_player.sv` (clk_sys) replays each command on the falling-E enable the 6809 would
    use. When the ring is empty it pauses the whole machine instead of drawing with broken
    timing.
  - `vectrex.vhd`: the 6809 is held in reset in PiTrex mode, and the CPU bus outputs are muxed
    with the player's, so every downstream decode is unchanged.
  - OSD: System > **PiTrex Mode**. Switching it resets the machine; switching it off boots the
    BIOS normally.

The full format is in [PROTOCOL.md](PROTOCOL.md), including the presence handshake (B5):
PiTrex Mode On only makes the core *listen*; the 6809 runs normally until a live program shows
a coded heartbeat, then PiTrex takes over, and when the program exits or dies the core hands
back. The program never writes to DDR until it has confirmed the Vectrex core (`/tmp/CORENAME`)
and the core's live status signature. Builds identify themselves: Core Info shows the PiTrex build, and the self-test
prints the build the core reports in the status block.

## Using it on a MiSTer
**Easiest: the Scripts menu.** Copy the programs and `scripts/pitrex_launch.sh` to
`/media/fat/pitrex/`, the `scripts/PiTrex_*.sh` entries to `/media/fat/Scripts/`, and the core
to `/media/fat/_Unstable/Vectrex_PiTrex_B<n>_<date>.rbf`. Then pick e.g. **PiTrex_VecDrive**
from Scripts. From another core it loads the Vectrex core (switch PiTrex Mode on when it
appears). If the Vectrex core is already running, it just switches programs.
**PiTrex_Stop** ends the program and hands the Vectrex back.

**By hand:**
1. Build the core (Quartus 17.0.2 Lite; fitter seed 5 meets timing at all four corners) or use a
   release build.
2. Load it, then OSD > System > PiTrex Mode = On. The Vectrex keeps running normally until a
   PiTrex program starts.
3. Over SSH (root, for `/dev/mem`): `PTX_BACKEND=mister ./ptx_selftest`. Start the core and
   PiTrex Mode **before** the program; the program resumes from the core's read index.

## Building the ARM programs (Linux or WSL)
```
git clone https://github.com/gtoal/pitrex upstream/pitrex   # the PiTrex library and demos
make                                     # patch the library copy, build hello + ptxplay (PC)
make run                                 # hello -> build/stream.ptxs -> PNG frames
tools/build_prog.sh arm cube upstream/pitrex/hello_world/cube.c     # static ARM binary
```
ARM builds need `gcc-arm-linux-gnueabihf`. `tests/ptx_selftest.c` needs no PiTrex library:
```
arm-linux-gnueabihf-gcc -O2 -static -Isrc -o ptx_selftest tests/ptx_selftest.c src/ptx_*.c
```

## Testing without a MiSTer
The file backend (the default) writes the command stream to a file. `tools/ptxplay` replays it
through a software VIA and a vecx-style analog model and writes PNG frames plus per-frame stats
(cycles per frame, writes, lit cycles):
```
tools/run_stream.sh cube 40 30 1          # name, frames, first frame, count
```
Inputs can be faked with `PTX_POT="x1,y1,x2,y2"`, `PTX_BUTTONS=0xFE` and `PTX_BUTTONS_FROM=<frame>`.
Holding a button from frame 0 opens the PiTrex calibration menu.

## Contents
| Path | What |
|---|---|
| `src/` | stream layer, shadow VIA/PSG, MiSTer and file backends, `pitrexio-mister.c` |
| `tests/ptx_selftest.c` | bring-up test without the PiTrex library (square, cross, joystick dot, button marks, status line) |
| `tests/ptx_peek.c` | dumps the shared DDR block (header, status, input) |
| `games/vecdrive/` | VecDrive, a pseudo-3D sunset racer: SEA / DESERT / CITY sections with checkpoints, 3-2-1 start, cars and trucks, palms and round trees, engine sound or an original synthwave loop (button 3), plus a sunset overlay (`make_overlay.py` -> `VecDrive.art`, built with `--max-colors 256`) |
| `games/gyrocks/` | Gyrocks by Carsten Wartmann (GPL-2) with additions: ship damage from rocks and enemies, 3 lives, vector explosions, sound, game over (fire is button 4) |
| `scripts/` | Scripts-menu launchers (`PiTrex_<Game>.sh`, `PiTrex_Stop.sh`) and `pitrex_launch.sh` |
| `games/vecbreak/` | VecBreak, a small Breakout on the PiTrex library (analog joystick, buttons, PSG sound and an intro jingle), plus its overlay: `make_overlay.py` draws `VecBreak.png` from the game's coordinates and the core's Overlay Tools turn it into `VecBreak.art` (copy to `games/Vectrex/Overlays/` and load it from the OSD) |
| `tools/` | library patcher, build/run scripts, the PC stream player, a Quartus seed sweep and timing report script |

## Credits
- Videodr0me: the enhanced Vectrex core this builds on, and the DDR arbiter design.
- The PiTrex project (Kevin Koster, Malban, Graham Toal, James Churchill and others): the
  library and interface this targets, notably Kevin Koster's `pitrexio-gpio.c` API.
- The analog model in `ptxplay` follows vecx's.

Licence: GPL-2.0-or-later (see the SPDX headers). The PiTrex library is not included; it is
fetched and patched at build time.

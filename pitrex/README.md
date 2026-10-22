# PiTrex mode for the MiSTer Vectrex core

[PiTrex](https://github.com/gtoal/pitrex) is a cartridge with a Raspberry Pi in it: the Pi takes
over a real Vectrex and drives its hardware directly, which lets far more powerful software run
on the console. PiTrex mode does the same on a MiSTer. The program runs on the DE10-Nano's own
ARM processor, and the Vectrex core takes the place of the console. You don't need a Pi or a
cartridge.

The same route also runs games written for the **UVMC2** multicart, rebuilt for the ARM: see
[UVMC2.md](UVMC2.md).

With **PiTrex Mode** off (the default), the core behaves exactly like
[Videodr0me's Vectrex core](https://github.com/MiSTer-devel/Vectrex_MiSTer), which this builds on.

**Contents:** [Install](#install) · [Playing](#playing) · [Updating and removing](#updating-and-removing) ·
[Troubleshooting](#troubleshooting) · [How it works](#how-it-works) · [Building](#building) ·
[Credits](#credits-and-licence)

## Install
1. Download `PiTrex_MiSTer_<build>_<date>.zip` from the
   [Releases page](https://github.com/z00p/Vectrex_MiSTer/releases).
2. Unzip it onto the root of the MiSTer's SD card (`/media/fat`), over the existing folders.
   It adds:
   | Path | What |
   |---|---|
   | `_Unstable/Vectrex_PiTrex_<build>_<date>.rbf` | the Vectrex core with PiTrex mode |
   | `pitrex/` | the programs and the launcher |
   | `Scripts/PiTrex_*.sh` | Setup, Update, Stop and Uninstall |
   | `games/Vectrex/Overlays/` | overlays for VecBreak, VecDrive and Tac/Scan |

   Unzipping changes nothing else on the card. In particular, **the official Vectrex core in
   `_Console` is not replaced or modified**: the PiTrex core is a separate file in `_Unstable`,
   so the normal Vectrex entry and its updates (e.g. from update_all) carry on as before.
3. **Required:** run Scripts > **PiTrex_Setup** once. It adds the **PiTrex & UVMC2** folder
   to the main menu, where each entry loads the core, its overlay and the program in one go.
   - It switches PiTrex Mode on and sets `log_file_entry=1` in `MiSTer.ini`, so the MiSTer
     reports which file a menu entry loads. It keeps a backup of the ini.
   - It starts a small helper, `pitrexd`, at boot.
   - PiTrex_Uninstall undoes all of it.

## Playing
- **From the main menu** (after PiTrex_Setup): **PiTrex & UVMC2** > e.g. **VecDrive**. The
  core, the overlay and the program load in one go. Games aren't in the Scripts menu; it
  only holds Setup, Update, Stop and Uninstall.
- **From a shell (SSH):** `/media/fat/pitrex/pitrex_launch.sh vecdrive` loads the PiTrex core
  if needed, switches PiTrex Mode on and starts the program.
- **Overlays:** OSD > **Load Overlay**, then pick `VecDrive.art` or `VecBreak.art`.
- **Stop:** Scripts > **PiTrex_Stop**. The Vectrex resets and runs its BIOS (or the loaded
  cartridge) again.

| Program | Controls |
|---|---|
| **VecDrive**: pseudo-3D racer. Four timed phases across nine scenery themes, a fork at the end of each phase to choose your route, hills, traffic and the finish screen | stick steers, button 1 gas (and start), button 2 brake, button 3 cycles the engine sound, the built-in tune and any music you add (see below) |
| **VecBreak**: Breakout with sound and an intro tune | stick left/right moves the bat, button 1 serves and restarts |
| **Gyrocks**: Carsten Wartmann's tunnel shooter, with damage, lives, explosions and sound added | stick moves, button 4 fires, button 1 restarts after game over |
| **Cube demo**: the PiTrex rotating cube | none |
| **Selftest**: checks the link to the core; shows the stick, the buttons and the core's build | stick and buttons |
| **UVMC2 Hello**: the UVMC2 team's example game | stick steers, a button thrusts |
| **Tac/Scan** (UVMC2 port, build it yourself; see [UVMC2.md](UVMC2.md)) | 1 fire, 2 grab, 3 coin, 4 start |

The core's own pause button also pauses PiTrex programs.

**Your own music in VecDrive:** put AY-3-8912 register dumps in `/media/fat/pitrex/music/`
as `.ay14` files: 14 bytes per 50 Hz frame (registers 0 to 13 in order). An optional `.loop`
file with the same name sets the loop point (`loop=<frame>`). The first dump plays during the
race, and button 3 cycles through the rest. None are included.

## Updating and removing
- **PiTrex_Update** (Scripts) downloads the newest release from GitHub and unpacks it. It
  needs the MiSTer to be online. The new core goes into `_Unstable` next to the older PiTrex
  builds (delete those by hand if you like); the main-menu entries switch to the newest. It
  never touches the official core in `_Console`.
- **PiTrex_Uninstall** removes everything PiTrex added, including the optional extras and all
  `Vectrex_PiTrex_*` cores in `_Unstable`. The official core and your Vectrex settings are kept.

## Troubleshooting
| Symptom | Likely cause and fix |
|---|---|
| The program starts but the Vectrex just shows its BIOS screen | PiTrex Mode is off. OSD > System > **PiTrex Mode** = On; to keep it, OSD > (right) > **Save settings**. The launcher also switches it on when it loads the core. |
| The **PiTrex & UVMC2** folder is missing or a game isn't in it | Run Scripts > PiTrex_Setup. An entry only appears for a program that is in `/media/fat/pitrex` (Tac/Scan once you've built and copied it). |
| The picture freezes or stutters | Another heavy job is running on the ARM (e.g. an update or a network copy). Stop it, or run PiTrex_Stop and start the program again. |
| SSH or the network can't reach the MiSTer | A VPN on your PC can block local network traffic. Turn it off, or allow local network access. |
| A main-menu entry loads the core but the game doesn't start | Run Scripts > PiTrex_Setup (it starts `pitrexd` and sets `log_file_entry=1`), then try again. |
| A UVMC2 picture looks too big or too small | Set `scale=<n>` in `/media/fat/pitrex/uvmc2.cfg` (default 120; larger means bigger). |
| You want to see what a program is doing | Its output goes to `/tmp/pitrex.log` on the MiSTer (`/tmp/pitrexd.log` for menu starts). |

Problems and suggestions: please use this fork's
[Issues](https://github.com/z00p/Vectrex_MiSTer/issues), not Videodr0me's repository.

## How it works
The program runs on the ARM under Linux. The PiTrex library is built unchanged, except for two
things:
- a replacement for `pitrexio-gpio.c` (`src/pitrexio-mister.c`);
- about 14 timing functions, patched by `tools/patch_lib.py` so that delays advance stream time
  instead of busy-waiting.

Every VIA write becomes a 64-bit command in a ring in DDR3, at byte address `0x34000000`.
The core fetches the commands through its DDR arbiter and replays them at exact E-clock timing,
with the 6809 held in reset. Reads never wait for the FPGA:
- VIA timers and flags come from a shadow VIA on the ARM;
- buttons and the joystick come from an input block that the core writes back every 0.5 ms.

Safety:
- With PiTrex Mode On, the core only *listens*, and the Vectrex runs normally until a live
  program appears.
- The core takes over only while the program sends a coded heartbeat.
- It hands back on a clean exit or after 100 ms of silence (a crash).

The full format is in [PROTOCOL.md](PROTOCOL.md).

## Building
### ARM programs (Linux or WSL)
```
git clone https://github.com/gtoal/pitrex upstream/pitrex   # the PiTrex library and demos
make                                     # patch the library copy; build hello + ptxplay for the PC
tools/build_prog.sh arm cube upstream/pitrex/hello_world/cube.c     # static ARM binary
```
ARM builds need `gcc-arm-linux-gnueabihf`. `tests/ptx_selftest.c` doesn't need the PiTrex
library:
```
arm-linux-gnueabihf-gcc -O2 -static -Isrc -o ptx_selftest tests/ptx_selftest.c src/ptx_*.c
```
To run one by hand over SSH (as root, for `/dev/mem`): switch PiTrex Mode on, then run
`PTX_BACKEND=mister ./cube`.

### Testing without a MiSTer
By default a program writes its command stream to a file. `tools/ptxplay` replays it through a
software VIA and a vecx-style analogue model, and writes PNG frames plus per-frame statistics:
```
tools/run_stream.sh cube 40 30 1          # program, frames to run, first frame to render, count
```
Fake inputs with `PTX_POT="x1,y1,x2,y2"`, `PTX_BUTTONS=0xFE` and `PTX_BUTTONS_FROM=<frame>`.

### The core
Quartus Prime Lite 17.0.2. The project (`Vectrex.qpf`) is in the repository root, and fitter
seed 5 (set in `Vectrex.qsf`) meets timing at all four corners. After a change, run
`tools/seed_sweep.sh <seeds>`, which re-fits and checks timing with `--multicorner=on`.

### Source map
| Path | What |
|---|---|
| `src/` | stream layer, shadow VIA/PSG, the MiSTer and file backends, `pitrexio-mister.c` |
| `tests/` | `ptx_selftest.c` (bring-up without the library), `ptx_peek.c` (dumps the shared block) |
| `games/vecdrive/`, `games/vecbreak/` | the two games, with their overlay generators (`make_overlay.py`) |
| `games/gyrocks/` | Gyrocks with our additions (see its README) |
| `scripts/` | the launcher, the menu builder, pitrexd and the Scripts-menu tools |
| `tools/` | library patcher, build and run scripts, `ptxplay`, seed sweep and timing reports |
| `../rtl/pitrex/` | the core side: `pitrex_ddr.sv` (ring reader) and `pitrex_player.sv` (bus player) |

## Credits and licence
- **Videodr0me:** the enhanced Vectrex core, its renderer and the DDR arbiter design. Please
  support his work: [buymeacoffee.com/videodr0me](https://buymeacoffee.com/videodr0me).
- **The PiTrex project** (Kevin Koster, Malban, Graham Toal, James Churchill and others): the
  library and interface this targets, notably Kevin Koster's `pitrexio-gpio.c` API.
- **Carsten Wartmann:** Gyrocks.
- **The UVMC2 team** ([uvmc2-sdk](https://github.com/tullulah/uvmc2-sdk)): the UVMC2 runtime, beam model, libvpy and games, published here with their permission. See [UVMC2.md](UVMC2.md) for the full credits.
- The analogue model in `ptxplay` follows vecx's.
- The coding and testing were AI-assisted (Claude), with everything checked on real hardware.

Licence: GPL-2.0-or-later (see the SPDX headers). The PiTrex library isn't included; it's
fetched and patched at build time.

# PiTrex mode for the MiSTer Vectrex core

[PiTrex](https://github.com/gtoal/pitrex) is a cartridge with a Raspberry Pi in it: the Pi takes
over a real Vectrex and drives its hardware directly, which lets far more powerful software run
on the console. PiTrex mode does the same on a MiSTer. The program runs on the DE10-Nano's own
ARM processor, and the Vectrex core takes the place of the console. You don't need a Pi or a
cartridge.

The same route also runs games written for the **UVMC2** multicart, rebuilt for the ARM: see
[UVMC2.md](UVMC2.md). And, as an unofficial **alpha**, the UVMC2 cartridge images themselves
(`.um2` files, unmodified), picked in the core's OSD: start with [ADDING_GAMES.md](ADDING_GAMES.md)
(where your images and the files from your UVMC2 SD card go), and see [UVMC2_GAMES.md](UVMC2_GAMES.md)
for the details.

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
   | `_Unstable/Vectrex-PiTrex_<YYYYMMDD>.rbf` | the Vectrex core with PiTrex mode; the menu entries load it as "Vectrex-PiTrex", so its settings live in their own `config/Vectrex-PiTrex_v1.CFG` |
   | `pitrex/` | the programs and the launcher |
   | `pitrex/um2run/` | `um2run`, which runs UVMC2 cartridge images (alpha, see [UVMC2_GAMES.md](UVMC2_GAMES.md)) |
   | `games/Vectrex-PiTrex/UVMC2/` | where you put your `.um2` images (created by PiTrex_Setup; no images are included) |
   | `Scripts/PiTrex_*.sh` | Setup, Update, Stop and Uninstall |
   | `games/Vectrex/Overlays/` | overlays for VecBreak, VecDrive, Tac/Scan and Gong Rider |

   Every program is pre-built; you never compile anything. The only extras are your own MAME
   romset (Tac/Scan; usually already in `/media/fat/games/mame`).

   Unzipping changes nothing else on the card. In particular, **the official Vectrex core in
   `_Console` is not replaced or modified**: the PiTrex core is a separate file in `_Unstable`,
   so the normal Vectrex entry and its updates (e.g. from update_all) carry on as before.
3. **Required:** run Scripts > **PiTrex_Setup** once. It adds the **PiTrex & UVMC2** folder
   to the main menu, where each entry loads the core, its overlay and the program in one go.
   - It switches PiTrex Mode on and adds a `[Vectrex-PiTrex]` section with `log_file_entry=1`
     to `MiSTer.ini`, so the MiSTer reports which file a menu entry or an OSD pick loads. Only
     our core is affected; your other settings are left alone (details in
     [UVMC2_GAMES.md](UVMC2_GAMES.md#what-pitrex_setup-changes)).
   - It starts a small helper, `pitrexd`, at boot.
   - PiTrex_Uninstall undoes all of it.

## Playing
- **From the main menu** (after PiTrex_Setup): **PiTrex & UVMC2** > e.g. **VecDrive**. The
  core, the overlay and the program load in one go. Games aren't in the Scripts menu; it
  only holds Setup, Update, Stop, Uninstall and Make_UVMC2_Card (which builds the UVMC2 card image,
  see [ADDING_GAMES.md](ADDING_GAMES.md)).
- **UVMC2 cartridge images (`.um2`, alpha):** main menu > **PiTrex & UVMC2** > **UVMC2 Games**, then
  OSD > **Load UVMC2 / PiTrex**. How to add your games and the files from your UVMC2 SD card:
  [ADDING_GAMES.md](ADDING_GAMES.md). Per-game settings and the tested games are in
  [UVMC2_GAMES.md](UVMC2_GAMES.md).
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
| **Gong Rider**: Roger Boesch's Master of the Lamps-style game, with an overlay | stick steers and strikes the gongs; buttons 1 to 4 strike the gongs too |
| **Cube demo**: the PiTrex rotating cube | none |
| **Selftest**: checks the link to the core; shows the stick, the buttons and the core's build | stick and buttons |
| **UVMC2 Hello**: the UVMC2 team's example game | stick steers, a button thrusts |
| **UVMC2 Playroom**: the UVMC2 team's showcase, a walking figure, a hub and four rooms | stick up walks (all the way up runs), left/right turns, button 2 waves; walk into a door to play |
| **UVMC2 FX demo**: shoot a wireframe shape and watch it come apart | stick aims, 1 fires, 2 blows it apart, 3 dissolves it, 4 starts again |
| **Tac/Scan** (UVMC2 port, included; reads your MAME `tacscan.zip` from `/media/fat/games/mame`; see [UVMC2.md](UVMC2.md) for the optional sound) | 1 fire, 2 grab, 3 coin, 4 start |

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
  PiTrex cores in `_Unstable` and `config/Vectrex-PiTrex_v1.CFG`. The official core, your Vectrex settings and your own files (`games/Vectrex-PiTrex`, `pitrex/uvmc2` with your card folder and card images, and anything of yours in `pitrex/um2run`) are kept.

## Troubleshooting
| Symptom | Likely cause and fix |
|---|---|
| The program starts but the Vectrex just shows its BIOS screen | PiTrex Mode is off. OSD > System > **PiTrex Mode** = On; to keep it, OSD > (right) > **Save settings**. The launcher also switches it on when it loads the core. |
| The **PiTrex & UVMC2** folder is missing or a game isn't in it | Run Scripts > PiTrex_Setup. An entry only appears for a program that is in `/media/fat/pitrex` (Tac/Scan needs nothing built, only the romset). |
| The picture freezes or stutters | Another heavy job is running on the ARM (e.g. an update or a network copy). Stop it, or run PiTrex_Stop and start the program again. |
| SSH or the network can't reach the MiSTer | A VPN on your PC can block local network traffic. Turn it off, or allow local network access. |
| A main-menu entry loads the core but the game doesn't start | Run Scripts > PiTrex_Setup (it starts `pitrexd` and adds the `[Vectrex-PiTrex]` section to `MiSTer.ini`), then try again. If it still fails, note the cause byte in `devmem 0x3400005C 32` (bits 15..8, see [PROTOCOL.md](PROTOCOL.md)) and report it. |
| SSH or scripts are very slow while a game runs | Expected: MiSTer Main runs at real-time priority and keeps the CPU busy, and PiTrex programs and `pitrexd` run just above it (`chrt -r 49`) so they keep their timing. |
| A UVMC2 picture looks too big or too small | Set `scale=<n>` in `/media/fat/pitrex/uvmc2.cfg` (default 120; larger means bigger). |
| A `.um2` game doesn't start | See [UVMC2_GAMES.md](UVMC2_GAMES.md#troubleshooting): both ARM CPUs must be online (reboot after BigMig), and `/tmp/pitrexd.log` says why a pick was refused. |
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
| `tests/` | `ptx_selftest.c` (bring-up without the library), `ptx_peek.c` (dumps the shared block), `pitrexd_mock.sh` (tests pitrexd on a PC against a scratch directory) |
| `games/vecdrive/`, `games/vecbreak/` | the two games, with their overlay generators (`make_overlay.py`) |
| `games/gyrocks/` | Gyrocks with our additions (see its README) |
| `games/doom/`, `games/gongrider/` | DOOM (MiSTer entry point and platform layer; see its README) and Roger Boesch's Gong Rider, each with its build line |
| `games/uvmc2_tacscan/z80shim/`, `third_party/aae_z80/` | the Z80 core (Tim Cottrill's AAE core) and the adapter Tac/Scan is built with |
| `scripts/` | the launcher, the menu builder, pitrexd and the Scripts-menu tools |
| `um2run/` | runs UVMC2 cartridge images on the ARM (alpha): loader, RP2350 device model, SD card, cart audio, self test; see its README |
| `tools/` | library patcher, build and run scripts, `ptxplay`, seed sweep and timing reports |
| `../rtl/pitrex/` | the core side: `pitrex_ddr.sv` (ring reader) and `pitrex_player.sv` (bus player) |

## Credits and licence
- **Videodr0me:** the enhanced Vectrex core, its renderer and the DDR arbiter design. Please
  support his work: [buymeacoffee.com/videodr0me](https://buymeacoffee.com/videodr0me).
- **The PiTrex project** (Kevin Koster, Malban, Graham Toal, James Churchill and others): the
  library and interface this targets, notably Kevin Koster's `pitrexio-gpio.c` API.
- **Carsten Wartmann:** Gyrocks.
- **Daniel Ferrer:** UVMC2 (with the team) and [PureDOOM-pitrex](https://github.com/tullulah/PureDOOM-pitrex), the wireframe DOOM. **id Software:** the DOOM source (GPL).
- **Roger Boesch:** Gong Rider ([source](https://rogerboesch.games/learn/gong-rider/), MIT licence).
- **Tim Cottrill:** the AAE Z80 core that Tac/Scan runs on, used with his permission ([AAE issue 13](https://github.com/tcottrill/AAE/issues/13)). It is a modified port of **Edward Massey's** 1997 Z80 emulator (GNU LGPL v2, distributed here under the GPL as the LGPL allows); his readme and documentation are in `third_party/aae_z80/`. The earlier mz80 licensing issue no longer applies.
- **The UVMC2 team** ([uvmc2-sdk](https://github.com/tullulah/uvmc2-sdk)): the UVMC2 runtime, beam model, libvpy and games, published here with their permission. See [UVMC2.md](UVMC2.md) for the full credits. `um2run` runs cartridge images built with their SDK; it contains no UVMC2 firmware and no images, and it is not an official UVMC2 tool.
- **Ralf Kornmann:** the UVMC2 firmware. `um2run` runs cartridge images without it, and none of it is included here.
- **Jason Kopp:** the UVMC2 hardware.
- The analogue model in `ptxplay` follows vecx's.
- The coding and testing were AI-assisted (Claude), with everything checked on real hardware.

Licence: GPL-2.0-or-later (see the SPDX headers). The PiTrex library isn't included; it's
fetched and patched at build time.

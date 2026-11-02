# UVMC2 cartridge images on the MiSTer (beta)

The Vectrex-PiTrex core can run **UVMC2 cartridge images** (`.um2` files, the firmware images of
the [UVMC2](https://github.com/tullulah/uvmc2-sdk) multicart) unmodified. The MiSTer's ARM processor
plays the part of the cartridge's RP2350 (the program is called `um2run`, see
[um2run/README.md](um2run/README.md)) and the core is the console.

This is an **unofficial beta**. It is not made by, or supported by, the UVMC2 team: please do not
report MiSTer problems to them, use this fork's
[Issues](https://github.com/z00p/Vectrex_MiSTer/issues). **No cartridge images, ROM sets or card
images are included**: bring your own (and your own MAME ROM sets for the games that need them).

**Contents:** [What you need](#what-you-need) · [Where files go](#where-files-go) ·
[Playing](#playing) · [Sidecar settings](#sidecar-settings) · [Card images](#card-images) ·
[Tested games](#tested-games) · [What PiTrex_Setup changes](#what-pitrex_setup-changes) · [Troubleshooting](#troubleshooting)

## What you need
- A MiSTer (DE10-Nano) with the Vectrex-PiTrex core from this release, and **Scripts > PiTrex_Setup**
  run once.
- **Both ARM CPUs online.** `cat /sys/devices/system/cpu/online` over SSH must say `0-1`. After
  using the BigMig core the MiSTer needs a normal reboot first; until then `pitrexd` refuses to
  start a game and says so on the Vectrex (and writes `REFUSED ... CPUs` to `/tmp/pitrexd.log`),
  because the game would starve the MiSTer's own program (menu, controllers).
- Your `.um2` images, and for some of them the files from your UVMC2 SD card. Start with
  [ADDING_GAMES.md](ADDING_GAMES.md): it has the steps and says which games need a card.

## Where files go
| What | Where |
|---|---|
| Everything from your UVMC2 SD card | `/media/fat/games/Vectrex-PiTrex/UVMC2/` (PiTrex_Setup creates the folder; subfolders are fine): the `.um2` images and the card's files, in one place. The card image `sd.img` is built from this folder, see [ADDING_GAMES.md](ADDING_GAMES.md) |
| PiTrex programs (`.ptx`) | `/media/fat/games/Vectrex-PiTrex/PiTrex/` (PiTrex_Setup copies them there) |
| Sidecar settings `Game.um2.cfg` | next to the image, same name plus `.cfg` |
| Card images (`.img`) | next to the image, or in `/media/fat/pitrex/uvmc2/` (a file called `sd.img` there is the default card for every game without an `sd=` key). The OSD lists do not show them. PiTrex_Uninstall leaves your files in both places alone, and anything of yours in `/media/fat/pitrex/um2run/` (the third place `sd=` names are looked up). |

The core is called "Vectrex-PiTrex", however you load it: its file browser opens
`games/Vectrex-PiTrex` and its settings are in `config/Vectrex-PiTrex_v1.CFG`.

## Playing
There are three ways to start a game:
- **The OSD:** load the core, then **Load UVMC2** > pick the `.um2`. (This core switches PiTrex Mode
  on by itself when you pick a file; if nothing happens, check OSD > System > PiTrex Mode.)
- **The Vectrex menu:** pick **UVMC2 Menu** in **Load UVMC2**, or use **UVMC2 Menu** in the main
  menu's **PiTrex & UVMC2** folder. When a game ends you are back in the menu.
- **An MGL file:** a standard MiSTer shortcut, see [ADDING_GAMES.md](ADDING_GAMES.md#three-ways-to-start-a-game).

Then:
1. A LOADING screen with a progress bar shows while the cartridge boots from its card; the big images take 10 to 20 seconds. The first second or two of sound can be static.
2. Picking another game stops the running one and starts the new one. **Scripts > PiTrex_Stop** stops it.

Controls are the cartridge's own. BattleZone: button 1 starts, button 4 fires. **The OSD is sluggish while a game runs**
(the MiSTer's menu program shares a CPU with the game): it works, give it a moment.

## Sidecar settings
Optional file `<image>.um2.cfg` next to the image, one `key=value` per line, `#` for comments.
Nothing in it is ever executed. Unknown keys are ignored; bad values are ignored with a line in
`/tmp/pitrexd.log`.

| Key | Meaning | Default |
|---|---|---|
| `frame_cycles=25000` | Length of one stream frame in Vectrex E cycles. 30000 is 50 Hz, 25000 is 60 Hz. Images with "punch" in the name get 25000 without a sidecar. | 30000 (Punch-Out 25000) |
| `sd=name.img` | Card image the cartridge sees. A name without a folder is looked up next to the `.um2`, then in `/media/fat/pitrex/uvmc2/`, then in `/media/fat/pitrex/um2run/`. No spaces in the path. | `pitrex/uvmc2/sd.img` if it exists, else no card |
| `args=--secs 600` | Extra `um2run` arguments, appended last (see um2run/README.md). | none |
| `settle=8` | Seconds to wait before starting (lets the MiSTer finish setting the video mode). | 8 after a core load, 1 if the core has been up 15 s |
| `cpu1_after=10` | Seconds the game's second core stays on CPU0 before moving to CPU1. | 10 after a core load, 2 if the core has been up 15 s |
| `orient=0` / `ccw` / `cw` | **Experimental.** Set the core's Orientation (normal / rotated 90 degrees counter-clockwise / clockwise) for this game and reload the core if the saved setting differs. | leave as it is |
| `view=standard` / `open` | **Experimental.** Set the core's View Area (standard / open matte). Open matte is the default for this core: PiTrex_Setup or PiTrex_Update sets it once, and your OSD choice stands after that. | leave as it is |

Examples:
```
# Punch-Out.um2.cfg
frame_cycles=25000
sd=punchout_sd.img
```

### orient and view (experimental)
Landscape games (Star Wars, BattleZone) are drawn sideways on the portrait Vectrex screen, so
their sides are cropped. `orient` and `view` change the core's own Orientation and View Area
options for you before the game starts: pitrexd edits only
`config/Vectrex-PiTrex_v1.CFG`, reloads the core, then starts the game. A setting you do not
name is left alone (so it stays as the last game or you left it: add `orient=0` to the sidecars
of your portrait games if you mix them). **Not tested on hardware:** which direction (`cw` or
`ccw`) is the right one, and whether the cartridge's own PICTURE option must be changed as well. `VECTREX_v1.CFG` is never touched. You can always do the same by hand in the OSD.

## Card images
Some images read their ROM set from an SD card and keep scores and settings on it. The card
is **emulated from an ordinary file**: a FAT32 image of the whole card without a partition
table (a "superfloppy"). The largest tried on a MiSTer is a 3.8 GiB card with
Star Wars. A raw dump of a real card (with its partition table) is untested: copy the card's
files instead.

**The easy way is in [ADDING_GAMES.md](ADDING_GAMES.md):** copy everything on your UVMC2 SD card
(or just your ROM zips) to `/media/fat/games/Vectrex-PiTrex/UVMC2/` and pick **REFRESH CARD** in
the Vectrex menu, or run **Scripts > PiTrex_Make_UVMC2_Card**. That builds
`/media/fat/pitrex/uvmc2/sd.img` on the MiSTer and keeps the saves of the old card.
ADDING_GAMES.md also lists which files each tested game needs.

**Folders the images expect** (all lower or upper case, the card is case-insensitive):
```
roms/                         ROM sets as MAME zips: your own MAME sets
config/                       created and written by the games (settings)
scores/                       created and written by the games (high scores)
PORTDATA/Punchout/BASEROMS/punchout.zip      Punch-Out
PORTDATA/Punchout/CONFIG/     created and written by Punch-Out (settings)
```
**Star Wars** takes each ROM from whichever zip has it (matched by checksum, not by name), looking in
`roms/starwars1.zip`, `roms/starwars.zip` and `roms/starwar1.zip`. The program ROM
`136021-114` is in `starwar1`; the other seven are in `starwars`.

### Advanced: build a card on a PC
On Linux (or WSL on Windows, tested). You need `dosfstools` and `mtools`
(`sudo apt install dosfstools mtools`). In a folder that holds your ROM zips:
```
mkfs.vfat -C -F 32 -s 1 -n UVM2SD sd.img 65536       # 64 MiB, FAT32, 512-byte clusters, no partition table
mmd -i sd.img ::roms ::config ::scores ::PORTDATA ::PORTDATA/Punchout ::PORTDATA/Punchout/BASEROMS
mcopy -i sd.img starwars.zip starwar1.zip punchout.zip ::roms/
mcopy -i sd.img punchout.zip ::PORTDATA/Punchout/BASEROMS/
mdir -/ -i sd.img ::                                  # check what is on it
```
`-s 1` matters: FAT32 needs at least 65525 clusters, which a 64 MiB image only reaches with
small clusters. Then copy
the image to `/media/fat/pitrex/uvmc2/` on the MiSTer (with the SD card in your PC, or over the network).
A card you build this way is left alone unless you put newer files in the UVMC2 folder and build
the card there.

On Windows, the easiest way is **WSL** with the commands above: the image is a normal file that you can
find from Windows under `\\wsl$\...` or in your WSL home. A tool that mounts raw disk images
as drive letters (for example OSFMount) should also work if you format the mounted image
as FAT32 with 512-byte clusters and no partition table; that route has not been tested here.

### Checking a card image
`fsck.vfat -n sd.img` should report no errors, and `mdir -/ -i sd.img ::` should list your zips.
If a game says it cannot find its ROMs, the usual causes are a wrong folder, a misspelt zip name, or a ROM set from a
different MAME version (the games check the files' checksums).

## Tested games
On a DE10-Nano, 26 October to 2 November 2026 (the B10 core from 29 October), with the images named below. Frame rates are from the attract modes and vary with the scene.
Other builds of the same games may behave differently. These are the images used for testing. They were shared in UVMC2 community groups; download links will be added once their authors are happy for us to link them.

| Game (image tested) | Card | How it ran |
|---|---|---|
| Star Wars (`Star_Wars.um2`) | your MAME `starwars` + `starwar1` zips in `roms/` | music and speech good; about 45 frames/s; whole picture with Open Matte |
| BattleZone 0.3a (`BattleZone_0_3a.um2`) | a card image, no ROMs (it writes to the card) | plays well, cart audio good; about 40 frames/s; whole picture with Open Matte. Button 1 starts, button 4 fires |
| Pac-Man (`pacman-uvmc2-SD-card.um2`) | a card image with the folders, no ROMs | cart audio good; about 32 frames/s, game speed right |
| Punch-Out (`Punchout.um2`) | your MAME `punchout.zip` | plays well, close to full speed (about 55 of its 60 frames/s); sound comes through the Vectrex PSG; its JACK audio route is untested |
| Veccy Road 0.10 (`veccy_road_v010.um2`) | none | plays well, with sound, about 50 frames/s |

The sidecar files used for these runs (the card names are up to you):
```
# Star_Wars.um2.cfg
sd=uvmc2_sd.img
view=open
```
```
# BattleZone_0_3a.um2.cfg
sd=bz_sd.img
view=open
```
```
# pacman-uvmc2-SD-card.um2.cfg
sd=uvmc2_sd.img
```
```
# Punchout.um2.cfg
frame_cycles=25000
sd=uvmc2_sd.img
```
Veccy Road needs no sidecar. With a card built from the UVMC2 folder (see
[ADDING_GAMES.md](ADDING_GAMES.md)), `sd.img` is the default card, so the `sd=` lines are not needed.

A game picked from the OSD does not load an overlay, and the last cartridge's overlay can stay
on screen: OSD > Video Overlay & Geometry > Overlay = Off.

## What PiTrex_Setup changes
- **MiSTer.ini:** it adds this section at the end of the file:
  ```
  [Vectrex-PiTrex]
  log_file_entry=1
  ```
  `log_file_entry` is a standard MiSTer.ini option: with it on, the MiSTer's menu program writes
  the name of the file you pick in the OSD to a few small files in `/tmp`. `pitrexd` reads them to
  know which game to start. In a `[Vectrex-PiTrex]` section it is only on while our core runs;
  your `[MiSTer]` settings and every
  other core are left as they were. If you use an alternative ini (`MiSTer_alt_1.ini` and so on),
  add the same two lines to that file yourself.
- **Older releases** (B8 and earlier) set `log_file_entry=1` for all cores in `[MiSTer]` and kept a
  backup, `MiSTer.ini.pitrex-bak`. Setup now puts that line back as it was in the backup, then
  deletes the backup.
- **config/Vectrex-PiTrex_v1.CFG:** PiTrex Mode on (our core's own settings file; the official
  core's `VECTREX_v1.CFG` is not used).
- **linux/user-startup.sh:** a comment and one line that start `pitrexd` at boot (after a blank line).
- **Folders:** `games/Vectrex-PiTrex/UVMC2` (with a short README.txt), `games/Vectrex-PiTrex/PiTrex`,
  `pitrex/uvmc2`, and the menu folder `_PiTrex & UVMC2`.
- **Scripts:** the entry `PiTrex_Make_UVMC2_Card`.
- **The card:** if `pitrex/uvmc2/sd.img` is missing or out of date, it builds `sd.img` from the
  UVMC2 folder, see [ADDING_GAMES.md](ADDING_GAMES.md).

**PiTrex_Uninstall** undoes all of it (it removes the two MiSTer.ini lines, and the section header
when nothing else is in it) and leaves your own files alone: the UVMC2 folder, card images and
their `.bak` backups, sidecars, and anything of yours in `pitrex/um2run/`.

## Troubleshooting
| Symptom | What to do |
|---|---|
| Nothing happens after the pick | `cat /tmp/pitrexd.log` over SSH. `REFUSED ... CPUs` means a reboot is needed (BigMig). `ignored a stale file report` is normal after a restart of the MiSTer's menu program. Check OSD > System > PiTrex Mode = On. If `/tmp/pitrexd.log` shows nothing at all for the pick, check that MiSTer.ini (or the alternative ini you use) has the `[Vectrex-PiTrex]` section. |
| PiTrex_Setup says MiSTer.ini could not be changed | Picks in the OSD will not start games until it is fixed. Check that the SD card is not full or read-only and run PiTrex_Setup again, or add the two lines shown in [What PiTrex_Setup changes](#what-pitrex_setup-changes) to `MiSTer.ini` yourself. |
| "Missing directory: Vectrex-PiTrex" in the OSD | Run PiTrex_Setup (it creates the folder) or create `games/Vectrex-PiTrex/UVMC2` yourself. |
| The game says ROMS NOT FOUND or MISSING | See [ADDING_GAMES.md](ADDING_GAMES.md#which-games-need-a-card): which files, which folder, then build the card again. If the game's sidecar has an `sd=` line, it uses that card, not `sd.img`. |
| Black screen for a long time | Wait for the LOADING screen to finish. If the screen stays black or the MiSTer's menu stops responding: Scripts > PiTrex_Stop, or SSH `killall um2run`. |
| Static at the start | Expected for the first second or two. Up to about 10 seconds when the pick had to switch View Area (the core reloads first, see `/tmp/pitrexd.log`); the next pick starts clean. |
| The picture is cropped at the sides | Landscape game on a portrait screen. OSD > View Area = Open Matte shows it all (the default, unless you changed it), or put `view=open` in the sidecar. |
| A message on the Vectrex instead of the game | CARD OUT OF DATE: the game still starts, on the current card; pick REFRESH CARD in the Vectrex menu when you have time. LINUX IS ON ONE CPU: reboot the MiSTer. UM2RUN IS MISSING: run Scripts > PiTrex_Update. GAME FILE NOT FOUND: the file went away between the pick and the start. GAME STOPPED: the game ended by itself; `/tmp/um2.log` has why and `/tmp/pitrexd.log` the exit status. |
| The game froze | Check the core's Pause button first (a mapped controller button, or Pause when OSD open): a paused core stops taking the picture and the game waits for it. `/tmp/um2.log` then says `core not fetching`, and `core fetching again` when it carries on. |
| An old overlay is on top of the game | OSD > Video Overlay & Geometry > Overlay = Off. |
| A controller button does something different from the normal Vectrex core (e.g. View) | The MiSTer keeps controller mappings per core name, so "Vectrex-PiTrex" has its own mapping, separate from VECTREX's. Set it up once in the OSD (Define joystick buttons). |
| Log of the last game | `/tmp/um2.log` (the game), `/tmp/um2.prev.log` (the game before it), `/tmp/pitrexd.log` (picks and settings). |

# UVMC2 cartridge images on the MiSTer (alpha)

The Vectrex-PiTrex core can run **UVMC2 cartridge images** (`.um2` files, the firmware images of
the [UVMC2](https://github.com/tullulah/uvmc2-sdk) multicart) unmodified. The MiSTer's ARM processor
plays the part of the cartridge's RP2350 (the program is called `um2run`, see
[um2run/README.md](um2run/README.md)) and the core is the console.

This is an **unofficial alpha**. It is not made by, or supported by, the UVMC2 team: please do not
report MiSTer problems to them, use this fork's
[Issues](https://github.com/z00p/Vectrex_MiSTer/issues). **No cartridge images, ROM sets or card
images are included**: bring your own (and your own MAME ROM sets for the games that need them).

**Contents:** [What you need](#what-you-need) · [Where files go](#where-files-go) ·
[Playing](#playing) · [Sidecar settings](#sidecar-settings) · [Card images](#card-images-how-to-make-one) ·
[Tested games](#tested-games) · [What PiTrex_Setup changes](#what-pitrex_setup-changes) · [Troubleshooting](#troubleshooting)

## What you need
- A MiSTer (DE10-Nano) with the Vectrex-PiTrex core from this release, and **Scripts > PiTrex_Setup**
  run once.
- **Both ARM CPUs online.** `cat /sys/devices/system/cpu/online` over SSH must say `0-1`. After
  using the BigMig core the MiSTer needs a normal reboot first; until then `pitrexd` refuses to
  start a game (it writes `REFUSED ... CPUs` to `/tmp/pitrexd.log`), because the game would starve
  the MiSTer's own program (menu, controllers).
- Your `.um2` images, and for some of them a card image (below).

## Where files go
| What | Where |
|---|---|
| `.um2` images | `/media/fat/games/Vectrex-PiTrex/UVMC2/` (PiTrex_Setup creates the folder; subfolders are fine). `.ptx` programs also show up in the same OSD list. |
| Sidecar settings `Game.um2.cfg` | next to the image, same name plus `.cfg` |
| Card images (`.img`) | next to the image, or in `/media/fat/pitrex/uvmc2/` (a file called `sd.img` there is the default card for every game without an `sd=` key). The OSD list shows only `.um2` and `.ptx`, so the images do not clutter it. PiTrex_Uninstall leaves your files in both places alone. |

The core's file browser opens `games/<core name>`. The menu entry **UVMC2 Games** loads the core
as "Vectrex-PiTrex", so it opens `games/Vectrex-PiTrex`. If you load the core by hand from
`_Unstable` instead, it runs as "VECTREX", opens `games/Vectrex`, uses the Vectrex settings
file, cannot change Orientation or View Area for you (see below), and **a pick in the OSD does not
start anything** (see [What PiTrex_Setup changes](#what-pitrex_setup-changes)).

## Playing
1. Main menu > **PiTrex & UVMC2** > **UVMC2 Games**. This loads the core with PiTrex mode on.
2. OSD > **Load UVMC2 / PiTrex** > pick the `.um2`. (This core switches PiTrex Mode on by itself
   when you pick a file; if nothing happens, check OSD > System > PiTrex Mode.)
3. A LOADING screen with a progress bar shows while the cartridge boots from its card; the big images take 10 to 20 seconds. The first second or two of sound can be static.
4. Picking another game stops the running one and starts the new one. **Scripts > PiTrex_Stop** stops it.

Controls are the cartridge's own. BattleZone: button 1 starts, button 4 fires. **The OSD is sluggish while a game runs**
(the MiSTer's menu program shares a CPU with the game): it works, give it a moment.

## Sidecar settings
Optional file `<image>.um2.cfg` next to the image, one `key=value` per line, `#` for comments.
Nothing in it is ever executed. Unknown keys are ignored; bad values are ignored with a line in
`/tmp/pitrexd.log`.

| Key | Meaning | Default |
|---|---|---|
| `frame_cycles=25000` | Length of one stream frame in Vectrex E cycles. 30000 is 50 Hz, 25000 is 60 Hz. Punch-Out wants 25000. | 30000 |
| `sd=name.img` | Card image the cartridge sees. A name without a folder is looked up next to the `.um2`, then in `/media/fat/pitrex/uvmc2/`, then in `/media/fat/pitrex/um2run/`. No spaces in the path. BattleZone **writes** to its card: give it its own copy. | `pitrex/uvmc2/sd.img` if it exists, else no card |
| `args=--secs 600` | Extra `um2run` arguments, appended last (see um2run/README.md). | none |
| `settle=8` | Seconds to wait before starting (lets the MiSTer finish setting the video mode). | 8 after a core load, 1 if the core has been up 15 s |
| `cpu1_after=10` | Seconds the game's second core stays on CPU0 before moving to CPU1. | 10 after a core load, 2 if the core has been up 15 s |
| `orient=0` / `ccw` / `cw` | **Experimental.** Set the core's Orientation (normal / rotated 90 degrees counter-clockwise / clockwise) for this game and reload the core if the saved setting differs. | leave as it is |
| `view=standard` / `open` | **Experimental.** Set the core's View Area (standard / open matte). | leave as it is |

Examples:
```
# Punch-Out.um2.cfg
frame_cycles=25000
sd=punchout_sd.img
```
```
# BattleZone.um2.cfg
sd=bz_sd.img
```

### orient and view (experimental)
Landscape games (Star Wars, BattleZone) are drawn sideways on the portrait Vectrex screen, so
their sides are cropped. `orient` and `view` change the core's own Orientation and View Area
options for you before the game starts: pitrexd edits only
`config/Vectrex-PiTrex_v1.CFG`, reloads the core, then starts the game. A setting you do not
name is left alone (so it stays as the last game or you left it: add `orient=0` to the sidecars
of your portrait games if you mix them). **Not tested on hardware:** which direction (`cw` or
`ccw`) is the right one, and whether the cartridge's own PICTURE option must be changed as well. If the core runs as
VECTREX (not loaded from the menu entry) the keys are skipped with a line in the log, and
`VECTREX_v1.CFG` is never touched. You can always do the same by hand in the OSD.

## Card images: how to make one
Some images read their ROM set from an SD card and keep scores and settings on it. The card
is **emulated from an ordinary file**: a FAT32 image of the whole card without a partition
table (a "superfloppy"). It is tested with 64 MB images; other sizes should work.

**Folders the images expect** (all lower or upper case, the card is case-insensitive):
```
roms/                         ROM sets as MAME zips: your own MAME sets
config/                       created and written by the games (settings)
scores/                       created and written by the games (high scores)
PORTDATA/Punchout/BASEROMS/punchout.zip      Punch-Out
```
- **Pac-Man** needs no ROM set; it only writes `config/` and `scores/`.
- **BattleZone** needs no ROM set (it is in the image) but writes to the card: use its own copy.
- **Star Wars** needs your own MAME `starwars.zip` and `starwar1.zip` in `roms/`. It takes each
  ROM from whichever zip has it (matched by checksum, not by name), looking in
  `roms/starwars1.zip`, `roms/starwars.zip` and `roms/starwar1.zip`. The program ROM
  `136021-114` is in `starwar1`; the other seven are in `starwars`.
- **Punch-Out** needs your own MAME `punchout.zip` at `PORTDATA/Punchout/BASEROMS/punchout.zip`
  (the working card also has a copy in `roms/`).
- **Veccy Road** and the demos need no card.

### On Linux (or WSL on Windows, tested)
You need `dosfstools` and `mtools` (`sudo apt install dosfstools mtools`). In a folder that holds
your ROM zips:
```
mkfs.vfat -C -F 32 -s 1 -n UVM2SD sd.img 65536       # 64 MiB, FAT32, 512-byte clusters, no partition table
mmd -i sd.img ::roms ::config ::scores ::PORTDATA ::PORTDATA/Punchout ::PORTDATA/Punchout/BASEROMS
mcopy -i sd.img starwars.zip starwar1.zip punchout.zip ::roms/
mcopy -i sd.img punchout.zip ::PORTDATA/Punchout/BASEROMS/
mdir -/ -i sd.img ::                                  # check what is on it
```
`-s 1` matters: FAT32 needs at least 65525 clusters, which a 64 MiB image only reaches with
small clusters. Make a separate image for BattleZone (`cp sd.img bz_sd.img`) and name it in its sidecar. Then copy
the images to the MiSTer, next to the `.um2` in `/media/fat/games/Vectrex-PiTrex/UVMC2/` (with the SD card in your PC, or over the network).

On Windows, the easiest way is **WSL** with the commands above: the image is a normal file that you can
find from Windows under `\\wsl$\...` or in your WSL home. A tool that mounts raw disk images
as drive letters (for example OSFMount) should also work if you format the mounted image
as FAT32 with 512-byte clusters and no partition table; that route has not been tested here.

### Checking a card image
`fsck.vfat -n sd.img` should report no errors, and `mdir -/ -i sd.img ::` should list your zips.
If a game says it cannot find its ROMs, the usual causes are a wrong folder, a misspelt zip name, or a ROM set from a
different MAME version (the games check the files' checksums).

## Tested games
On a DE10-Nano, 26 to 29 October 2026 (the B10 core on 29 October), with the images named below.
Other builds of the same games may behave differently. These are the images used for testing. They were shared in UVMC2 community groups; download links will be added once their authors are happy for us to link them.

| Game (image tested) | Card | How it ran |
|---|---|---|
| Star Wars (`Star_Wars.um2`) | your MAME `starwars` + `starwar1` zips in `roms/` | music and speech good; about 40 frames/s; whole picture with Open Matte |
| BattleZone 0.3a (`BattleZone_0_3a.um2`) | its **own** copy, no ROMs (it writes to the card) | plays well, cart audio good; about 23 frames/s; whole picture with Open Matte. Button 1 starts, button 4 fires |
| Pac-Man (`pacman-uvmc2-SD-card.um2`) | a card image with the folders, no ROMs | cart audio good; about 26 frames/s, game speed right |
| Punch-Out (`Punchout.um2`) | your MAME `punchout.zip` | plays but about two thirds speed (the MiSTer manages ~38 of its 60 frames/s), so sound and speech are a bit off; sound comes through the Vectrex PSG; its JACK audio route is untested |
| Veccy Road 0.10 (`veccy_road_v010.um2`) | none | plays well, with sound, about 39 frames/s |

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
Veccy Road needs no sidecar. `view=open` only works when the core was loaded from
**PiTrex & UVMC2 > UVMC2 Games** (see [Where files go](#where-files-go)); otherwise set OSD > View Area = Open Matte yourself.

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
  know which game to start. In a `[Vectrex-PiTrex]` section it is only on while our core runs under
  that name (the PiTrex & UVMC2 menu entries load it that way); your `[MiSTer]` settings and every
  other core are left as they were. If you use an alternative ini (`MiSTer_alt_1.ini` and so on),
  add the same two lines to that file yourself.
- **Older releases** (B8 and earlier) set `log_file_entry=1` for all cores in `[MiSTer]` and kept a
  backup, `MiSTer.ini.pitrex-bak`. Setup now puts that line back as it was in the backup, then
  deletes the backup.
- **config/Vectrex-PiTrex_v1.CFG:** PiTrex Mode on (our core's own settings file; the official
  core's `VECTREX_v1.CFG` is not used).
- **linux/user-startup.sh:** one line that starts `pitrexd` at boot.
- **Folders:** `games/Vectrex-PiTrex/UVMC2`, `pitrex/uvmc2`, and the menu folder `_PiTrex & UVMC2`.

**PiTrex_Uninstall** undoes all of it (it removes the two MiSTer.ini lines, and the section header
when nothing else is in it) and leaves your own files alone.

## Troubleshooting
| Symptom | What to do |
|---|---|
| Nothing happens after the pick | `cat /tmp/pitrexd.log` over SSH. `REFUSED ... CPUs` means a reboot is needed (BigMig). `ignored a stale file report` is normal after a restart of the MiSTer's menu program. Check OSD > System > PiTrex Mode = On. If `/tmp/pitrexd.log` shows nothing at all for the pick, load the core from **PiTrex & UVMC2 > UVMC2 Games** (a core loaded by hand does not report picks) and check that MiSTer.ini (or the alternative ini you use) has the `[Vectrex-PiTrex]` section. |
| "Missing directory: Vectrex-PiTrex" in the OSD | Run PiTrex_Setup (it creates the folder) or create `games/Vectrex-PiTrex/UVMC2` yourself. |
| The game says ROMS NOT FOUND or MISSING | See [Card images](#card-images-how-to-make-one): folder, name, and a card named in the sidecar. |
| Black screen for a long time | Wait for the LOADING screen to finish. If the screen stays black or the MiSTer's menu stops responding: Scripts > PiTrex_Stop, or SSH `killall um2run`. |
| Static at the start | Expected for the first second or two. Up to about 10 seconds when the pick had to switch View Area (the core reloads first, see `/tmp/pitrexd.log`); the next pick starts clean. |
| The picture is cropped at the sides | Landscape game on a portrait screen. OSD > View Area = Open Matte shows it all, or put `view=open` in the sidecar. |
| An old overlay is on top of the game | OSD > Video Overlay & Geometry > Overlay = Off. |
| The OSD browser opens `games/Vectrex` | The core was loaded by hand from `_Unstable`, so picks will not start. Load it from PiTrex & UVMC2 > UVMC2 Games. |
| A game started again by itself after a restart | Fixed in this release (stale file reports are ignored). |
| Log of the last game | `/tmp/um2.log` (the game), `/tmp/pitrexd.log` (picks and settings). |

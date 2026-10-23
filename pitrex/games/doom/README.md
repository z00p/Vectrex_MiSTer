# DOOM (wireframe) for PiTrex on MiSTer

**Work in progress: not in the release yet.** It runs, but drawing glitches and the layout
still need work. Build it yourself to try it.

Daniel Ferrer's [PureDOOM-pitrex](https://github.com/tullulah/PureDOOM-pitrex): DOOM drawn as
vectors on the Vectrex. It is GPL-2; the DOOM source it is built on is by id Software (GPL).
The two files here are the MiSTer entry point and platform layer (libc instead of the
original's bare-metal code); the renderer is the upstream one, unmodified.

The release ships `doom` pre-built. The only thing you add is the free shareware WAD:
put `doom1.wad` in `/media/fat/games/Vectrex/DOOM/` (it is not included). Saves and the
`.doomrc` are written to the same folder.

## Controls
| Input | In the game | In menus |
|---|---|---|
| Stick up / down | forward / back | move up / down |
| Stick left / right | turn | move left / right |
| Button 1 | fire | select |
| Button 2 | use (doors, switches) | back |
| Button 3 | run | |
| Button 4 | hold to strafe; tap to open the menu | close the menu |

## Settings (environment variables)
| Variable | Meaning |
|---|---|
| `DOOM_YSCALE` | vertical scale of the picture (4000 to 30000, default 13500) |
| `DOOM_YOFF` | vertical offset (default 6000) |
| `DOOM_BRIGHT` | line brightness (1 to 127, default 48) |
| `DOOM_CLASSIC` | `1` = the original PiTrex proportions (scale 24000, offset 0, brightness 64) |
| `DOOM_WAD`, `DOOM_HOME` | WAD file and save folder, if you keep them elsewhere |
| `DOOM_HZ` | vector refresh rate (default 50) |

## Building (developers)
```
git clone https://github.com/tullulah/PureDOOM-pitrex upstream/PureDOOM-pitrex
tools/build_doom.sh arm        # -> build/arm/doom (static ARM binary); needs gcc-arm-linux-gnueabihf
```

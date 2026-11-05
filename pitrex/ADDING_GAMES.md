# Adding UVMC2 games

This page is for you if you have a real UVMC2 cartridge and its SD card. It shows where your
cartridge images (`.um2` files) go, and how to give the games the files they read from the card.
**Nothing is included**: no images, ROM sets or card images. Bring your own.

## Quick answers
- **Do `.um2` files need converting?** No. They run as they are.
- **Where does everything go?** One folder: `/media/fat/games/Vectrex-PiTrex/UVMC2/`. Copy your
  whole UVMC2 SD card there, and add new games there too.
- **Where do the MAME ROMs a game needs go?** In that folder, in the same place as on your real
  card: `roms/` for most games (Punch-Out: `PORTDATA/Punchout/BASEROMS/`).
- **What is the card image?** The games read their files from a card image, `sd.img`, made from
  that folder. You don't handle it: it lives in `/media/fat/pitrex/uvmc2/`, and every game uses it
  unless its settings file names another card.
- **Can I use an image of my real SD card?** Copy its files instead (see the steps). A raw dump of
  the card is untested, and it has a partition table.
- **Why do some games need settings files?** A settings file, `Game.um2.cfg` next to `Game.um2`,
  changes how one game runs. Most games need none: Punch-Out gets its speed by itself, and Open
  Matte, which shows all of the picture in Star Wars and BattleZone, is on by default.

## Steps
1. Install the release and run **Scripts > PiTrex_Setup** once (see [README.md](README.md#install)).
   It creates the folder.
2. Copy everything on your UVMC2 SD card to `/media/fat/games/Vectrex-PiTrex/UVMC2/`, with its
   folders: `roms`, `PORTDATA`, loose files such as `QBERT2.VSM`, and the `.um2` games.
3. Build the card: run **Scripts > PiTrex_Setup** again, or **Scripts > PiTrex_Make_UVMC2_Card**.
   It shows how far it has got. The time depends on the card size: about 25 minutes for 3 GiB of
   files on a DE10-Nano, far less for a card with a few ROM zips. `.uf2` firmware, the menu pictures
   and videos in `ASSETS` folders (a game's sound kept there does go on the card) and macOS files
   (`__MACOSX`, `._*`, `.DS_Store`) are left out.
4. Load the Vectrex-PiTrex core (main menu > Console > Vectrex-PiTrex), then use the OSD: **Load UVMC2** lists your games.

**Or** just put `.um2` files in that folder (subfolders are fine). A game that needs no card
(Veccy Road, the demos) then plays at once. With nothing to put on a card, PiTrex_Setup makes a
blank `sd.img`, so games can still save.

You can copy the files with the MiSTer's SD card in your PC, or over the network.

### Two ways to start a game
- **The OSD:** load the core, then **Load UVMC2** (or **Load PiTrex** for PiTrex programs).
- **An MGL file:** a standard MiSTer shortcut straight to one game. Put it anywhere in the main
  menu:
  ```xml
  <mistergamedescription>
    <rbf>_Console/Vectrex-PiTrex</rbf>
    <file delay="2" type="f" index="3" path="UVMC2/Punchout.um2"/>
  </mistergamedescription>
  ```
  The path is relative to `games/Vectrex-PiTrex`. PiTrex programs work the same way, for example
  `path="PiTrex/gyrocks.ptx"`.

### Changing the card later
A new `.um2` shows up straight away and needs no card update. After adding or removing other files,
a game shows CARD OUT OF DATE for a moment and starts on the current card: run
**Scripts > PiTrex_Make_UVMC2_Card** when you have time.
- Your saves are kept: everything the games wrote to the old card (settings, high scores) is
  copied to the new one. The old card stays as `sd.img.bak` until the next rebuild.
- A file you remove from the folder is left off the new card. The first time the builder
  replaces a card you made some other way, it keeps every file of that card that is not in the
  folder.
- The card is not rebuilt while a game is running.
- If something goes wrong (not enough free space, for example), the old card is left as it was.
- The card is at least 64 MiB, and bigger if your files need it, up to 16000 MiB.

## Updating from an older release
- After **Scripts > PiTrex_Update**, run **Scripts > PiTrex_Setup** once.
- This core now uses `[Vectrex-PiTrex]` in `MiSTer.ini`, not `[Vectrex]`. PiTrex_Setup copies your
  `[Vectrex]` settings across, so the picture stays as you had it.
- If `pitrex/uvmc2/card/` has your files in it, it keeps working as before: the builder uses it
  and copies games from it to the UVMC2 folder. Cards and settings files that already work are
  not touched. To move to the one folder, move the files from `pitrex/uvmc2/card/` to
  `games/Vectrex-PiTrex/UVMC2/`, leaving nothing but `README.txt` behind, and build the card.
- The menu entries are rewritten as standard MGLs. PiTrex programs move to
  `games/Vectrex-PiTrex/PiTrex`.
- From 1.0 the core is in `_Console` (the alphas and betas put it in `_Unstable`); the old copies
  are removed and the menu entries follow. An MGL file of your own that says
  `<rbf>_Unstable/Vectrex-PiTrex</rbf>` needs `_Console/Vectrex-PiTrex` instead.
- From 1.0 the core keeps its settings in `config/Vectrex-PiTrex_v2.CFG`, so OSD options start
  from their defaults; PiTrex_Setup sets PiTrex Mode and Open Matte again.

## Which games need a card
These are the games tested on the MiSTer (see [UVMC2_GAMES.md](UVMC2_GAMES.md#tested-games)).

| Game | Needs a card? | Files on the card |
|---|---|---|
| Star Wars | yes | your MAME `starwars.zip` and `starwar1.zip` in `roms/` |
| Punch-Out | yes | your MAME `punchout.zip` in `PORTDATA/Punchout/BASEROMS/` |
| Pac-Man | yes, for settings and scores | none: it writes `config/` and `scores/` itself |
| BattleZone | yes, for settings and scores | none: the game is all in the image |
| Vector Tank | yes | the game's `PORTDATA/vectortank/` folder from its download, with `ASSETS/` (its music) |
| Veccy Road, the demos | no | |

- **Star Wars** takes each ROM from whichever zip has it, so the names of the files inside do not
  matter. Without `starwar1.zip` it stops on its title screen with the words ROMS NO.
- **Punch-Out** runs at 60 Hz, `frame_cycles=25000`. Any image with "punch" in its name gets that
  without a settings file (see [Sidecar settings](UVMC2_GAMES.md#sidecar-settings)).
- **BattleZone** writes to its card and shares `sd.img` with the other games, as it shares a real
  card. A `bz_sd.img` from an older release keeps working while its settings file names it.
- **Other games:** if a game says ROMS NOT FOUND, ROMS NO or MISSING, it wants files from your
  card. Copy the whole card to the UVMC2 folder and build it again.

## Advanced
- **Settings files (sidecars).** A file `Game.um2.cfg` next to `Game.um2` holds per-game settings,
  one `key=value` per line. The key `sd=name.img` gives the game another card. A name without a
  folder is looked up next to the `.um2`, then in `/media/fat/pitrex/uvmc2/`, then in
  `/media/fat/pitrex/um2run/`. All keys are in [UVMC2_GAMES.md](UVMC2_GAMES.md#sidecar-settings).
- **A card for one game.** Copy `sd.img` to a new name in `/media/fat/pitrex/uvmc2/` (for example
  `mygame_sd.img`) and put `sd=mygame_sd.img` in that game's settings file. The card builder only
  rebuilds `sd.img`, so a copy like this keeps the files it had.
- **Over SSH:** `/media/fat/pitrex/make_card.sh` builds the card (as root);
  `make_card.sh --if-needed` builds it only when the folder has changed, as Setup does;
  `make_card.sh --check` only tells you, with exit status 1 if the card is out of date.
  `sd.img.list` next to the card lists the files it was built from; the builder uses it to tell
  your files from the ones the games wrote, so leave it next to the card.
- **Building a card on a PC** (Linux or WSL with mtools): see
  [UVMC2_GAMES.md](UVMC2_GAMES.md#advanced-build-a-card-on-a-pc). Put the image in
  `/media/fat/pitrex/uvmc2/` as `sd.img`, or under another name given in a settings file. A card
  you made this way is only rebuilt by Setup if you put newer files in the UVMC2 folder.
- **PiTrex_Uninstall** leaves your files alone: the UVMC2 folder, the card images and their backups,
  your `.um2` files and their settings files, and anything of yours in `pitrex/um2run/`.

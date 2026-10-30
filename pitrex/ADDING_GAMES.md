# Adding UVMC2 games (alpha)

This page is for you if you have a real UVMC2 cartridge and its SD card. It shows where your
cartridge images (`.um2` files) go, and how to give the games the files they read from the card.
No images, ROM sets or card images come with this release: bring your own.

Some games are a `.um2` file and nothing else. Others read ROM sets from the cartridge's SD card,
or keep settings and high scores on it. On the MiSTer that card is a single file, a **card image**.
You don't make it yourself: you copy the files from your real card to a folder, and PiTrex_Setup
builds the card image from them.

## Steps
1. Install the release and run **Scripts > PiTrex_Setup** once (see [README.md](README.md#install)).
   It creates the folders below.
2. Copy your `.um2` files to `/media/fat/games/Vectrex-PiTrex/UVMC2/`. Subfolders are fine.
3. Copy the files from your UVMC2 SD card to `/media/fat/pitrex/uvmc2/card/`, keeping the
   folders as they are (`roms`, `PORTDATA` and loose files such as `QBERT2.VSM`). You can copy
   the whole card: `.um2` and `.uf2` files, `ASSETS` folders (menu pictures) and macOS files
   (`__MACOSX`, `._*`, `.DS_Store`) are left out. If you only have ROM zips, put them in
   `card/roms/` (see the table below).
4. Run **Scripts > PiTrex_Setup** again, or **Scripts > PiTrex_Make_UVMC2_Card**. Either one
   builds the card image `/media/fat/pitrex/uvmc2/sd.img` from the folder, one line per step.
   Every game uses this card unless its settings name another one.
5. Main menu > **PiTrex & UVMC2** > **UVMC2 Games**, then OSD > **Load UVMC2 / PiTrex** and pick
   the `.um2`.

You can copy the files with the MiSTer's SD card in your PC, or over the network.

### Changing the card later
Add or remove files in `pitrex/uvmc2/card/`, then run PiTrex_Setup or PiTrex_Make_UVMC2_Card
again. Setup only rebuilds the card when the folder has changed; PiTrex_Make_UVMC2_Card always
does.
- Your saves are kept: everything the games wrote to the old card (settings, high scores) is
  copied to the new one. The old card stays as `sd.img.bak` until the next rebuild.
- A file you remove from the card folder is left off the new card. The first time the builder
  replaces a card you made some other way, it keeps every file of that card that is not in the
  card folder.
- Stop a running game first (**Scripts > PiTrex_Stop**). The card is not rebuilt while a game
  is running.
- If something goes wrong (not enough free space, for example), the old card is left as it was.
- The card is at least 64 MiB, and bigger if your files need it. A card over 2000 MiB is not
  made (the games cannot read card images of 2 GiB or more yet): remove files the games don't
  need, such as ROM sets for games you don't have.

## Which games need a card
These are the games tested on the MiSTer (see [UVMC2_GAMES.md](UVMC2_GAMES.md#tested-games)).

| Game | Needs a card? | Files on the card | Own copy? |
|---|---|---|---|
| Star Wars | yes | your MAME `starwars.zip` and `starwar1.zip` in `roms/` | no |
| Punch-Out | yes | your MAME `punchout.zip` in `PORTDATA/Punchout/BASEROMS/` | no |
| Pac-Man | yes, for settings and scores | none: it writes `config/` and `scores/` itself | no |
| BattleZone | yes, for settings and scores | none: the game is all in the image | yes, `bz_sd.img` (made for you, see below) |
| Veccy Road, the demos | no | | |

- **Star Wars** takes each ROM from whichever zip has it, so the names of the files inside do not
  matter. Without `starwar1.zip` it stops on its title screen with the words ROMS NO.
- **Punch-Out** also wants a settings file next to its image, `Punchout.um2.cfg`, with the line
  `frame_cycles=25000` (see [Sidecar settings](UVMC2_GAMES.md#sidecar-settings)).
- **BattleZone** writes to its card, so it gets its own copy. When a BattleZone image (a name
  like `BattleZone_0_3a.um2`) is in the UVMC2 folder, the card builder also makes `bz_sd.img`,
  with the same files and BattleZone's own saves. If the image has no settings file yet, it
  writes one, `<image>.um2.cfg`, with the line `sd=bz_sd.img`. It never changes a settings file
  you already have: it tells you what it found, and you can add `sd=bz_sd.img` yourself.
- **Other games:** if a game says ROMS NOT FOUND, ROMS NO or MISSING, it wants files from your
  card. Copy the whole card to `pitrex/uvmc2/card/` and build it again.

## Advanced
- **Settings files (sidecars).** A file `Game.um2.cfg` next to `Game.um2` holds per-game settings,
  one `key=value` per line. The key `sd=name.img` gives the game another card. A name without a
  folder is looked up next to the `.um2`, then in `/media/fat/pitrex/uvmc2/`, then in
  `/media/fat/pitrex/um2run/`. All keys are in [UVMC2_GAMES.md](UVMC2_GAMES.md#sidecar-settings).
- **A card for one game.** Copy `sd.img` to a new name in `/media/fat/pitrex/uvmc2/` (for example
  `mygame_sd.img`) and put `sd=mygame_sd.img` in that game's settings file. The card builder only
  rebuilds `sd.img` and `bz_sd.img`, so a copy like this keeps the files it had.
- **Over SSH:** `/media/fat/pitrex/make_card.sh` builds the card (as root);
  `make_card.sh --if-needed` builds it only when the folder has changed, as Setup does.
  `sd.img.list` next to the card lists the files it was built from; the builder uses it to tell
  your files from the ones the games wrote, so leave it next to the card.
- **Building a card on a PC** (Linux or WSL with mtools): see
  [UVMC2_GAMES.md](UVMC2_GAMES.md#advanced-build-a-card-on-a-pc). Put the image in
  `/media/fat/pitrex/uvmc2/` as `sd.img`, or under another name given in a settings file. A card
  you made this way is only rebuilt by Setup if you put newer files in the card folder.
- **PiTrex_Uninstall** leaves your files alone: the card folder, the card images and their backups,
  your `.um2` files and their settings files.

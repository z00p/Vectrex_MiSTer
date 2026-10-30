# Adding UVMC2 games (alpha)

This page is for you if you have a real UVMC2 cartridge and its SD card. It shows where your
cartridge images (`.um2` files) go, and how to give the games the files they read from the card.
**Nothing is included**: no images, ROM sets or card images. Bring your own.

## Quick answers
- **Do `.um2` files need converting?** No. They run as they are.
- **Are `.um2` files kept loose, or inside a card image?** Loose, in
  `/media/fat/games/Vectrex-PiTrex/UVMC2/`. The card image holds what your cartridge's SD card
  holds apart from the games: ROM sets, settings and high scores.
- **Where do the MAME ROMs a game needs go?** In the card folder, in the same place as on your
  real card: `roms/` for most games (Punch-Out: `PORTDATA/Punchout/BASEROMS/`). They end up
  inside the card image.
- **What is the card image called, and where does it go?** `sd.img`, in
  `/media/fat/pitrex/uvmc2/`. You don't make it: PiTrex_Setup builds it for you. Every game uses
  it unless its settings file names another card.
- **Can I use an image of my real SD card?** Copy its files instead (see the steps). A raw dump of
  the card is untested, it has a partition table, and images of 2 GiB or more do not work yet.
- **Why do some games need settings files?** A settings file, `Game.um2.cfg` next to `Game.um2`,
  changes how one game runs. BattleZone gets its own card, and its settings file is written for
  you automatically. Punch-Out needs `frame_cycles=25000` for its speed. `view=open` shows all of
  the picture in Star Wars and BattleZone. Most other games need none.

## Steps
1. Install the release and run **Scripts > PiTrex_Setup** once (see [README.md](README.md#install)).
   It creates the folders below.
2. Copy everything on your UVMC2 SD card to `/media/fat/pitrex/uvmc2/card/`, with its folders:
   `roms`, `PORTDATA`, loose files such as `QBERT2.VSM`, and the `.um2` games.
3. Run **Scripts > PiTrex_Setup** again (or **Scripts > PiTrex_Make_UVMC2_Card**). It:
   - copies the `.um2` games to `/media/fat/games/Vectrex-PiTrex/UVMC2/`, keeping their
     subfolders. It never overwrites a file there: if a different file of the same name is
     already there, it leaves it alone and tells you.
   - builds the card image `/media/fat/pitrex/uvmc2/sd.img` from the rest, one line per step.
     `.uf2` firmware, `ASSETS` folders (menu pictures) and macOS files (`__MACOSX`, `._*`,
     `.DS_Store`) are left out.
4. Main menu > **PiTrex & UVMC2** > **UVMC2 Games**, then OSD > **Load UVMC2 / PiTrex** and pick
   the `.um2`. Always start from that menu entry: if you load the core any other way, Mine Storm
   stays on screen and picking a game does nothing.

**Or** put your `.um2` files straight in `/media/fat/games/Vectrex-PiTrex/UVMC2/` (subfolders are
fine). A game that needs no card (Veccy Road, the demos) then plays at once. If you only have ROM
zips, put them in `pitrex/uvmc2/card/roms/` and run PiTrex_Setup.

You can copy the files with the MiSTer's SD card in your PC, or over the network. The games stay
in the card folder as well: to remove a game for good, delete it in both places.

### Changing the card later
Add or remove files in `pitrex/uvmc2/card/`, then run PiTrex_Setup or PiTrex_Make_UVMC2_Card
again. Setup only rebuilds the card when the folder has changed; PiTrex_Make_UVMC2_Card always
does. New `.um2` games in the folder are copied either way.
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

## Updating from the first alpha
- After **Scripts > PiTrex_Update**, run **Scripts > PiTrex_Setup** once.
- Cards and settings files that already work are not touched until you put files in
  `pitrex/uvmc2/card/` and build a card.
- When you do: if BattleZone's settings file already names `bz_sd.img`, that card is rebuilt
  where it is. Its saves are kept, and the old one stays as `bz_sd.img.bak`. `sd.img` is
  rebuilt the same way, keeping your old card as `sd.img.bak`.

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
  you already have: if yours names another card, `bz_sd.img` is not made and it tells you what
  it found. To use `bz_sd.img`, put `sd=bz_sd.img` in the file yourself and build again.
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
  your `.um2` files and their settings files, and anything of yours in `pitrex/um2run/`.

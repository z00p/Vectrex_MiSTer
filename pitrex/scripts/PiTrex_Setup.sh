#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One-time setup for the main-menu folder "PiTrex & UVMC2" (run again after adding programs):
#  - PiTrex Mode On in config/Vectrex-PiTrex_v2.CFG (our core's own settings file)
#  - log_file_entry=1 for our core only, in a [Vectrex-PiTrex] section of MiSTer.ini (Main then reports the file an OSD pick
#    or an MGL loads; the [MiSTer] settings and other cores are left alone; see pitrex/pitrex_ini.py)
#  - the folder games/Vectrex-PiTrex/UVMC2: the core's OSD file browser opens games/<core name>, so UVMC2 games (.um2)
#    and everything from a UVMC2 SD card go there; PiTrex programs (.ptx) in games/Vectrex-PiTrex/PiTrex
#  - pitrexd, which starts the program or game a menu entry or an OSD pick selects: now and at every boot
#  - builds the menu entries (make_menu.sh)
#  - the UVMC2 card image pitrex/uvmc2/sd.img built from that folder (make_card.sh) when it has card files and the
#    card is missing or older
# Undo with PiTrex_Uninstall.
FAT=/media/fat
PITREX=$FAT/pitrex
INI=$FAT/MiSTer.ini
START=$FAT/linux/user-startup.sh

python3 - <<'PYEOF'
# PiTrex Mode = OSD status bit 117 (byte 14, bit 5). Our core loads as "Vectrex-PiTrex" (the MGL's
# setname), so its settings live in their own file, Vectrex-PiTrex_v2.CFG since B13 (Videodr0me's
# VECTREX_v2 layout). Clear the bit older releases set (status bit 36, byte 4 bit 4) in the
# official core's VECTREX_v1.CFG.
def setbit(path, i, m, on):
    try:
        b = bytearray(open(path, "rb").read())
    except OSError:
        if not on:
            return
        b = bytearray()
    b += bytes(max(0, 16 - len(b)))
    new = (b[i] | m) if on else (b[i] & ~m)
    if new != b[i]:
        b[i] = new
        open(path, "wb").write(b)
setbit("/media/fat/config/Vectrex-PiTrex_v2.CFG", 14, 0x20, True)
setbit("/media/fat/config/VECTREX_v1.CFG", 4, 0x10, False)
PYEOF

mkdir -p "$FAT/games/Vectrex-PiTrex/UVMC2" "$PITREX/uvmc2"
# the one folder: make_card.sh builds the card image pitrex/uvmc2/sd.img from it (written once, so its date stays old)
if [ ! -f "$FAT/games/Vectrex-PiTrex/UVMC2/README.txt" ]; then
	cat > "$FAT/games/Vectrex-PiTrex/UVMC2/README.txt" <<'EOF'
Copy everything from your UVMC2 SD card here, with its folders (roms, PORTDATA, loose
files such as QBERT2.VSM and the .um2 games), and add new .um2 games here too.
The OSD lists the games. The other files go on the card image the games see: run
Scripts > PiTrex_Make_UVMC2_Card after adding them. Saves on the card are kept.
.um2 and .uf2 files, ASSETS folders and macOS files are not put on the card.
See ADDING_GAMES.md in the PiTrex guide.
(This file is not copied to the card.)
EOF
fi
chmod +x "$PITREX"/*.sh "$PITREX/um2run/um2run" "$PITREX/um2run/mister_run.sh" 2>/dev/null
if [ ! -f "$PITREX/um2run/um2run" ]; then
	echo "Note: $PITREX/um2run/um2run is missing, so .um2 games cannot start (unzip the release again)."
fi

INI_OK=1
if ! python3 "$PITREX/pitrex_ini.py" setup "$INI"; then
	INI_OK=0
	echo "PROBLEM: MiSTer.ini could not be changed (see the error above), so picking a .um2 game or a .ptx program"
	echo "in the OSD will not start it. Check that the SD card is not full or read-only, then run PiTrex_Setup again."
	echo "Or add these two lines at the end of $INI yourself:  [Vectrex-PiTrex]  and  log_file_entry=1"
fi

if ! grep -q pitrexd "$START" 2>/dev/null; then
	printf '\n# PiTrex: start programs picked in the Vectrex OSD\n[ -x %s/pitrexd.sh ] && %s/pitrexd.sh &\n' "$PITREX" "$PITREX" >> "$START"
	echo "user-startup.sh: pitrexd added"
fi
if ! ps | grep -v grep | grep -q pitrexd.sh; then
	nohup "$PITREX/pitrexd.sh" > /dev/null 2>&1 &
	echo "pitrexd started"
fi

"$PITREX/make_menu.sh"

# UVMC2 card: build pitrex/uvmc2/sd.img when the card folder has files and the card is missing or older
# (make_card.sh --if-needed says nothing when there is nothing to do)
if [ -f "$PITREX/make_card.sh" ]; then
	bash "$PITREX/make_card.sh" --if-needed || echo "The UVMC2 card was not made (see above). Games that need no card still play."
fi
if [ "$INI_OK" = 1 ]; then
	echo "Done: main menu > PiTrex & UVMC2. UVMC2 games (.um2) go in games/Vectrex-PiTrex/UVMC2."
else
	echo "Setup is not complete: MiSTer.ini could not be changed (see PROBLEM above). The PiTrex & UVMC2 menu entries"
	echo "work, but OSD picks of .um2 games and .ptx programs will not start until it is fixed."
	exit 1
fi

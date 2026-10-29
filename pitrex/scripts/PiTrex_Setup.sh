#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One-time setup for the main-menu folder "PiTrex & UVMC2" (run again after adding programs):
#  - PiTrex Mode On in config/Vectrex-PiTrex_v1.CFG (our core's own settings file)
#  - log_file_entry=1 for our core only, in a [Vectrex-PiTrex] section of MiSTer.ini (Main then reports the file an OSD pick
#    or an MGL loads; the [MiSTer] settings and other cores are left alone; see pitrex/pitrex_ini.py)
#  - the folder games/Vectrex-PiTrex/UVMC2: the core's OSD file browser ("Load UVMC2 / PiTrex") opens games/<core name>,
#    so put UVMC2 games (.um2) and PiTrex programs (.ptx) there
#  - pitrexd, which starts the program or game a menu entry or an OSD pick selects: now and at every boot
#  - builds the menu entries (make_menu.sh)
# Undo with PiTrex_Uninstall.
FAT=/media/fat
PITREX=$FAT/pitrex
INI=$FAT/MiSTer.ini
START=$FAT/linux/user-startup.sh

python3 - <<'PYEOF'
# PiTrex Mode = OSD status bit 36 (byte 4, bit 4). Our core loads as "Vectrex-PiTrex" (the MGL's
# setname), so its settings live in their own file; clear the bit older releases set in the
# official core's VECTREX_v1.CFG.
def setbit(path, on):
    try:
        b = bytearray(open(path, "rb").read())
    except OSError:
        if not on:
            return
        b = bytearray()
    b += bytes(max(0, 16 - len(b)))
    new = (b[4] | 0x10) if on else (b[4] & ~0x10)
    if new != b[4]:
        b[4] = new
        open(path, "wb").write(b)
setbit("/media/fat/config/Vectrex-PiTrex_v1.CFG", True)
setbit("/media/fat/config/VECTREX_v1.CFG", False)
PYEOF

mkdir -p "$FAT/games/Vectrex-PiTrex/UVMC2" "$PITREX/uvmc2"
chmod +x "$PITREX"/*.sh "$PITREX/um2run/um2run" "$PITREX/um2run/mister_run.sh" 2>/dev/null
if [ ! -f "$PITREX/um2run/um2run" ]; then
	echo "Note: $PITREX/um2run/um2run is missing, so .um2 games cannot start (unzip the release again)."
fi

python3 "$PITREX/pitrex_ini.py" setup "$INI"

if ! grep -q pitrexd "$START" 2>/dev/null; then
	printf '\n# PiTrex: start programs picked in the Vectrex OSD\n[ -x %s/pitrexd.sh ] && %s/pitrexd.sh &\n' "$PITREX" "$PITREX" >> "$START"
	echo "user-startup.sh: pitrexd added"
fi
if ! ps | grep -v grep | grep -q pitrexd.sh; then
	nohup "$PITREX/pitrexd.sh" > /dev/null 2>&1 &
	echo "pitrexd started"
fi

"$PITREX/make_menu.sh"
echo "Done: main menu > PiTrex & UVMC2. UVMC2 games (.um2) go in games/Vectrex-PiTrex/UVMC2."

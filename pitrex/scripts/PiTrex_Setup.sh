#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One-time setup for the main-menu folder "PiTrex & UVMC2" (run again after adding programs):
#  - PiTrex Mode On in config/Vectrex-PiTrex_v1.CFG (our core's own settings file)
#  - log_file_entry=1 in MiSTer.ini (Main reports the file an MGL loads; backup kept)
#  - pitrexd, which starts the program a menu entry picks: now and at every boot
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

if grep -q '^log_file_entry=1' "$INI"; then
	echo "MiSTer.ini: log_file_entry already 1"
else
	cp "$INI" "$INI.pitrex-bak"
	sed -i 's/^log_file_entry=0/log_file_entry=1/' "$INI"
	grep -q '^log_file_entry=1' "$INI" || printf '\nlog_file_entry=1\n' >> "$INI"
	echo "MiSTer.ini: log_file_entry=1 (backup MiSTer.ini.pitrex-bak)"
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
echo "Done: main menu > PiTrex & UVMC2."

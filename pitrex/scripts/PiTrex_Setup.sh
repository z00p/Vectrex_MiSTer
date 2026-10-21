#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One-time setup for the main-menu folder "PiTrex & UVMC2" (run again after adding programs):
#  - PiTrex Mode On in config/VECTREX_v1.CFG (harmless for cartridges: the core only listens)
#  - log_file_entry=1 in MiSTer.ini (Main reports the file an MGL loads; backup kept)
#  - pitrexd, which starts the program a menu entry picks: now and at every boot
#  - builds the menu entries (make_menu.sh)
# Undo with PiTrex_Uninstall.
FAT=/media/fat
PITREX=$FAT/pitrex
INI=$FAT/MiSTer.ini
START=$FAT/linux/user-startup.sh

python3 - <<'EOF'
p = "/media/fat/config/VECTREX_v1.CFG"
try:
    b = bytearray(open(p, "rb").read())
except OSError:
    b = bytearray()
b += bytes(max(0, 16 - len(b)))
if not b[4] & 0x10:
    b[4] |= 0x10
    open(p, "wb").write(b)
    print("PiTrex Mode set On")
EOF

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

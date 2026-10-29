#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Remove PiTrex-on-MiSTer: the programs, the PiTrex cores in _Unstable, the overlays it
# installed, its Scripts entries (this one last) and, if present, the optional OSD-loader
# setup (pitrexd line in linux/user-startup.sh, log_file_entry from the MiSTer.ini backup).
# Your Vectrex settings (config/VECTREX_v1.CFG) are left alone; PiTrex's own
# (config/Vectrex-PiTrex_v1.CFG) go. Your own files are NOT touched: pitrex/uvmc2/ (card images,
# the Tac/Scan romset) and games/Vectrex-PiTrex (.um2 images, sidecars, card images); those folders
# are only removed when nothing of yours is left in them.
[ -x /media/fat/Scripts/PiTrex_Stop.sh ] && /media/fat/Scripts/PiTrex_Stop.sh > /dev/null
for p in $(ps | grep -v grep | grep pitrexd.sh | awk '{print $1}'); do kill "$p"; done

find /media/fat/pitrex -mindepth 1 -maxdepth 1 ! -name uvmc2 -exec rm -rf {} + 2>/dev/null
rm -f /media/fat/pitrex/uvmc2/tacscan/README.txt
rmdir /media/fat/pitrex/uvmc2/tacscan /media/fat/pitrex/uvmc2 /media/fat/pitrex 2>/dev/null
rm -f /media/fat/games/Vectrex/PiTrex/*.ptx
rmdir /media/fat/games/Vectrex/PiTrex 2>/dev/null
rm -rf "/media/fat/_PiTrex & UVMC2"
rm -f /media/fat/games/Vectrex-PiTrex/UVMC2/README.txt
rmdir /media/fat/games/Vectrex-PiTrex/UVMC2 /media/fat/games/Vectrex-PiTrex 2>/dev/null
rm -f /media/fat/_Unstable/Vectrex_PiTrex_B*.rbf /media/fat/_Unstable/Vectrex-PiTrex_*.rbf
rm -f /media/fat/config/Vectrex-PiTrex_v1.CFG
rm -f /media/fat/games/Vectrex/Overlays/VecBreak.art /media/fat/games/Vectrex/Overlays/VecDrive.art /media/fat/games/Vectrex/Overlays/TacScan.art /media/fat/games/Vectrex/Overlays/GongRider.art

START=/media/fat/linux/user-startup.sh
if grep -q pitrexd "$START" 2>/dev/null; then
	sed -i '/# PiTrex: start programs picked in the Vectrex OSD/d; /pitrexd/d' "$START"
	echo "Removed pitrexd from user-startup.sh"
fi
INI=/media/fat/MiSTer.ini
if [ -f "$INI.pitrex-bak" ]; then
	if grep -q '^log_file_entry=0' "$INI.pitrex-bak"; then
		sed -i 's/^log_file_entry=1/log_file_entry=0/' "$INI"
		echo "MiSTer.ini: log_file_entry back to 0"
	elif ! grep -q '^log_file_entry=' "$INI.pitrex-bak"; then
		sed -i '/^log_file_entry=1$/d' "$INI"
		echo "MiSTer.ini: removed the log_file_entry line Setup added"
	fi
	rm -f "$INI.pitrex-bak"
fi

echo "PiTrex removed."
[ -d /media/fat/games/Vectrex-PiTrex ] && echo "Your files in games/Vectrex-PiTrex were left in place."
[ -d /media/fat/pitrex/uvmc2 ] && echo "Your files in pitrex/uvmc2 (card images, romsets) were left in place."
rm -f /media/fat/Scripts/PiTrex_*.sh /media/fat/Scripts/UVMC2_*.sh

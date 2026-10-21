#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Remove PiTrex-on-MiSTer: the programs, the PiTrex cores in _Unstable, the overlays it
# installed, its Scripts entries (this one last) and, if present, the optional OSD-loader
# setup (pitrexd line in linux/user-startup.sh, log_file_entry from the MiSTer.ini backup).
# Your Vectrex settings (config/VECTREX_v1.CFG) are left alone.
[ -x /media/fat/Scripts/PiTrex_Stop.sh ] && /media/fat/Scripts/PiTrex_Stop.sh > /dev/null
for p in $(ps | grep -v grep | grep pitrexd.sh | awk '{print $1}'); do kill "$p"; done

rm -rf /media/fat/pitrex /media/fat/games/Vectrex/PiTrex
rm -f /media/fat/_Unstable/Vectrex_PiTrex_B*.rbf
rm -f /media/fat/games/Vectrex/Overlays/VecBreak.art /media/fat/games/Vectrex/Overlays/VecDrive.art

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
	fi
	rm -f "$INI.pitrex-bak"
fi

echo "PiTrex removed."
rm -f /media/fat/Scripts/PiTrex_*.sh

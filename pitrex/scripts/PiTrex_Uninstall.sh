#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Remove PiTrex-on-MiSTer: the programs, the PiTrex cores in _Unstable, the overlays it
# installed, its Scripts entries (this one last) and the OSD-loader setup (pitrexd line in
# linux/user-startup.sh, log_file_entry=1 in the [Vectrex-PiTrex] section of MiSTer.ini).
# Your Vectrex settings (config/VECTREX_v1.CFG) are left alone; PiTrex's own
# (config/Vectrex-PiTrex_v1.CFG) go. Your own files are NOT touched: pitrex/uvmc2/ (card images,
# the card folder, the Tac/Scan romset), anything of yours in pitrex/um2run/ (card images named by
# sd=, images, sidecars) and games/Vectrex-PiTrex (.um2 images, sidecars, card images); those
# folders are only removed when nothing of yours is left in them.
[ -x /media/fat/Scripts/PiTrex_Stop.sh ] && /media/fat/Scripts/PiTrex_Stop.sh > /dev/null
for p in $(ps | grep -v grep | grep pitrexd.sh | awk '{print $1}'); do kill "$p"; done

# MiSTer.ini first, while the helper is still there (it also undoes the [MiSTer] change of older releases)
INI=/media/fat/MiSTer.ini
if [ -f /media/fat/pitrex/pitrex_ini.py ]; then
	if ! python3 /media/fat/pitrex/pitrex_ini.py uninstall "$INI"; then
		echo "PROBLEM: MiSTer.ini could not be changed (see the error above), so the lines [Vectrex-PiTrex] and"
		echo "log_file_entry=1 that PiTrex_Setup added may still be in it. They only affect the Vectrex-PiTrex core,"
		echo "which is being removed, so they do no harm; to tidy up, delete them from $INI yourself."
	fi
elif [ -f "$INI.pitrex-bak" ]; then
	# an older release without the helper: log_file_entry back as it was in its backup
	if grep -q '^log_file_entry=0' "$INI.pitrex-bak"; then
		sed -i 's/^log_file_entry=1/log_file_entry=0/' "$INI"
		echo "MiSTer.ini: log_file_entry back to 0"
	fi
	rm -f "$INI.pitrex-bak"
fi

find /media/fat/pitrex -mindepth 1 -maxdepth 1 ! -name uvmc2 ! -name um2run -exec rm -rf {} + 2>/dev/null
# pitrex/um2run: only the release's own files. sd= card names are looked up there too, so it can hold your card
# images (BattleZone's saves), .um2 images, sidecars and .load files; those, and um2run.* copies, stay.
rm -f /media/fat/pitrex/um2run/um2run /media/fat/pitrex/um2run/mister_run.sh \
	/media/fat/pitrex/um2run/README.md /media/fat/pitrex/um2run/DESIGN.md
rmdir /media/fat/pitrex/um2run 2>/dev/null
# TACSCAN.PCM is made by make_menu.sh from MAME's tacscan samples (made again by the next Setup)
rm -f /media/fat/pitrex/uvmc2/tacscan/README.txt /media/fat/pitrex/uvmc2/tacscan/TACSCAN.PCM /media/fat/pitrex/uvmc2/card/README.txt
rmdir /media/fat/pitrex/uvmc2/tacscan /media/fat/pitrex/uvmc2/card /media/fat/pitrex/uvmc2 /media/fat/pitrex 2>/dev/null
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
	# Setup appended a blank line, the comment and the start line: remove all three. The blank line goes only if
	# it is blank and right before the comment (a file without a final newline got no blank line, so the line
	# before the comment is then the user's own and stays).
	awk '
		/^# PiTrex: start programs picked in the Vectrex OSD/ { if (have && (prev == "" || prev == "\r")) have = 0; next }
		/pitrexd/ { next }
		{ if (have) print prev; prev = $0; have = 1 }
		END { if (have) print prev }
	' "$START" > "$START.pitrex-tmp" && mv "$START.pitrex-tmp" "$START"
	rm -f "$START.pitrex-tmp"
	# nothing left: Setup made the file
	[ -s "$START" ] || rm -f "$START"
	echo "Removed pitrexd from user-startup.sh"
fi
echo "PiTrex removed."
[ -d /media/fat/games/Vectrex-PiTrex ] && echo "Your files in games/Vectrex-PiTrex were left in place."
[ -d /media/fat/pitrex/uvmc2 ] && echo "Your files in pitrex/uvmc2 (card images, the card folder, romsets) were left in place."
[ -d /media/fat/pitrex/um2run ] && echo "Your files in pitrex/um2run (card images, images, sidecars) were left in place."
rm -f /media/fat/Scripts/PiTrex_*.sh /media/fat/Scripts/UVMC2_*.sh

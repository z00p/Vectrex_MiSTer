#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Download the newest PiTrex-on-MiSTer release from GitHub and unpack it onto the SD card.
# Only the release's own files are written: the core in _Unstable, /media/fat/pitrex,
# the PiTrex_*.sh Scripts entries and the overlays in games/Vectrex/Overlays.
REPO=z00p/Vectrex_MiSTer
CA="--cacert /etc/ssl/certs/cacert.pem"
TMP=/tmp/pitrex_update.zip

echo "Looking for the latest PiTrex release..."
url=$(curl -fsSL $CA "https://api.github.com/repos/$REPO/releases" \
	| grep -o '"browser_download_url": *"[^"]*PiTrex_MiSTer[^"]*\.zip"' | head -1 \
	| sed 's/.*"\(https[^"]*\)"$/\1/')
if [ -z "$url" ]; then
	echo "No release found (no network, or GitHub unreachable)."
	exit 1
fi
echo "Downloading $(basename "$url")"
curl -fsSL $CA -o "$TMP" "$url" || { echo "Download failed."; exit 1; }

# stop a running PiTrex program first, so its binary can be replaced
[ -x /media/fat/Scripts/PiTrex_Stop.sh ] && /media/fat/Scripts/PiTrex_Stop.sh > /dev/null

unzip -o -q "$TMP" -d /media/fat || { echo "Unpacking failed."; exit 1; }
rm -f "$TMP"
chmod +x /media/fat/pitrex/* /media/fat/pitrex/um2run/um2run /media/fat/pitrex/um2run/mister_run.sh /media/fat/Scripts/PiTrex_*.sh 2>/dev/null
mkdir -p /media/fat/games/Vectrex-PiTrex/UVMC2 /media/fat/pitrex/uvmc2
# games now start from the main-menu folder: drop the per-game Scripts entries of B6 and older
rm -f /media/fat/Scripts/PiTrex_Cube_demo.sh /media/fat/Scripts/PiTrex_Gyrocks.sh \
	/media/fat/Scripts/PiTrex_Selftest.sh /media/fat/Scripts/PiTrex_VecBreak.sh \
	/media/fat/Scripts/PiTrex_VecDrive.sh /media/fat/Scripts/PiTrex_VecDrive_v2.sh \
	/media/fat/Scripts/UVMC2_Hello.sh /media/fat/Scripts/UVMC2_TacScan.sh
# the main-menu entries name the core file: point them at the newest one
[ -d "/media/fat/_PiTrex & UVMC2" ] && bash /media/fat/pitrex/make_menu.sh > /dev/null
echo "Updated. The launcher and the menu use the newest core (from B8 named Vectrex-PiTrex_<date>.rbf; the older _B<n>_ files are removed)."

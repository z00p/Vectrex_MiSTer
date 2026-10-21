#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# UVMC2: Tac/Scan (Sega 1982), the UVMC2 team's port. Needs your own romset at
# /media/fat/pitrex/uvmc2/tacscan/tacscan.zip (MAME name: tacscan).
ZIP=/media/fat/pitrex/uvmc2/tacscan/tacscan.zip
[ -x /media/fat/pitrex/tacscan ] || { echo "Build tacscan first (see pitrex/uvmc2 in the repository)"; exit 1; }
[ -f "$ZIP" ] || { echo "Put the tacscan.zip romset in /media/fat/pitrex/uvmc2/tacscan/"; exit 1; }
UVMC2_DIR=/media/fat/pitrex/uvmc2/tacscan AAE_ROMZIP=$ZIP UVMC2_MUTE=0,1,2 exec /media/fat/pitrex/pitrex_launch.sh tacscan

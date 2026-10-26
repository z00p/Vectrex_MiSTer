#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# PiTrex on MiSTer launcher (used by the "PiTrex - <game>.sh" entries in Scripts).
# usage: pitrex_launch.sh <program> [args...]
# Stops any running PiTrex program, starts the new one in the background (it
# waits, read-only, for the Vectrex core and PiTrex Mode), then loads the core.
PITREX=/media/fat/pitrex
FAT=/media/fat
CORE=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_[0-9]*.rbf 2>/dev/null | sort | tail -1)
[ -n "$CORE" ] || CORE=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_*.rbf 2>/dev/null | sort -V | tail -1)
[ -n "$CORE" ] || CORE=$(ls -1 $FAT/_Unstable/Vectrex_PiTrex_B*.rbf 2>/dev/null | sort -V | tail -1)
PROG="$1"; shift

if [ -z "$CORE" ]; then echo "No Vectrex-PiTrex core found in /media/fat/_Unstable"; exit 1; fi
if [ ! -x "$PITREX/$PROG" ]; then echo "No program $PITREX/$PROG"; exit 1; fi

# stop a running PiTrex program (it hands the Vectrex back cleanly on SIGTERM)
for p in $(ps | grep -v grep | grep -v pitrex_launch | grep -E "/media/fat/pitrex/[^ ]+( |$)|/tmp/ptx/" | grep -v -E "pitrex_launch|pitrexd" | awk '{print $1}'); do
	kill "$p" 2>/dev/null
done
sleep 1

cd "$PITREX" || exit 1
if [ "$(cat /tmp/CORENAME 2>/dev/null)" = "Vectrex-PiTrex" ]; then
	# our core already running: just switch programs
	PTX_BACKEND=mister nohup chrt -r 49 "$PITREX/$PROG" "$@" > /tmp/pitrex.log 2>&1 &
	echo "Starting $PROG on the running Vectrex-PiTrex core"
else
	# Load our core through an MGL so it runs as "Vectrex-PiTrex" with its own settings
	# (the official Vectrex core also reports VECTREX, so that name proves nothing).
	# The program waits, reading /tmp/CORENAME only, until the core is up.
	PTX_BACKEND=mister PTX_WAIT_CORE=60 nohup chrt -r 49 "$PITREX/$PROG" "$@" > /tmp/pitrex.log 2>&1 &
	echo "Starting $PROG on $(basename "$CORE")"
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
	RBF=${CORE#$FAT/}; RBF=${RBF%.rbf}
	printf '<mistergamedescription>\n  <rbf>%s</rbf>\n  <setname>Vectrex-PiTrex</setname>\n</mistergamedescription>\n' "$RBF" > /tmp/pitrex_core.mgl
	echo "load_core /tmp/pitrex_core.mgl" > /dev/MiSTer_cmd
fi

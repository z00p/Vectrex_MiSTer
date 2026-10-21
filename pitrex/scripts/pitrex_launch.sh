#!/bin/bash
# PiTrex on MiSTer launcher (used by the "PiTrex - <game>.sh" entries in Scripts).
# usage: pitrex_launch.sh <program> [args...]
# Stops any running PiTrex program, starts the new one in the background (it
# waits, read-only, for the Vectrex core and PiTrex Mode), then loads the core.
PITREX=/media/fat/pitrex
CORE=$(ls -1 /media/fat/_Unstable/Vectrex_PiTrex_B*.rbf 2>/dev/null | sort -V | tail -1)
PROG="$1"; shift

if [ -z "$CORE" ]; then echo "No Vectrex_PiTrex core found in /media/fat/_Unstable"; exit 1; fi
if [ ! -x "$PITREX/$PROG" ]; then echo "No program $PITREX/$PROG"; exit 1; fi

# stop a running PiTrex program (it hands the Vectrex back cleanly on SIGTERM)
for p in $(ps | grep -v grep | grep -v pitrex_launch | grep -E "(ptx_selftest|hello|cube|gyrocks|vecbreak|vecdrive)( |$)|/tmp/ptx/" | awk '{print $1}'); do
	kill "$p" 2>/dev/null
done
sleep 1

cd "$PITREX" || exit 1
if [ "$(cat /tmp/CORENAME 2>/dev/null)" = "VECTREX" ]; then
	# Vectrex core already running: just switch programs (keeps PiTrex Mode as it is)
	PTX_BACKEND=mister nohup "$PITREX/$PROG" "$@" > /tmp/pitrex.log 2>&1 &
	echo "Starting $PROG on the running Vectrex core"
else
	# Load the core first; the program waits (reading /tmp/CORENAME only) until
	# the new core is up, then read-only until PiTrex Mode is on.
	PTX_BACKEND=mister PTX_WAIT_CORE=60 nohup "$PITREX/$PROG" "$@" > /tmp/pitrex.log 2>&1 &
	echo "Starting $PROG on $(basename "$CORE")"
	# PiTrex Mode = OSD status bit 36 (byte 4, bit 4) in config/VECTREX_v1.CFG, read when the core
	# loads. With the mode On the core only listens until a program's heartbeat appears, so
	# leaving it On is harmless for normal cartridges. Other settings are kept.
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
    print("PiTrex Mode set On in VECTREX_v1.CFG")
EOF
	echo "load_core $CORE" > /dev/MiSTer_cmd
fi
echo "(PiTrex Mode must be On: OSD > System)"

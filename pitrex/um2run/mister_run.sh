#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# On the MiSTer: run a .um2 through the Vectrex core (PiTrex mode must be ON, Vectrex-PiTrex core loaded).
#   ./mister_run.sh game.um2 [um2run args]     log: /tmp/um2.log (bounded: um2run --bus stops after 300 s unless --secs N)
#   ./mister_run.sh --stop                                 hands the Vectrex back
# --hold is ON by default (a frame's commands reach the ring at once; this removed doubled "ghost" strokes in testing). Give --no-hold to switch it off.
# The LOADING screen + progress bar (until the image first draws) is on by default; give --no-loading to switch it off (sidecar <image>.load keeps the SD sector total).
# Cart audio (28 Oct 2026, needs a Vectrex-PiTrex core with the PCM channel, build >= 6): on by default; UM2_NOAUDIO=1 ./mister_run.sh ... switches it off (A/B), UM2_AUDPRE=<ms>
# sets the jitter buffer (default 60).  The log's "um2aud:" / "cart audio:" lines show what reached the core.  See DESIGN.md, "Cart audio".
# The .um2 images are not part of this release: bring your own (see README.md). Normally pitrexd runs this for you when you pick a game in the OSD.
cd "$(dirname "$0")" || exit 1
if [ "$1" = "--stop" ]; then killall um2run 2>/dev/null; exit 0; fi
[ -n "$1" ] || { echo "usage: $0 game.um2 [um2run args] | --stop"; exit 1; }
img=$1; shift
hold=--hold; args=""
for a in "$@"; do if [ "$a" = "--no-hold" ]; then hold=""; else args="$args $a"; fi; done
# Black-screen guard (26 Oct 2026): unless the caller picks its own placement (--cpu/--rt), use the proven 2-CPU layout but SETTLE
# first and keep core 1 on CPU0 for the first seconds, so Main (pinned to CPU1) is never starved while it sets the video mode.
# The guard is only needed right after a core load: with the core up for 15 s or more (age of /tmp/CORENAME) it shrinks to 1 s of settle, and core 1 has its own
# CPU from the start.  The long guard costs cart audio (BattleZone: static for the first 10 s and then a 20 s fast catch-up, seen on hardware 28 Oct), and even 2 s
# showed as a slow, rough start (2 Nov 2026, 80 starts at 2 / 1 / 0.5 / 0 s: Punch-Out's second second played 40% of real time at 2 s, 100% at 0, none stopped).
st=8; c1=10; ca=$(( $(date +%s) - $(stat -c %Y /tmp/CORENAME 2>/dev/null || date +%s) )); [ "$ca" -ge 15 ] && { st=1; c1=0; }
# whole words only: $args is " a1 a2 ..." (a space before each arg), so with a space added at the end every arg,
# first or last, is " arg "; --cpu1 / --cpu1-after alone do not count as picking the placement
case "$args " in *" --cpu "*|*" --rt "*) ;; *) args="--cpu 0 --cpu1 1 --donate 3000 --settle ${SETTLE:-$st} --cpu1-after ${CPU1_AFTER:-$c1}$args";; esac
# PTX_LEAD=1 (3 Nov 2026): the game runs at most one frame ahead of the core, not the backend's two. Same frame rate on ten games, about 40% less
# queued picture, so input reaches the screen sooner (GitHub issue 1).
# The last game's log is kept as /tmp/um2.prev.log. A game that dies by itself is said on the Vectrex and in pitrexd's log, with its exit status: a SIGUSR1 death left
# nothing anywhere until 31 Oct. Stopped on purpose (the backend's signal handler exits 0, or killed) and core changes stay quiet.
[ -f /tmp/um2.log ] && mv -f /tmp/um2.log /tmp/um2.prev.log
# Bluetooth test (9 Nov 2026): with a file FAIR_TEST beside this script the game runs at normal priority instead of SCHED_RR 50, so the
# kernel's USB / Bluetooth work on CPU0 is not held back by the real-time throttle (950 ms of every second for RT work).
sched="chrt -r 50"; [ -f FAIR_TEST ] && { sched=""; echo "$(date +%T) FAIR_TEST: um2run at normal priority" >> /tmp/pitrexd.log; }
(
	PTX_LEAD=${PTX_LEAD:-1} PTX_BACKEND=mister PTX_WAIT_CORE=60 nohup $sched ./um2run --bus $hold $args "$img" > /tmp/um2.log 2>&1
	rc=$?
	case $rc in 0|137|143) exit 0 ;; esac
	sleep 3                                       # um2run sees a core change before Main names the new core
	[ "$(cat /tmp/CORENAME 2>/dev/null)" = "Vectrex-PiTrex" ] || exit 0
	# A reset the player asked for is not a crash (8 Nov 2026: a Vectrex cart picked in the OSD over a UVMC2 game showed GAME STOPPED / EXIT STATUS 3
	# on top of the cart). The core's drop cause (pitrex_reset.sv) bit 2 = OSD reset, bit 3 = a file loaded into the core, bit 6 = a BIOS loaded.
	c=$(sed -n 's/.*core was reset.*cause 0x\([0-9a-f][0-9a-f]\)).*/\1/p' /tmp/um2.log | tail -1)
	if [ -n "$c" ] && [ $(( 0x$c & 0x4C )) -ne 0 ]; then
		echo "$(date +%T) um2run $(basename "$img") stopped: the core was reset from the OSD (cause 0x$c)" >> /tmp/pitrexd.log; exit 0
	fi
	echo "$(date +%T) um2run $(basename "$img") ended by itself, exit status $rc, see /tmp/um2.log" >> /tmp/pitrexd.log
	PTX_BACKEND=mister exec nohup chrt -r 49 "$PWD/um2run" --message "GAME STOPPED|EXIT STATUS $rc" > /tmp/um2msg.log 2>&1
) > /dev/null 2>&1 &
echo "started um2run --bus $hold $args $img, log /tmp/um2.log"

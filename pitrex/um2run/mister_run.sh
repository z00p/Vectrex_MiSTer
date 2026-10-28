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
# The guard is only needed right after a core load: with the core up for 15 s or more (age of /tmp/CORENAME) it shrinks to 1 / 2 s.  The long
# guard costs cart audio (BattleZone: static for the first 10 s and then a 20 s fast catch-up, seen on hardware 28 Oct; with 1 / 2 s it was clean).
st=8; c1=10; ca=$(( $(date +%s) - $(stat -c %Y /tmp/CORENAME 2>/dev/null || date +%s) )); [ "$ca" -ge 15 ] && { st=1; c1=2; }
case "$args" in *--cpu*|*--rt*) ;; *) args="--cpu 0 --cpu1 1 --donate 3000 --settle ${SETTLE:-$st} --cpu1-after ${CPU1_AFTER:-$c1}$args";; esac
PTX_BACKEND=mister PTX_WAIT_CORE=60 nohup chrt -r 50 ./um2run --bus $hold $args "$img" > /tmp/um2.log 2>&1 &
echo "started um2run --bus $hold $args $img (pid $!), log /tmp/um2.log"

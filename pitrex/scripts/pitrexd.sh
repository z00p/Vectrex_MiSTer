#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# pitrexd: starts the program picked in the Vectrex core's OSD ("Load UVMC2 / PiTrex", B9) or by
# an MGL menu entry. A .ptx file is the ARM program itself; a .um2 file is a UVMC2 cartridge
# image, run unmodified by um2run (see start_um2 below).
# Main reports the pick in /tmp/FILESELECT + /tmp/FULLPATH + /tmp/CURRENTPATH (needs log_file_entry=1
# in MiSTer.ini; PiTrex_Setup.sh sets it for our core only, in a [Vectrex-PiTrex] section). Started from linux/user-startup.sh.
# Paths. The PITREXD_* variables are test hooks (the mock test on a PC points them at a scratch directory); on the MiSTer leave them unset.
FAT=${PITREXD_FAT:-/media/fat}
PITREX=$FAT/pitrex
T=${PITREXD_TMP:-/tmp}                      # where Main keeps CORENAME, FILESELECT, FULLPATH, CURRENTPATH
RUN=$T/ptx
CMD=${PITREXD_CMD:-/dev/MiSTer_cmd}
CPUS=${PITREXD_CPUS:-/sys/devices/system/cpu/online}
# .um2 support (B9). Layout: $UM2/um2run + $UM2/mister_run.sh; per-game settings in a sidecar
# <image>.um2.cfg next to the picked file; card images in $UM2SD_DIR (default card: sd.img).
UM2=$PITREX/um2run
UM2SD_DIR=$PITREX/uvmc2
LOG=$T/pitrexd.log
# A "selected" report whose FULLPATH / CURRENTPATH are more than this many seconds older than FILESELECT is not a pick (see the main loop)
STALE=${PITREXD_STALE:-2}
# ... and more than this many seconds older than the current core load (CORENAME)
CORE_SLACK=${PITREXD_CORE_SLACK:-5}

# This daemon runs from boot, in every core, so it has to be cheap when it is not needed (31 Oct: at realtime priority with about 12 forks a
# second it cost a fork-heavy script about 10%). Waiting for our core it runs at normal priority and looks at CORENAME once a second. With our
# core loaded it polls fast at realtime priority: Main runs at SCHED_FIFO 48 and the programs above that, and a pick or a core change must still get through.
# The idle path uses shell builtins only: no fork from one poll to the next. PITREXD_RT set: leave the priority alone (the mock test).
prio=""
set_prio() {
	[ "$prio" = "$1" ] && return 0
	prio=$1
	[ -z "$PITREXD_RT" ] || return 0
	if [ "$1" = fast ]; then chrt -r -p 49 $$ > /dev/null 2>&1; else chrt -o -p 0 $$ > /dev/null 2>&1; fi
}
# nap <seconds>: read with a timeout on a pipe nobody writes to is a sleep without a fork
if mkfifo "$T/pitrexd.pipe.$$" 2>/dev/null && exec 3<> "$T/pitrexd.pipe.$$"; then
	rm -f "$T/pitrexd.pipe.$$"
	nap() { read -r -t "$1" -u 3 _; }
else
	nap() { sleep "$1"; }
fi

# A change of FILESELECT = newer than our marker, which carries the time of the report last looked at (the test compares to the nanosecond:
# Main can write FILESELECT twice in one second; 28 Oct, a pick was missed with %Y.%Z).
SEEN=$T/pitrexd.seen
seen_now() { rm -f "$SEEN"; touch -r "$T/FILESELECT" "$SEEN" 2>/dev/null; }
seen_now
last_f=""; last_t=0

core_is_ours() { local c; read -r c 2>/dev/null < $T/CORENAME; case "$c" in Vectrex-PiTrex|VECTREX) return 0 ;; esac; return 1; }

# PiTrex programs: the generic match used for switching and for the supervisor below.
# um2run is started by mister_run.sh as "./um2run --bus ...", which the path patterns miss. The card and menu builders
# are not programs: leaving the core used to kill a card build half way (8 Nov 2026).
ptx_pids() { ps | grep -v grep | grep -E "$PITREX/[^ ]+( |$)|$RUN/|um2run --bus" | grep -v -E "pitrex_launch|pitrexd\.sh|make_card|make_menu" | awk '{print $1}'; }

# SIGTERM every PiTrex program and wait for it to exit (SIGKILL after ~2 s).
stop_programs() {
	local pids p i
	pids=$(ptx_pids)
	[ -n "$pids" ] || return 0
	for p in $pids; do kill "$p" 2>/dev/null; done
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		pids=$(ptx_pids)
		[ -n "$pids" ] || return 0
		sleep 0.1
	done
	for p in $pids; do kill -9 "$p" 2>/dev/null; done
	echo "$(date +%T) program did not exit on SIGTERM: killed" >> $LOG
}

# cfg_get <file> <key>: value of "key=value" in a sidecar (last one wins; blank lines, lines starting
# with # and a trailing CR are ignored; the value ends at the end of the line). Never sourced.
cfg_get() {
	[ -f "$1" ] || return 0
	tr -d '\r' < "$1" | sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" | tail -1 | sed 's/[[:space:]]*$//'
}

# find_art <game> <sidecar>: the overlay art to load again when the core is reloaded for a game (art_lib.sh, next to this script).
# Without the library there is no art, and the reload is as it was.
case "$0" in */*) ART_LIB=${0%/*}/art_lib.sh ;; *) ART_LIB=./art_lib.sh ;; esac
[ -f "$ART_LIB" ] && . "$ART_LIB" || find_art() { return 0; }

# show <text>: a pick that cannot start says why on the Vectrex for 20 s ('|' = new line, 4 lines of 16 characters at most: 17 and up run off the screen's sides, John's photo 8 Nov 2026),
# as well as in the log. Never over a running program; the next pick or a core change stops it like any other program.
show() {
	[ -x "$UM2/um2run" ] || return 0
	[ -z "$(ptx_pids)" ] || return 0
	PTX_BACKEND=mister nohup chrt -r 49 "$UM2/um2run" --message "$1" > $T/um2msg.log 2>&1 &
}

# newest Vectrex-PiTrex core in _Console (from 1.0), else in _Unstable (the alphas and betas)
# (standard MiSTer naming from B8: Vectrex-PiTrex_YYYYMMDD.rbf; older B<n> names as a fallback)
newest_core() {
	local c
	c=$(ls -1 $FAT/_Console/Vectrex-PiTrex_[0-9]*.rbf 2>/dev/null | sort | tail -1)
	[ -n "$c" ] || c=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_[0-9]*.rbf 2>/dev/null | sort | tail -1)
	[ -n "$c" ] || c=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_*.rbf 2>/dev/null | sort -V | tail -1)
	[ -n "$c" ] || c=$(ls -1 $FAT/_Unstable/Vectrex_PiTrex_B*.rbf 2>/dev/null | sort -V | tail -1)
	[ -n "$c" ] && echo "$c"
}

# display_apply <orientation 0|1|2|-> <view 0|1|-> : read-modify-write the core's settings file ($FAT/config/Vectrex-PiTrex_v2.CFG),
# the same way pitrex_launch.sh sets PiTrex Mode (Main rewrites the file whenever the OSD is used, so only our bits are touched).
# The layout is B13's (Videodr0me's VECTREX_v2):
#   Orientation = status[76:75] = byte 9 bits 4:3  (0 normal, 1 rotated 90 degrees counter-clockwise, 2 clockwise)
#   View Area   = status[65]    = byte 8 bit 1     (0 standard, 1 open matte)
#   PiTrex Mode = status[117]   = byte 14 bit 5    (forced on when the file is rewritten, as the core is about to be reloaded for the program)
# Exit status: 0 file changed, 1 nothing to change, 2 error. "-" leaves a value alone.
display_apply() {
	DCFG=$FAT/config/Vectrex-PiTrex_v2.CFG python3 - "$1" "$2" <<'PYEOF'
import os, sys
path = os.environ["DCFG"]
try:
    b = bytearray(open(path, "rb").read())
except OSError:
    b = bytearray()
b += bytes(max(0, 16 - len(b)))
new = bytearray(b)
if sys.argv[1] != "-":
    new[9] = (new[9] & ~0x18) | (int(sys.argv[1]) << 3)
if sys.argv[2] != "-":
    new[8] = (new[8] & ~0x02) | (0x02 if sys.argv[2] == "1" else 0)
if new == b:
    sys.exit(1)
new[14] |= 0x20
try:
    open(path, "wb").write(new)
except OSError:
    sys.exit(2)
PYEOF
}

# Load the newest Vectrex-PiTrex core again (an MGL, exactly like pitrex_launch.sh) and wait until Main reports it in CORENAME.
# Returns 1 when there is no core to load; waits at most 30 s (um2run waits for the core by itself too).
#   reload_core [art]: with the path of an .art file, the MGL loads it as the overlay (index 2, as the menu MGLs do), because the reload wipes
#   the art Main loaded after the pick. A path under games/Vectrex-PiTrex is written relative to it, as in the menu MGLs; any other as it is.
reload_core() {
	local core rbf before i art=$1
	core=$(newest_core) || return 1
	rbf=${core#$FAT/}; rbf=${rbf%.rbf}
	[ -n "$art" ] && [ -f "$art" ] || art=""
	case "$art" in "$FAT/games/Vectrex-PiTrex/"*) art=${art#$FAT/games/Vectrex-PiTrex/} ;; esac
	art=${art//"&"/"&amp;"}; art=${art//"<"/"&lt;"}; art=${art//'"'/"&quot;"}
	{
		printf '<mistergamedescription>\n  <rbf>%s</rbf>\n  <setname>Vectrex-PiTrex</setname>\n' "$rbf"
		[ -z "$art" ] || printf '  <file delay="1" type="f" index="2" path="%s"/>\n' "$art"
		printf '</mistergamedescription>\n'
	} > $T/pitrex_core.mgl
	before=$(stat -c %y $T/CORENAME 2>/dev/null)
	echo "load_core $T/pitrex_core.mgl" > $CMD
	i=0
	while [ $i -lt 150 ]; do
		sleep 0.2; i=$((i+1))
		[ "$(stat -c %y $T/CORENAME 2>/dev/null)" != "$before" ] && [ "$(cat $T/CORENAME 2>/dev/null)" = "Vectrex-PiTrex" ] && return 0
	done
	echo "$(date +%T) core reload: CORENAME did not change within 30 s, going on" >> $LOG
	return 0
}

# Sidecar keys orient= and view= (EXPERIMENTAL): set the core's Orientation / View Area for a game (landscape games such as Star Wars
# and BattleZone are drawn sideways on the portrait Vectrex screen) and reload the core when the saved setting differs.
# Only the core's own settings file Vectrex-PiTrex_v2.CFG is touched, never VECTREX_v1.CFG: a core that runs as VECTREX is left alone.
# An unset key leaves that setting as it is. Sets reloaded=1 when the core was reloaded; the reload loads the game's overlay art again (find_art).
# set_display <sidecar> <game file>
reloaded=0
set_display() {
	local cfg=$1 o v ob vb c art
	o=$(cfg_get "$cfg" orient); v=$(cfg_get "$cfg" view)
	[ -n "$o$v" ] || return 0
	case "${o,,}" in "") ob=- ;; 0) ob=0 ;; ccw) ob=1 ;; cw) ob=2 ;; *) echo "$(date +%T) $cfg: bad orient '$o' ignored (0, ccw or cw)" >> $LOG; ob=- ;; esac
	case "${v,,}" in "") vb=- ;; standard) vb=0 ;; open) vb=1 ;; *) echo "$(date +%T) $cfg: bad view '$v' ignored (standard or open)" >> $LOG; vb=- ;; esac
	[ "$ob$vb" = "--" ] && return 0
	read -r c 2>/dev/null < $T/CORENAME
	if [ "$c" != "Vectrex-PiTrex" ]; then
		echo "$(date +%T) orient/view skipped: the core runs as '$c' (its settings file is not touched); load it from the PiTrex & UVMC2 menu" >> $LOG
		return 0
	fi
	display_apply "$ob" "$vb"
	case $? in
		0) art=$(find_art "$2" "$cfg")
		   echo "$(date +%T) orient=${o:--} view=${v:--}: Vectrex-PiTrex_v2.CFG updated, reloading the core${art:+ with overlay ${art##*/}}" >> $LOG
		   if reload_core "$art"; then reloaded=1; else echo "$(date +%T) orient/view: no Vectrex-PiTrex core in $FAT/_Console or _Unstable to reload" >> $LOG; fi ;;
		2) echo "$(date +%T) orient/view: cannot write $FAT/config/Vectrex-PiTrex_v2.CFG" >> $LOG ;;
	esac
	return 0
}

# Start a UVMC2 cartridge image through mister_run.sh (its defaults: --bus --hold --cpu 0 --cpu1 1
# --donate 3000 --settle 8 --cpu1-after 10, or 1 / 0 once the core has been up 15 s; log /tmp/um2.log). Sidecar <image>.um2.cfg, key=value:
#   frame_cycles=25000      stream frame length in E cycles (default 30000 = 50 Hz; Punch-Out wants 25000 = 60 Hz)
#   sd=bz_sd.img            card image; a relative name is looked up next to the .um2 file, then in $UM2SD_DIR, then in $UM2 (BattleZone WRITES to its card: own copy).
#                           Default $UM2SD_DIR/sd.img if it exists, else no card. No spaces in the path.
#   args=--secs 600         extra um2run arguments, appended last
#   settle=8 cpu1_after=10  override the black-screen guard timings (mister_run.sh env SETTLE / CPU1_AFTER)
#   orient=0|ccw|cw         EXPERIMENTAL: the core's Orientation (normal / 90 degrees counter-clockwise / clockwise); see set_display above
#   view=standard|open      EXPERIMENTAL: the core's View Area (standard / open matte)
#   overlay=Star_Castle.art overlay art for a game whose art is not named like it: a path relative to the game's folder, or a bare name looked up
#                           there, then in games/Vectrex-PiTrex/Overlays and games/Vectrex/Overlays. Without it the art is <game>.art beside the game
#                           (then in games/Vectrex-PiTrex/Overlays). The core loads it after a pick; a reload for orient/view loads it again.
#                           make_card.sh copies the art next to the game for that (never over a .art of yours); orient=cw/ccw take the Tate version only.
# Returns 1 without touching a running program when it refuses (one core, missing files).
start_um2() {
	local img=$1 cfg=$1.cfg online fc sd args st c1 n opt=""
	read -r online < $CPUS
	if [ "$online" != "0-1" ]; then
		# BigMig leaves Linux on maxcpus=1 until a normal reboot: um2run would starve Main (OSD, controls)
		echo "$(date +%T) REFUSED $(basename "$img"): Linux has CPUs '$online' online, um2run needs 0-1 (reboot out of BigMig)" >> $LOG
		show "LINUX ON ONE CPU|REBOOT MISTER|THEN PICK AGAIN"
		return 1
	fi
	if [ ! -f "$UM2/um2run" ] || [ ! -f "$UM2/mister_run.sh" ]; then
		echo "$(date +%T) REFUSED $(basename "$img"): $UM2/um2run or mister_run.sh missing" >> $LOG
		show "UM2RUN MISSING|RUN PITREX|UPDATE"
		return 1
	fi
	fc=$(cfg_get "$cfg" frame_cycles)
	case "$fc" in ""|*[!0-9]*) [ -z "$fc" ] || echo "$(date +%T) $cfg: bad frame_cycles '$fc' ignored" >> $LOG; fc=30000
		# games that only run right with their own frame length need no sidecar for it
		n=${img##*/}; case "${n,,}" in *punch*) fc=25000 ;; esac ;; esac
	sd=$(cfg_get "$cfg" sd)
	if [ -n "$sd" ]; then
		case "$sd" in /*) ;; *) if [ -f "${img%/*}/$sd" ]; then sd=${img%/*}/$sd; elif [ -f "$UM2SD_DIR/$sd" ] || [ ! -f "$UM2/$sd" ]; then sd=$UM2SD_DIR/$sd; else sd=$UM2/$sd; fi ;; esac
		case "$sd" in *[[:space:]]*) echo "$(date +%T) $cfg: sd path has spaces, no card" >> $LOG; sd="" ;; esac
		if [ -n "$sd" ] && [ ! -f "$sd" ]; then echo "$(date +%T) $cfg: card $sd not found, no card" >> $LOG; sd=""; fi
	elif [ -f "$UM2SD_DIR/sd.img" ]; then
		sd=$UM2SD_DIR/sd.img
	fi
	args=$(cfg_get "$cfg" args)
	st=$(cfg_get "$cfg" settle); c1=$(cfg_get "$cfg" cpu1_after)
	case "$st" in *[!0-9]*) st="" ;; esac
	case "$c1" in *[!0-9]*) c1="" ;; esac
	[ -n "$sd" ] && opt="--sd $sd"

	stop_programs
	# a game runs on the card as it is; say so for a moment if the folder has moved on
	if [ "$sd" = "$UM2SD_DIR/sd.img" ]; then
		bash "$PITREX/make_card.sh" --check > /dev/null 2>&1
		if [ $? = 1 ]; then
			echo "$(date +%T) card out of date: starting on the current card" >> $LOG
			show "CARD OUT OF DATE|RUN PITREX MAKE|UVMC2 CARD|FROM SCRIPTS"
			sleep 3
			stop_programs
		fi
	fi
	reloaded=0
	set_display "$cfg" "$img"
	cd "$UM2" || return 1
	# mister_run.sh: cd to its own directory, nohup chrt -r 50 ./um2run --bus --hold ... > /tmp/um2.log &
	# --secs: um2run --bus stops after 300 s by default (a test bound); a game picked from the OSD runs until stopped.
	SETTLE=$st CPU1_AFTER=$c1 sh ./mister_run.sh "$img" --frame-cycles "$fc" $opt --secs 1000000000 $args >> $LOG 2>&1
	echo "$(date +%T) started um2run $(basename "$img") frame_cycles=$fc card=${sd:-none}${args:+ args=$args}" >> $LOG
}

# start_ptx <file>: a PiTrex program, one at a time: stop what is running (launcher or OSD started)
start_ptx() {
	local f=$1 name
	stop_programs
	name=$(basename "$f"); name=${name%.*}; name=${name// /_}
	if ! { mkdir -p "$RUN" && cp "$f" "$RUN/$name" && chmod +x "$RUN/$name"; }; then
		echo "$(date +%T) REFUSED $f: cannot copy it to $RUN" >> $LOG
		show "CANNOT START|PROGRAM"
		return 1
	fi
	cd "$PITREX" 2>/dev/null || cd $T
	[ -f $T/pitrex.log ] && mv -f $T/pitrex.log $T/pitrex.prev.log
	PTX_BACKEND=mister nohup chrt -r 49 "$RUN/$name" > $T/pitrex.log 2>&1 &
	echo "$(date +%T) started $name from $f" >> $LOG
}

# Our core started from the cores list runs as VECTREX, not Vectrex-PiTrex: its OSD offers the games, but MiSTer.ini's log_file_entry=1 is only in our
# section, so picks would do nothing and say nothing (30 Oct). The core name alone cannot tell it from the official core; Main's arguments can:
# the rbf, and no MGL. Reload it through an MGL once, the way the menu entry starts it. PITREXD_MAINCMD: test hook, a file with Main's arguments.
plain_ours() {
	local a
	if [ -n "$PITREXD_MAINCMD" ]; then a=$(tr '\0' ' ' < "$PITREXD_MAINCMD" 2>/dev/null)
	else a=$(tr '\0' ' ' < /proc/$(pidof -s MiSTer)/cmdline 2>/dev/null); fi     # -s: there can be several just after a load
	case "$a" in *.mgl*|*.MGL*) return 1 ;; *Vectrex-PiTrex_*.rbf*|*Vectrex_PiTrex_*.rbf*) return 0 ;; esac
	return 1
}
core_arrived() {
	local c
	read -r c 2>/dev/null < $T/CORENAME
	[ "$c" = VECTREX ] && plain_ours || return 0
	echo "$(date +%T) our core was started without the menu entry: loading it again as Vectrex-PiTrex" >> $LOG
	reload_core || echo "$(date +%T) core reload: no Vectrex-PiTrex core in $FAT/_Console or _Unstable" >> $LOG
}

# Supervisor. A program may only write the shared DDR block while our core is the loaded core.
# The program checks /tmp/CORENAME itself before every publish (src/ptx_backend_mister.c), but
# a user-space process cannot be revoked instantly: after another core is loaded there is a
# window (Main reconfigures the FPGA, then updates CORENAME, then we poll, every 0.2 s with our core loaded, then
# SIGTERM is delivered) in which it could still write. This kill makes that window as short
# as the daemon can; the program's own check usually wins. Only the ours -> other transition
# triggers it, so a launcher that starts a program before it loads the core is left alone.
was_ours=0; core_is_ours && { was_ours=1; set_prio fast; core_arrived; }
core_is_ours || set_prio idle

while :; do
	if core_is_ours; then
		nap 0.2
		if [ "$was_ours" != 1 ]; then was_ours=1; set_prio fast; core_arrived; fi
	else
		if [ "$was_ours" = 1 ]; then
			was_ours=0
			if [ -n "$(ptx_pids)" ]; then
				stop_programs
				echo "$(date +%T) core changed away from Vectrex-PiTrex: stopped PiTrex program(s)" >> $LOG
			fi
			set_prio idle
		fi
		# Nothing to do here but wait. A report written meanwhile (a menu entry loads the core and its file together) is handled once the core is seen.
		nap 1
		continue
	fi
	[ "$T/FILESELECT" -nt "$SEEN" ] || continue
	seen_now
	[ "$(cat $T/FILESELECT 2>/dev/null)" = "selected" ] || continue
	core_is_ours || continue
	# Stale reports. After Main restarts or loads a core it writes FILESELECT="selected" again with the OLD FULLPATH /
	# CURRENTPATH (28 Oct: the last game restarted by itself). Main writes the path files while the user browses, not when
	# the file is picked (29 Oct: entering the folder, then picking its first file 9 s later), so old path files alone do
	# not make a report stale. Stale = whichever path file is newer is more than $STALE s (default 2, whole seconds) older
	# than FILESELECT AND more than $CORE_SLACK s older than the current core load (CORENAME): every real OSD pick browses
	# after the core was loaded; a menu MGL writes FULLPATH and FILESELECT together, so it never gets here.
	fs=$(stat -c %Y $T/FILESELECT 2>/dev/null || echo 0)
	pt=$(stat -c %Y $T/FULLPATH 2>/dev/null || echo 0)
	ct=$(stat -c %Y $T/CURRENTPATH 2>/dev/null || echo 0)
	cn=$(stat -c %Y $T/CORENAME 2>/dev/null || echo 0)
	[ "$ct" -gt "$pt" ] && pt=$ct
	if [ $(( fs - pt )) -gt "$STALE" ] && [ $(( cn - pt )) -gt "$CORE_SLACK" ]; then
		echo "$(date +%T) ignored a stale file report ($(cat $T/FULLPATH 2>/dev/null)/$(cat $T/CURRENTPATH 2>/dev/null), $(( fs - pt )) s older than FILESELECT)" >> $LOG
		continue
	fi
	f=$(cat $T/FULLPATH 2>/dev/null)
	# An OSD pick (B9 hardware, 28 Oct) reports FULLPATH as the folder relative to /media/fat
	# ("games/Vectrex-PiTrex/UVMC2") and the file name in /tmp/CURRENTPATH.
	case "$f" in /*) ;; *) f=$FAT/$f ;; esac
	if [ -d "$f" ]; then f=$f/$(cat $T/CURRENTPATH 2>/dev/null); fi
	# From B12 each OSD entry takes one file type, and Main then leaves the extension off the reported name (3 Nov).
	case "${f,,}" in *.ptx|*.um2) ;; *) if [ -f "$f.um2" ]; then f=$f.um2; elif [ -f "$f.ptx" ]; then f=$f.ptx; fi ;; esac
	case "${f,,}" in *.ptx) kind=ptx ;; *.um2) kind=um2 ;; *) echo "$(date +%T) ignored $f: not a .um2 or .ptx file" >> $LOG; continue ;; esac
	if [ ! -f "$f" ]; then
		echo "$(date +%T) REFUSED $f: no such file" >> $LOG
		show "GAME FILE|NOT FOUND"
		continue
	fi
	# Main can report one MGL file load twice: leave a program just started from the same file alone
	if [ "$f" = "$last_f" ] && [ $(( $(date +%s) - last_t )) -lt 10 ] && [ -n "$(ptx_pids)" ]; then continue; fi
	last_f=$f; last_t=$(date +%s)

	if [ "$kind" = um2 ]; then
		start_um2 "$f"
		# The core was reloaded for orient/view: let the supervisor see the new core as the first one (the game was started after
		# the reload, so the "core changed" kill must not fire for it), and drop what Main wrote to FILESELECT meanwhile.
		if [ "$reloaded" = 1 ]; then was_ours=0; seen_now; last_f=$f; last_t=$(date +%s); fi
		continue
	fi

	start_ptx "$f"
done

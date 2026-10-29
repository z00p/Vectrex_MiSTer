#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# pitrexd: starts the program picked in the Vectrex core's OSD ("Load UVMC2 / PiTrex", B9) or by
# an MGL menu entry. A .ptx file is the ARM program itself; a .um2 file is a UVMC2 cartridge
# image, run unmodified by um2run (see start_um2 below).
# Main reports the pick in /tmp/FILESELECT + /tmp/FULLPATH + /tmp/CURRENTPATH (needs log_file_entry=1
# in MiSTer.ini; PiTrex_Setup.sh sets it). Started from linux/user-startup.sh.
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

# Main runs at SCHED_FIFO 48 and keeps the CPU almost fully busy, so a normal-priority daemon
# sees a file pick seconds late. Run above it, like the programs (the loop sleeps between polls).
if [ -z "$PITREXD_RT" ]; then PITREXD_RT=1 exec chrt -r 49 "$0" "$@"; fi

# Full-resolution times: Main can write FILESELECT twice in one second (28 Oct: a pick was missed with %Y.%Z).
stamp() { stat -c "%y %z" $T/FILESELECT 2>/dev/null; }
last=$(stamp)
last_f=""; last_t=0

core_is_ours() { local c; read -r c 2>/dev/null < $T/CORENAME; case "$c" in Vectrex-PiTrex|VECTREX) return 0 ;; esac; return 1; }

# PiTrex programs: the generic match used for switching and for the supervisor below.
# um2run is started by mister_run.sh as "./um2run --bus ...", which the path patterns miss.
ptx_pids() { ps | grep -v grep | grep -E "$PITREX/[^ ]+( |$)|$RUN/|um2run --bus" | grep -v -E "pitrex_launch|pitrexd" | awk '{print $1}'; }

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

# newest Vectrex-PiTrex core in _Unstable (standard MiSTer naming from B8: Vectrex-PiTrex_YYYYMMDD.rbf; older B<n> names as a fallback)
newest_core() {
	local c
	c=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_[0-9]*.rbf 2>/dev/null | sort | tail -1)
	[ -n "$c" ] || c=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_*.rbf 2>/dev/null | sort -V | tail -1)
	[ -n "$c" ] || c=$(ls -1 $FAT/_Unstable/Vectrex_PiTrex_B*.rbf 2>/dev/null | sort -V | tail -1)
	[ -n "$c" ] && echo "$c"
}

# display_apply <orientation 0|1|2|-> <view 0|1|-> : read-modify-write the core's settings file ($FAT/config/Vectrex-PiTrex_v1.CFG),
# the same way pitrex_launch.sh sets PiTrex Mode (Main rewrites the file whenever the OSD is used, so only our bits are touched).
#   Orientation = status[21:20] = byte 2 bits 5:4 (0 normal, 1 rotated 90 degrees counter-clockwise, 2 clockwise)
#   View Area   = status[31]    = byte 3 bit 7    (0 standard, 1 open matte)
#   PiTrex Mode = status[36]    = byte 4 bit 4    (forced on when the file is rewritten, as the core is about to be reloaded for the program)
# Exit status: 0 file changed, 1 nothing to change, 2 error. "-" leaves a value alone.
display_apply() {
	DCFG=$FAT/config/Vectrex-PiTrex_v1.CFG python3 - "$1" "$2" <<'PYEOF'
import os, sys
path = os.environ["DCFG"]
try:
    b = bytearray(open(path, "rb").read())
except OSError:
    b = bytearray()
b += bytes(max(0, 16 - len(b)))
new = bytearray(b)
if sys.argv[1] != "-":
    new[2] = (new[2] & ~0x30) | (int(sys.argv[1]) << 4)
if sys.argv[2] != "-":
    new[3] = (new[3] & ~0x80) | (0x80 if sys.argv[2] == "1" else 0)
if new == b:
    sys.exit(1)
new[4] |= 0x10
try:
    open(path, "wb").write(new)
except OSError:
    sys.exit(2)
PYEOF
}

# Load the newest Vectrex-PiTrex core again (an MGL, exactly like pitrex_launch.sh) and wait until Main reports it in CORENAME.
# Returns 1 when there is no core to load; waits at most 30 s (um2run waits for the core by itself too).
reload_core() {
	local core rbf before i
	core=$(newest_core) || return 1
	rbf=${core#$FAT/}; rbf=${rbf%.rbf}
	printf '<mistergamedescription>\n  <rbf>%s</rbf>\n  <setname>Vectrex-PiTrex</setname>\n</mistergamedescription>\n' "$rbf" > $T/pitrex_core.mgl
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
# Only the core's own settings file Vectrex-PiTrex_v1.CFG is touched, never VECTREX_v1.CFG: a core that runs as VECTREX is left alone.
# An unset key leaves that setting as it is. Sets reloaded=1 when the core was reloaded.
reloaded=0
set_display() {
	local cfg=$1 o v ob vb c
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
		0) echo "$(date +%T) orient=${o:--} view=${v:--}: Vectrex-PiTrex_v1.CFG updated, reloading the core" >> $LOG
		   if reload_core; then reloaded=1; else echo "$(date +%T) orient/view: no Vectrex-PiTrex core in $FAT/_Unstable to reload" >> $LOG; fi ;;
		2) echo "$(date +%T) orient/view: cannot write $FAT/config/Vectrex-PiTrex_v1.CFG" >> $LOG ;;
	esac
	return 0
}

# Start a UVMC2 cartridge image through mister_run.sh (its defaults: --bus --hold --cpu 0 --cpu1 1
# --donate 3000 --settle 8 --cpu1-after 10, or 1 / 2 once the core has been up 15 s; log /tmp/um2.log). Sidecar <image>.um2.cfg, key=value:
#   frame_cycles=25000      stream frame length in E cycles (default 30000 = 50 Hz; Punch-Out wants 25000 = 60 Hz)
#   sd=bz_sd.img            card image; a relative name is looked up next to the .um2 file, then in $UM2SD_DIR, then in $UM2 (BattleZone WRITES to its card: own copy).
#                           Default $UM2SD_DIR/sd.img if it exists, else no card. No spaces in the path.
#   args=--secs 600         extra um2run arguments, appended last
#   settle=8 cpu1_after=10  override the black-screen guard timings (mister_run.sh env SETTLE / CPU1_AFTER)
#   orient=0|ccw|cw         EXPERIMENTAL: the core's Orientation (normal / 90 degrees counter-clockwise / clockwise); see set_display above
#   view=standard|open      EXPERIMENTAL: the core's View Area (standard / open matte)
# Returns 1 without touching a running program when it refuses (one core, missing files).
start_um2() {
	local img=$1 cfg=$1.cfg online fc sd args st c1 opt=""
	read -r online < $CPUS
	if [ "$online" != "0-1" ]; then
		# BigMig leaves Linux on maxcpus=1 until a normal reboot: um2run would starve Main (OSD, controls)
		echo "$(date +%T) REFUSED $(basename "$img"): Linux has CPUs '$online' online, um2run needs 0-1 (reboot out of BigMig)" >> $LOG
		return 1
	fi
	if [ ! -f "$UM2/um2run" ] || [ ! -f "$UM2/mister_run.sh" ]; then
		echo "$(date +%T) REFUSED $(basename "$img"): $UM2/um2run or mister_run.sh missing" >> $LOG
		return 1
	fi
	fc=$(cfg_get "$cfg" frame_cycles)
	case "$fc" in ""|*[!0-9]*) [ -z "$fc" ] || echo "$(date +%T) $cfg: bad frame_cycles '$fc' ignored" >> $LOG; fc=30000 ;; esac
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
	reloaded=0
	set_display "$cfg"
	cd "$UM2" || return 1
	# mister_run.sh: cd to its own directory, nohup chrt -r 50 ./um2run --bus --hold ... > /tmp/um2.log &
	# --secs: um2run --bus stops after 300 s by default (a test bound); a game picked from the OSD runs until stopped.
	SETTLE=$st CPU1_AFTER=$c1 sh ./mister_run.sh "$img" --frame-cycles "$fc" $opt --secs 1000000000 $args >> $LOG 2>&1
	echo "$(date +%T) started um2run $(basename "$img") frame_cycles=$fc card=${sd:-none}${args:+ args=$args}" >> $LOG
}

# Supervisor. A program may only write the shared DDR block while our core is the loaded core.
# The program checks /tmp/CORENAME itself before every publish (src/ptx_backend_mister.c), but
# a user-space process cannot be revoked instantly: after another core is loaded there is a
# window (Main reconfigures the FPGA, then updates CORENAME, then we poll every 0.2 s, then
# SIGTERM is delivered) in which it could still write. This kill makes that window as short
# as the daemon can; the program's own check usually wins. Only the ours -> other transition
# triggers it, so a launcher that starts a program before it loads the core is left alone.
was_ours=0; core_is_ours && was_ours=1

while sleep 0.2; do
	if core_is_ours; then was_ours=1; else
		if [ "$was_ours" = 1 ]; then
			was_ours=0
			if [ -n "$(ptx_pids)" ]; then
				stop_programs
				echo "$(date +%T) core changed away from Vectrex-PiTrex: stopped PiTrex program(s)" >> $LOG
			fi
		fi
	fi
	now=$(stamp)
	[ "$now" = "$last" ] && continue
	last=$now
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
	case "${f,,}" in *.ptx) kind=ptx ;; *.um2) kind=um2 ;; *) continue ;; esac
	[ -f "$f" ] || continue
	# Main can report one MGL file load twice: leave a program just started from the same file alone
	if [ "$f" = "$last_f" ] && [ $(( $(date +%s) - last_t )) -lt 10 ] && [ -n "$(ptx_pids)" ]; then continue; fi
	last_f=$f; last_t=$(date +%s)

	if [ "$kind" = um2 ]; then
		start_um2 "$f"
		# The core was reloaded for orient/view: let the supervisor see the new core as the first one (the game was started after
		# the reload, so the "core changed" kill must not fire for it), and drop what Main wrote to FILESELECT meanwhile.
		if [ "$reloaded" = 1 ]; then was_ours=0; last=$(stamp); last_f=$f; last_t=$(date +%s); fi
		continue
	fi

	# one program at a time: stop what is running (launcher or OSD started)
	stop_programs
	name=$(basename "$f"); name=${name%.*}; name=${name// /_}
	mkdir -p "$RUN" && cp "$f" "$RUN/$name" && chmod +x "$RUN/$name" || continue
	cd "$PITREX" 2>/dev/null || cd $T
	PTX_BACKEND=mister nohup chrt -r 49 "$RUN/$name" > $T/pitrex.log 2>&1 &
	echo "$(date +%T) started $name from $f" >> $LOG
done

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# pitrexd: starts the PiTrex program picked in the Vectrex core's OSD
# (System > Load PiTrex Program). A .ptx file is the ARM program itself.
# Main reports the pick in /tmp/FILESELECT + /tmp/FULLPATH (needs log_file_entry=1
# in MiSTer.ini; PiTrex_Setup.sh sets it). Started from linux/user-startup.sh.
PITREX=/media/fat/pitrex
RUN=/tmp/ptx

stamp() { stat -c %Y.%Z /tmp/FILESELECT 2>/dev/null; }
last=$(stamp)

core_is_ours() { local c; read -r c 2>/dev/null < /tmp/CORENAME; case "$c" in Vectrex-PiTrex|VECTREX) return 0 ;; esac; return 1; }

# PiTrex programs: the generic match used for switching and for the supervisor below.
ptx_pids() { ps | grep -v grep | grep -E "/media/fat/pitrex/[^ ]+( |$)|$RUN/" | grep -v -E "pitrex_launch|pitrexd" | awk '{print $1}'; }

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
	echo "$(date +%T) program did not exit on SIGTERM: killed" >> /tmp/pitrexd.log
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
				echo "$(date +%T) core changed away from Vectrex-PiTrex: stopped PiTrex program(s)" >> /tmp/pitrexd.log
			fi
		fi
	fi
	now=$(stamp)
	[ "$now" = "$last" ] && continue
	last=$now
	[ "$(cat /tmp/FILESELECT 2>/dev/null)" = "selected" ] || continue
	core_is_ours || continue
	f=$(cat /tmp/FULLPATH 2>/dev/null)
	case "${f,,}" in *.ptx) ;; *) continue ;; esac
	[ -f "$f" ] || continue

	# one program at a time: stop what is running (launcher or OSD started)
	stop_programs
	name=$(basename "$f"); name=${name%.*}; name=${name// /_}
	mkdir -p "$RUN" && cp "$f" "$RUN/$name" && chmod +x "$RUN/$name" || continue
	cd "$PITREX" 2>/dev/null || cd /tmp
	PTX_BACKEND=mister nohup "$RUN/$name" > /tmp/pitrex.log 2>&1 &
	echo "$(date +%T) started $name from $f" >> /tmp/pitrexd.log
done

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

while sleep 0.5; do
	now=$(stamp)
	[ "$now" = "$last" ] && continue
	last=$now
	[ "$(cat /tmp/FILESELECT 2>/dev/null)" = "selected" ] || continue
	[ "$(cat /tmp/CORENAME 2>/dev/null)" = "VECTREX" ] || continue
	f=$(cat /tmp/FULLPATH 2>/dev/null)
	case "${f,,}" in *.ptx) ;; *) continue ;; esac
	[ -f "$f" ] || continue

	# one program at a time: stop what is running (launcher or OSD started)
	for p in $(ps | grep -v grep | grep -E "(ptx_selftest|hello|cube|gyrocks|vecbreak|vecdrive)( |$)|$RUN/" | awk '{print $1}'); do
		kill "$p"
	done
	sleep 1
	name=$(basename "$f"); name=${name%.*}; name=${name// /_}
	mkdir -p "$RUN" && cp "$f" "$RUN/$name" && chmod +x "$RUN/$name" || continue
	cd "$PITREX" 2>/dev/null || cd /tmp
	PTX_BACKEND=mister nohup "$RUN/$name" > /tmp/pitrex.log 2>&1 &
	echo "$(date +%T) started $name from $f" >> /tmp/pitrexd.log
done

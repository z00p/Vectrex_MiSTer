#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Stop the running PiTrex program; the Vectrex core hands back to the 6809.
n=0
for p in $(ps | grep -v grep | grep -v pitrex_launch | grep -E "(ptx_selftest|hello|cube|gyrocks|vecbreak|vecdrive)( |$)" | awk '{print $1}'); do
	kill "$p" 2>/dev/null && n=$((n+1))
done
echo "Stopped $n PiTrex program(s)."

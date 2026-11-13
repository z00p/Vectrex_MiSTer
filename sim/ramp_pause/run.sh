#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# RAMP pause test: sim/ramp_pause/run.sh [VARIANT ...]       (GHDL 5; default: before cur)
#   cur = rtl/vectrex.vhd as it is (fixed); before, before_free, free: see make_variants.py
# For each version: a reference run (no stall), then a stall of STALL_LEN clocks starting 0..SWEEP clocks after the
# E-cycle that presents the RAMP-on write (anchor 0) and the RAMP-off write (anchor 1), one clock apart.
# Prints the 16-phase table (every 12 clocks, as in Videodr0me's test) and, per anchor, how many runs differ from the
# reference and the largest Y difference. Expected for a correct core: 0 differing runs.
set -u
H="$(cd "$(dirname "$0")" && pwd)"; CORE="$H/../.."; R="$CORE/rtl"
W=/tmp/ramp_pause; mkdir -p "$W"
SWEEP=${SWEEP:-260}; FINE=${FINE:-48}; LEN=${STALL_LEN:-480}     # stall starts: every clock below FINE, then every 4th
python3 "$H/make_variants.py" "$R/vectrex.vhd" "$W/src" > /dev/null || exit 1
G="ghdl -a --std=08 -frelaxed -fsynopsys -Wno-hide -Wno-shared"
for v in ${@:-before cur}; do
	L="$W/$v"; rm -rf "$L"; mkdir -p "$L"
	case $v in cur) VX="$R/vectrex.vhd";; *) VX="$W/src/vectrex_$v.vhd";; esac
	( cd "$L" && $G "$H/stubs.vhd" "$R/via6522.vhd" "$R/bios_rom.vhd" "$R/bios_factory_rom.vhd" "$R/vectrex_analog_frontend.vhd" \
		"$R/sp0256_al2_decoded.vhd" "$R/sp0256.vhd" "$R/vectrex_cartridge.vhd" "$VX" "$H/tb_ramp_pause.vhd" \
		&& ghdl -e --std=08 -frelaxed -fsynopsys tb_ramp_pause ) > "$L/build.log" 2>&1 || { echo "$v: BUILD FAILED"; tail -15 "$L/build.log"; exit 1; }
	run() { (cd "$L" && ghdl -r --std=08 -fsynopsys tb_ramp_pause "$@" 2>&1 | grep -o 'RESULT.*'); }
	ref=$(run); echo "[$v] reference:            $ref"
	refv=${ref#*stall_at=-1 }
	for anc in 0 1; do
		name=$([ $anc = 0 ] && echo "RAMP on " || echo "RAMP off")
		diff=0; worst=0; tab=""; n=0; bad=""
		for ((s = 0; s <= SWEEP; s += (s < FINE ? 1 : 4))); do
			n=$((n + 1))
			r=$(run -gSTALL_AT=$s -gSTALL_ANCHOR=$anc -gSTALL_LEN=$LEN); rv=${r#*stall_at=$s }
			y=${rv#y=}; y=${y%% *}; ry=${refv#y=}; ry=${ry%% *}
			[ "$rv" != "$refv" ] && { diff=$((diff + 1)); [ $diff -le 8 ] && bad="$bad $s:${rv// /,}"; }
			d=$((y - ry)); [ ${d#-} -gt ${worst#-} ] && worst=$d
			[ $((s % 12)) = 0 ] && [ $s -lt 192 ] && tab="$tab $s:$y"
		done
		echo "[$v] stall after $name: $diff of $n runs differ, largest Y error $worst"
		echo "[$v]   end Y at 16 phases (clock:y):$tab"
		[ -n "$bad" ] && echo "[$v]   first differing runs:$bad"
	done
done

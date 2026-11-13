#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# RAMP pause test, pause-length sweep: sim/ramp_pause/sweep.sh [VARIANT ...]   (GHDL 5; default: before before_free free cur)
#   cur = rtl/vectrex.vhd as it is (fixed: the phase freezes during a pause in both modes)
#   before, before_free, free: the comparison versions from make_variants.py
# Every pause length in LENS, started at every clock in STARTS after the E-cycle that presents the RAMP-on write
# (anchor 0) and the RAMP-off write (anchor 1). Prints, per variant, anchor and length, how many runs differ from the
# no-pause reference and the largest Y error. Expected for a correct core: 0 everywhere.
# Short and odd lengths matter: a length that is a multiple of 16 clocks (one E period, four cpu_phase periods)
# cannot show a phase that runs on during the pause.
# Do not edit rtl/ while it runs: GHDL (mcode) re-checks the sources at every run and refuses changed ones.
set -u
H="$(cd "$(dirname "$0")" && pwd)"; CORE="$H/../.."; R="$CORE/rtl"
W=/tmp/ramp_pause; mkdir -p "$W"
LENS=${LENS:-"1 2 3 4 5 7 8 15 16 17 31 33 229 481"}
STARTS=${STARTS:-"$(seq -s ' ' 0 19) $(seq -s ' ' 23 12 259)"}   # every clock for 20 (over one E period), then every 12th
JOBS=${JOBS:-6}
python3 "$H/make_variants.py" "$R/vectrex.vhd" "$W/src" > /dev/null || exit 1
G="ghdl -a --std=08 -frelaxed -fsynopsys -Wno-hide -Wno-shared"
for v in ${@:-before before_free free cur}; do
	L="$W/$v"; rm -rf "$L"; mkdir -p "$L"
	case $v in cur) VX="$R/vectrex.vhd";; *) VX="$W/src/vectrex_$v.vhd";; esac
	( cd "$L" && $G "$H/stubs.vhd" "$R/via6522.vhd" "$R/bios_rom.vhd" "$R/bios_factory_rom.vhd" "$R/vectrex_analog_frontend.vhd" \
		"$R/sp0256_al2_decoded.vhd" "$R/sp0256.vhd" "$R/vectrex_cartridge.vhd" "$VX" "$H/tb_ramp_pause.vhd" \
		&& ghdl -e --std=08 -frelaxed -fsynopsys tb_ramp_pause ) > "$L/build.log" 2>&1 || { echo "$v: BUILD FAILED"; tail -15 "$L/build.log"; exit 1; }
	ref=$(cd "$L" && ghdl -r --std=08 -fsynopsys tb_ramp_pause 2>&1 | grep -o 'RESULT.*')
	echo "[$v] reference: $ref"
	# one line per run: anchor len start y x ramp_ticks lit_ticks
	for anc in 0 1; do for len in $LENS; do for s in $STARTS; do echo "$anc $len $s"; done; done; done |
	xargs -P "$JOBS" -n 3 sh -c 'cd "$0" && r=$(ghdl -r --std=08 -fsynopsys tb_ramp_pause -gSTALL_ANCHOR=$1 -gSTALL_LEN=$2 -gSTALL_AT=$3 2>&1 | grep -o "RESULT.*") &&
		echo "$1 $2 $3 $(echo "$r" | sed "s/.*y=\([-0-9]*\) x=\([-0-9]*\) ramp_ticks=\([0-9]*\) lit_ticks=\([0-9]*\)/\1 \2 \3 \4/")"' "$L" > "$L/runs.txt"
	echo "$ref" | sed 's/.*y=\([-0-9]*\) x=\([-0-9]*\) ramp_ticks=\([0-9]*\) lit_ticks=\([0-9]*\)/\1 \2 \3 \4/' > "$L/ref.txt"
	python3 - "$L/ref.txt" "$L/runs.txt" "$v" $((2 * $(echo $LENS | wc -w) * $(echo $STARTS | wc -w))) <<'PY'
import sys, collections
ref = open(sys.argv[1]).read().split()
rows = [l.split() for l in open(sys.argv[2]) if len(l.split()) == 7]
v = sys.argv[3]
agg = collections.OrderedDict()
for a, n, s, *res in sorted(rows, key=lambda r: (int(r[0]), int(r[1]), int(r[2]))):
    k = (a, int(n)); d = agg.setdefault(k, [0, 0, 0, []])
    d[0] += 1
    if res != ref:
        d[1] += 1; e = int(res[0]) - int(ref[0])
        if abs(e) > abs(d[2]): d[2] = e
        if len(d[3]) < 4: d[3].append("%s:%+d" % (s, e))
total = sum(d[1] for d in agg.values())
print("[%s] %d runs, %d differ from the reference" % (v, len(rows), total))
if len(rows) != int(sys.argv[4]):
    print("[%s]   INCOMPLETE: %d of %d runs gave no result (see a single run by hand)" % (v, int(sys.argv[4]) - len(rows), int(sys.argv[4])))
for (a, n), d in agg.items():
    if d[1]:
        print("[%s]   %s len %3d: %2d of %d differ, largest Y error %+d  (first: %s)"
              % (v, "RAMP on " if a == "0" else "RAMP off", n, d[1], d[0], d[2], " ".join(d[3])))
PY
done

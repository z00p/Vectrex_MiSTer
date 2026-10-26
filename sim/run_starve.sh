#!/bin/bash
# Starvation sweep (B8):  sim/run_starve.sh old|new [stream.ptxs]
#   old = vfb_ddr_arbiter.sv + pitrex_ddr.sv from git HEAD (or $OLD_REV, default HEAD = B7 before B8 edits are
#         committed; use f4e0718 afterwards), composition ABOVE PiTrex
#   new = working tree
# Sweeps the length of a composition-like burst window (out of every 20 ms) and the DDR read latency and
# prints, per trial, the command-FIFO underruns while replaying the calibration screen's recorded stream.
set -u
which=${1:-new}
PTXS=${2:-/mnt/c/AMD/pitrex-mister/build/calibrate.ptxs}
OLD_REV=${OLD_REV:-f4e0718}
CORE="$(cd "$(dirname "$0")/.." && pwd)"
W=/tmp/ptxsim/starve_$which; rm -rf "$W"; mkdir -p "$W/rtl"
FB="$CORE/rtl/videodr0me_fb"
if [ "$which" = old ]; then
	git -C "$CORE" show "$OLD_REV:rtl/videodr0me_fb/vfb_ddr_arbiter.sv" > "$W/rtl/vfb_ddr_arbiter.sv"
	git -C "$CORE" show "$OLD_REV:rtl/pitrex/pitrex_ddr.sv" > "$W/rtl/pitrex_ddr.sv"
else
	cp "$FB/vfb_ddr_arbiter.sv" "$W/rtl/"; cp "$CORE/rtl/pitrex/pitrex_ddr.sv" "$W/rtl/"
fi
python3 - "$PTXS" "$W/cal.hex" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    for (c,) in list(struct.iter_unpack("<Q", d[:len(d) // 8 * 8]))[:65536]:
        f.write('%016x\n' % c)
PY
sed "s/ .drop_cause(7.d0),//" "$CORE/sim/tb_ptx_starve.sv" > "$W/tb_old.sv"
if [ "$which" = old ]; then cp "$W/tb_old.sv" "$W/tb.sv"; else cp "$CORE/sim/tb_ptx_starve.sv" "$W/tb.sv"; fi
common="-Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-TIMESCALEMOD -Wno-INITIALDLY -Wno-BLKSEQ --timing --binary -O1"
( cd "$W" && verilator $common --top-module tb_ptx_starve --Mdir obj -o sim "$FB/vfb_control_pkg.sv" "$FB/vfb_color_pkg.sv" \
	"$FB/vfb_layout_pkg.sv" "$FB/vfb_async_fifo.sv" "$W/rtl/vfb_ddr_arbiter.sv" "$W/rtl/pitrex_ddr.sv" \
	"$CORE/rtl/pitrex/pitrex_player.sv" "$W/tb.sv" ) > "$W/build.log" 2>&1 \
	|| { echo "BUILD FAILED, see $W/build.log"; tail -20 "$W/build.log"; exit 1; }
# hold=1: composition requests held high for the whole window (pathological starvation bound)
for hold in 1; do
	for off in ${OFFS:-500 4000 8000}; do for win in ${WINS:-200 500 1000 3000}; do
		echo -n "off=$off "; "$W/obj/sim" +hex=$W/cal.hex +n=12000 +run_us=70000 +win=$win +off=$off +period=20000 +lat=${LAT:-30} +hold=$hold | grep -E "trial"
	done; done
done

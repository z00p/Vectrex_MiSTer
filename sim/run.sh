#!/bin/bash
# Run the PiTrex testbenches with Verilator (WSL):  sim/run.sh old|new [ddr|entry|audio|reset|all]
#   old = pre-fix RTL from git rev $OLD_REV (default 8ca9ec9, build B6): shows the audit cases failing
#   new = the working tree
# Build products go to /tmp/ptxsim (outside the repo).
set -u
which=${1:-new}; what=${2:-all}
OLD_REV=${OLD_REV:-8ca9ec9}
CORE="$(cd "$(dirname "$0")/.." && pwd)"
W=/tmp/ptxsim/$which; rm -rf "$W"; mkdir -p "$W/rtl"
if [ "$which" = old ]; then
	for f in pitrex_ddr pitrex_player pitrex_audio; do
		git -C "$CORE" show "$OLD_REV:rtl/pitrex/$f.sv" > "$W/rtl/$f.sv"
	done
	DEF=""
else
	for f in pitrex_ddr pitrex_player pitrex_audio; do cp "$CORE/rtl/pitrex/$f.sv" "$W/rtl/"; done
	DEF="-DNEW"
fi
FB="$CORE/rtl/videodr0me_fb"
common="-Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-TIMESCALEMOD -Wno-INITIALDLY -Wno-BLKSEQ --timing --binary -O1"
fail=0
build() { # name top files...
	local name=$1 top=$2; shift 2
	( cd "$W" && verilator $common $DEF --top-module "$top" --Mdir "obj_$name" -o "sim_$name" "$@" ) > "$W/build_$name.log" 2>&1 \
		|| { echo "BUILD FAILED ($name), see $W/build_$name.log"; tail -20 "$W/build_$name.log"; fail=1; return 1; }
}
run() { # name args...
	local name=$1; shift
	"$W/obj_$name/sim_$name" "$@" | grep -E "CASE|check|RESULT|dbg|trial"
}
if [ "$what" = ddr ] || [ "$what" = all ]; then
	build ddr tb_ptx_ddr "$FB/vfb_control_pkg.sv" "$FB/vfb_color_pkg.sv" "$FB/vfb_layout_pkg.sv" \
		"$FB/vfb_ddr_arbiter.sv" "$W/rtl/pitrex_ddr.sv" "$CORE/sim/tb_ptx_ddr.sv" && {
		for c in 0 1 2 3 4 5 6 7; do
			[ "$which" = old ] && { [ $c = 5 ] || [ $c = 7 ]; } && continue
			run ddr +case=$c
		done; }
fi
if [ "$what" = entry ] || [ "$what" = all ]; then
	build entry tb_ptx_entry "$FB/vfb_async_fifo.sv" "$W/rtl/pitrex_player.sv" "$CORE/sim/tb_ptx_entry.sv" && run entry
fi
if [ "$what" = audio ] || [ "$what" = all ]; then
	build audio tb_ptx_audio "$W/rtl/pitrex_audio.sv" "$CORE/sim/tb_ptx_audio.sv" && run audio
fi
if [ "$what" = reset ] || [ "$what" = all ]; then
	build reset tb_ptx_reset "$CORE/rtl/pitrex/pitrex_reset.sv" "$CORE/sim/tb_ptx_reset.sv" && run reset
fi
exit $fail

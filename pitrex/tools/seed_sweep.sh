#!/bin/bash
# Re-run fit + assemble + timing with different placement seeds until timing is met.
# Synthesis is reused (refused if the sources changed since it ran).
# Run from anywhere: bash tools/seed_sweep.sh 2 3 4 5
#
# Hardened (B7): every tool exit status is checked and aborts the sweep; stale reports
# and RBFs are removed before each seed; MEETS TIMING is printed only when the current
# run's summary shows numeric setup AND hold slack >= 0 at all four corners with TNS = 0,
# no unconstrained clocks and no ignored Vectrex.sdc constraints. The RBF is hashed.
set -u -o pipefail
cd "$(dirname "$0")/../core" || exit 9
Q=/c/intelFPGA_lite/17.0/quartus/bin64
[ $# -gt 0 ] || { echo "usage: $0 <seed>..."; exit 9; }
mkdir -p build_logs
trap 'git checkout -q -- Vectrex.qsf 2>/dev/null' EXIT

# the shared timing check lives in build_core.sh; extract the function so both stay identical
eval "$(sed -n '/^check_timing() {/,/^}/p' ../tools/build_core.sh)"
type check_timing > /dev/null 2>&1 || { echo "ERROR: cannot load check_timing from build_core.sh"; exit 9; }

# synthesis reuse is only valid if no source changed after it (qsf excluded: Quartus rewrites it)
map=output_files/Vectrex.map.summary
[ -s "$map" ] || { echo "ERROR: no synthesis result ($map); run build_core.sh first"; exit 9; }
newer=$(find rtl Vectrex.sv files.qip -type f \( -name '*.sv' -o -name '*.v' -o -name '*.vhd' -o -name '*.qip' -o -name '*.hex' -o -name '*.mif' \) -newer "$map" | head -3)
[ -z "$newer" ] || { echo "ERROR: sources changed after synthesis, rerun the full compile:"; echo "$newer"; exit 9; }

for seed in "$@"; do
	start=$(date +%s)
	log="build_logs/seed_$seed.log"; : > "$log"
	rm -f output_files/Vectrex.rbf output_files/Vectrex.sof output_files/Vectrex.sta.summary output_files/Vectrex.sta.rpt
	for step in "quartus_fit Vectrex --seed=$seed" "quartus_asm Vectrex" "quartus_sta Vectrex --multicorner=on"; do
		set -- $step
		"$Q/$1.exe" "${@:2}" >> "$log" 2>&1 || { echo "seed $seed: ERROR $1 failed, see core/$log"; tail -20 "$log"; exit 1; }
	done
	[ -s output_files/Vectrex.rbf ] && [ "$(stat -c %Y output_files/Vectrex.rbf)" -ge "$start" ] \
		|| { echo "seed $seed: ERROR Vectrex.rbf missing or not produced by this run"; exit 1; }
	cp output_files/Vectrex.rbf "output_files/Vectrex_seed$seed.rbf" || exit 1
	echo "seed $seed: $(sha256sum "output_files/Vectrex_seed$seed.rbf" | cut -c1-16) ($(( ($(date +%s) - start) / 60 )) min)"
	check_timing "$start" "$log"; rc=$?
	if [ $rc -eq 0 ]; then
		echo "seed $seed MEETS TIMING -> output_files/Vectrex_seed$seed.rbf"
		exit 0
	fi
	echo "seed $seed: timing not met or not verifiable (rc=$rc)"
done
echo "no seed met timing"
exit 1

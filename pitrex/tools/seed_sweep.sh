#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Re-run fit + assemble + timing with different placement seeds until timing is met.
# Synthesis is reused. Run from anywhere: bash tools/seed_sweep.sh 2 3 4 5
cd "$(dirname "$0")/../.." || exit 9   # repository root = core project
Q=/c/intelFPGA_lite/17.0/quartus/bin64
for seed in "$@"; do
	start=$(date +%s)
	"$Q/quartus_fit.exe" Vectrex --seed="$seed" > "seed_$seed.log" 2>&1 || { echo "seed $seed: fit failed"; continue; }
	"$Q/quartus_asm.exe" Vectrex >> "seed_$seed.log" 2>&1
	"$Q/quartus_sta.exe" Vectrex --multicorner=on >> "seed_$seed.log" 2>&1   # all four corners
	worst=$(awk '/^Type/ {t=$0} /Slack/ {print $3}' output_files/Vectrex.sta.summary | sort -n | head -1)
	mins=$(( ($(date +%s) - start) / 60 ))
	cp output_files/Vectrex.rbf "output_files/Vectrex_seed$seed.rbf"
	echo "seed $seed: worst slack $worst ns ($mins min)"
	if awk -v w="$worst" 'BEGIN { exit !(w >= 0) }'; then
		echo "seed $seed MEETS TIMING -> output_files/Vectrex_seed$seed.rbf"
		exit 0
	fi
done
echo "no seed met timing"
exit 1

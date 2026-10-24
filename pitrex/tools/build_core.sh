#!/bin/bash
# Full compile with the qsf seed, multicorner timing, then a seed sweep if needed.
# Keeps the committed qsf (Quartus rewrites it). Usage: bash tools/build_core.sh <tag> [sweep seeds...]
#
# Hardened (B7): every tool exit status is checked; stale reports/RBFs from earlier
# runs are removed first and anything older than this run is rejected; timing is only
# declared met when the current run's summary has a numeric setup AND hold slack at all
# four corners (all >= 0, TNS = 0), no unconstrained clocks and no ignored Vectrex.sdc
# constraints. Missing or malformed data is a failure, never a pass.
set -u -o pipefail
cd "$(dirname "$0")/../core" || exit 9
Q=/c/intelFPGA_lite/17.0/quartus/bin64
if [ $# -lt 1 ]; then echo "usage: $0 <tag> [sweep seeds...]"; exit 9; fi
tag="$1"; shift
mkdir -p build_logs
trap 'git checkout -q -- Vectrex.qsf 2>/dev/null' EXIT

# check_timing <run start epoch> <sta log>: prints a per-corner table; returns 0 only if complete and met
check_timing() {
	local t0="$1" log="$2" sum=output_files/Vectrex.sta.summary rpt=output_files/Vectrex.sta.rpt f
	for f in "$sum" "$rpt"; do
		[ -s "$f" ] || { echo "ERROR: $f missing or empty"; return 2; }
		[ "$(stat -c %Y "$f")" -ge "$t0" ] || { echo "ERROR: $f is older than this run (stale)"; return 2; }
	done
	[ -s "$log" ] || { echo "ERROR: $log missing or empty"; return 2; }
	if grep -E "Vectrex\.sdc" "$log" | grep -Eiq "ignored|empty collection|zero elements|could not be matched"; then
		echo "ERROR: Vectrex.sdc constraints were ignored (collection did not resolve):"
		grep -E "Vectrex\.sdc" "$log" | grep -Ei "ignored|empty collection|zero elements|could not be matched"
		return 1
	fi
	local unc
	unc=$(awk -F';' '/; Unconstrained Clocks/ {gsub(/ /,"",$3); gsub(/ /,"",$4); print $3 "," $4; exit}' "$rpt")
	[ "$unc" = "0,0" ] || { echo "ERROR: unconstrained clocks in report (got '$unc')"; return 1; }
	awk '
		/^Type[ ]*:/ {
			rest = $0; sub(/^Type[ ]*: /, "", rest)
			if (!match(rest, /^(Slow|Fast) [0-9]+mV -?[0-9]+C Model /)) { bad++; cur = ""; next }
			corner = substr(rest, 1, RLENGTH - 7); r2 = substr(rest, RLENGTH + 1)
			i = index(r2, " \047"); kind = (i > 0) ? substr(r2, 1, i - 1) : r2
			cur = corner SUBSEP kind; have = 1; next
		}
		/^Slack[ ]*:/ { v = $0; sub(/^Slack[ ]*: /, "", v); sl = v; next }
		/^TNS[ ]*:/ {
			t = $0; sub(/^TNS[ ]*: /, "", t)
			if (cur == "") next
			if (sl !~ /^-?[0-9]+(\.[0-9]+)?$/ || t !~ /^-?[0-9]+(\.[0-9]+)?$/) { bad++; cur = ""; next }
			if (!(cur in w) || sl + 0 < w[cur]) w[cur] = sl + 0
			if (t + 0 < 0) tns[cur] = t + 0
			if (!(cur in w0)) { split(cur, p, SUBSEP); corners[p[1]] = 1; kinds[p[2]] = 1 }
			w0[cur] = 1
			cur = ""
		}
		END {
			n = 0; for (c in corners) n++
			rc = 0
			if (bad) { printf "ERROR: %d malformed slack records\n", bad; rc = 2 }
			if (n != 4) { printf "ERROR: expected 4 timing corners, found %d\n", n; rc = 2 }
			for (c in corners) {
				for (k = 1; k <= 2; k++) {
					kind = (k == 1) ? "Setup" : "Hold"
					key = c SUBSEP kind
					if (!(key in w)) { printf "ERROR: no %s slack for corner %s\n", kind, c; rc = 2; continue }
					printf "  %-22s %-5s worst slack %7.3f ns%s\n", c, kind, w[key], (w[key] < 0 ? "  VIOLATED" : "")
				}
			}
			worst = ""
			for (key in w) { if (worst == "" || w[key] < worst) worst = w[key] }
			if (worst == "") { print "ERROR: no slack data at all"; rc = 2 } else printf "  overall worst (all analysis types): %.3f ns\n", worst
			if (rc == 0) {
				for (key in w) if (w[key] < 0) rc = 1
				for (key in tns) rc = 1
			}
			exit rc
		}' "$sum"
}

# run_tool <log> <tool> args...: appends to the log, aborts the whole script on failure
run_tool() {
	local log="$1"; shift
	"$Q/$1.exe" "${@:2}" >> "$log" 2>&1 || { echo "ERROR: $1 failed (exit $?), see core/$log"; tail -20 "$log"; exit 1; }
}

start=$(date +%s)
log="build_logs/build_$tag.log"; : > "$log"
rm -f output_files/Vectrex.rbf output_files/Vectrex.sof output_files/Vectrex.sta.summary output_files/Vectrex.sta.rpt
run_tool "$log" quartus_sh --flow compile Vectrex
run_tool "$log" quartus_sta Vectrex --multicorner=on
[ -s output_files/Vectrex.rbf ] && [ "$(stat -c %Y output_files/Vectrex.rbf)" -ge "$start" ] \
	|| { echo "ERROR: Vectrex.rbf missing or not produced by this run"; exit 1; }
cp output_files/Vectrex.rbf "output_files/Vectrex_$tag.rbf" || exit 1
echo "$tag: $(sha256sum "output_files/Vectrex_$tag.rbf" | cut -c1-16) ($(( ($(date +%s) - start) / 60 )) min)"
check_timing "$start" "$log"; rc=$?
if [ $rc -eq 0 ]; then
	echo "$tag MEETS TIMING -> output_files/Vectrex_$tag.rbf"
	exit 0
fi
echo "$tag: timing NOT met or not verifiable (check_timing rc=$rc)"
if [ $# -gt 0 ]; then
	bash ../tools/seed_sweep.sh "$@"
	exit $?
fi
exit 1

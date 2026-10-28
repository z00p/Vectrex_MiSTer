#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Mock test for scripts/pitrexd.sh on a PC (bash, python3 and GNU coreutils; WSL is fine). Nothing touches a MiSTer:
# pitrexd runs against a scratch directory (the PITREXD_* test hooks), with a fake Main that writes /tmp/FILESELECT & co,
# a fake core loader and a fake mister_run.sh that only records how it was called.
#   tests/pitrexd_mock.sh        expected: every line PASS, exit status 0 (takes about 90 s)
# Covers: a fresh .um2 pick starts; a stale "selected" report (Main restart / core load) is ignored; orient= / view= in the
# sidecar change only Vectrex-PiTrex_v1.CFG (right bits, other bytes kept) and reload the core before um2run starts; the same
# setting twice reloads once; a core running as VECTREX is left alone; the game started after the reload is not killed by the
# core-change supervisor; a real core change still stops the game.
D=$(cd "$(dirname "$0")" && pwd)
PD=$D/../scripts/pitrexd.sh
[ -f "$PD" ] || PD=$D/../pitrex/scripts/pitrexd.sh
W=$(mktemp -d /tmp/pitrexd_mock.XXXXXX)
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails+1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1   [$2]"; fi; }

export PITREXD_RT=1 PITREXD_FAT=$W/fat PITREXD_TMP=$W/tmp PITREXD_CMD=$W/cmd PITREXD_CPUS=$W/cpus
FAT=$PITREXD_FAT; T=$PITREXD_TMP
mkdir -p "$FAT/pitrex/um2run" "$FAT/pitrex/uvmc2" "$FAT/config" "$FAT/_Unstable" "$FAT/games/Vectrex-PiTrex/UVMC2" "$T" "$W/bin"
echo 0-1 > "$W/cpus"; : > "$W/cmd"
echo Vectrex-PiTrex > "$T/CORENAME"
touch "$FAT/pitrex/um2run/um2run" "$FAT/_Unstable/Vectrex-PiTrex_20261027.rbf" "$FAT/_Unstable/Vectrex-PiTrex_20261029.rbf"
UD=$FAT/games/Vectrex-PiTrex/UVMC2

# fake mister_run.sh: record the call, start a "game" (a sleep) and remember its pid
cat > "$FAT/pitrex/um2run/mister_run.sh" <<EOF
#!/bin/sh
echo "run \$* settle=\$SETTLE c1=\$CPU1_AFTER core=\$(cat $T/CORENAME)" >> $W/runs.log
sleep 600 &
echo \$! > $W/game.pid
EOF
# fake ps: lists the game the way MiSTer's ps shows um2run
cat > "$W/bin/ps" <<EOF
#!/bin/sh
echo "  PID USER TIME COMMAND"
[ -f $W/game.pid ] && kill -0 \$(cat $W/game.pid) 2>/dev/null && echo "\$(cat $W/game.pid) root 0:01 ./um2run --bus --hold game.um2"
exit 0
EOF
chmod +x "$W/bin/ps"; export PATH=$W/bin:$PATH

# fake core loader: when pitrexd writes load_core to the command file, "load" the core one second later
( while :; do
	if [ -s "$W/cmd" ]; then cat "$W/cmd" >> "$W/cmds.log"; : > "$W/cmd"; echo -n MENU > "$T/CORENAME"; sleep 1; echo -n Vectrex-PiTrex > "$T/CORENAME"; fi
	sleep 0.1
  done ) &
LOADER=$!
cleanup() { kill $LOADER $DPID 2>/dev/null; [ -f "$W/game.pid" ] && kill "$(cat "$W/game.pid")" 2>/dev/null; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

# fake Main: a fresh pick writes the path files, then FILESELECT (all within the same second)
pick() { echo -n "$1" > "$T/FULLPATH"; [ -n "$2" ] && echo -n "$2" > "$T/CURRENTPATH"; sleep 0.3; echo -n selected > "$T/FILESELECT"; }
runs() { [ -f "$W/runs.log" ] && wc -l < "$W/runs.log" || echo 0; }
waitfor() { local i=0; while [ $i -lt $(( $2 * 10 )) ]; do eval "$1" && return 0; sleep 0.1; i=$((i+1)); done; return 1; }
bytes() { od -An -tx1 -N16 "$FAT/config/Vectrex-PiTrex_v1.CFG" 2>/dev/null | tr -d ' \n'; }
gamealive() { [ -f "$W/game.pid" ] && kill -0 "$(cat "$W/game.pid")" 2>/dev/null; }

bash "$PD" > "$W/pitrexd.out" 2>&1 &
DPID=$!
sleep 1

touch "$UD/Plain.um2" "$UD/Land.um2" "$UD/Port.um2"
echo "frame_cycles=25000" > "$UD/Plain.um2.cfg"
printf 'orient=cw\r\nview=open\r\n' > "$UD/Land.um2.cfg"
echo "orient=0" > "$UD/Port.um2.cfg"

echo "== 1. fresh OSD pick (folder in FULLPATH, file in CURRENTPATH)"
n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain.um2
check "plain pick starts mister_run.sh with the sidecar frame length" 'waitfor "[ \$(runs) -gt $n ]" 6 && tail -1 "$W/runs.log" | grep -q -- "--frame-cycles 25000" && tail -1 "$W/runs.log" | grep -q "Plain.um2"'
check "no core reload without orient/view" '[ ! -s "$W/cmds.log" ] && [ ! -f "$FAT/config/Vectrex-PiTrex_v1.CFG" ]'

echo "== 2. stale report after a Main restart: FILESELECT rewritten, path files old"
sleep 1; n=$(runs)
touch -d '2 minutes ago' "$T/FULLPATH" "$T/CURRENTPATH"
echo -n selected > "$T/FILESELECT"
sleep 2
check "stale report ignored (no new start)" '[ $(runs) -eq $n ]'
check "ignore is logged" 'grep -q "ignored a stale file report" "$T/pitrexd.log"'
echo -n selected > "$T/FILESELECT"; sleep 1.5
check "a second stale report is also ignored" '[ $(runs) -eq $n ]'

echo "== 3. a genuine re-pick of the same file right after is a new pick (paths rewritten)"
sleep 11   # past the 10 s double-report guard
pick games/Vectrex-PiTrex/UVMC2 Plain.um2
check "re-pick starts again" 'waitfor "[ \$(runs) -gt $n ]" 6'

echo "== 4. MGL-style pick: absolute FULLPATH, no CURRENTPATH rewrite needed"
sleep 11; n=$(runs); touch -d '5 minutes ago' "$T/CURRENTPATH"
pick "$UD/Plain.um2" ""
check "MGL pick with a fresh FULLPATH starts" 'waitfor "[ \$(runs) -gt $n ]" 6'

echo "== 5. orient=cw view=open on the Vectrex-PiTrex core (settings file empty)"
sleep 11; n=$(runs); : > "$W/cmds.log"
t0=$(date +%s)
pick games/Vectrex-PiTrex/UVMC2 Land.um2
check "mister_run.sh runs after the reload" 'waitfor "[ \$(runs) -gt $n ]" 12'
check "core was reloaded through an MGL naming the newest rbf" 'grep -q "load_core $T/pitrex_core.mgl" "$W/cmds.log" && grep -q "Vectrex-PiTrex_20261029" "$T/pitrex_core.mgl" && grep -q "<setname>Vectrex-PiTrex</setname>" "$T/pitrex_core.mgl"'
check "CFG bits: byte2 0x20 (orientation 2 = cw), byte3 0x80 (open matte), byte4 0x10 (PiTrex mode)" '[ "$(bytes)" = "00002080100000000000000000000000" ]'
check "um2run was started after the CORENAME change (core=Vectrex-PiTrex, takes >= 1 s)" 'tail -1 "$W/runs.log" | grep -q "core=Vectrex-PiTrex" && [ $(( $(date +%s) - t0 )) -ge 1 ]'
check "pitrexd logged the update" 'grep -q "orient=cw view=open: Vectrex-PiTrex_v1.CFG updated" "$T/pitrexd.log"'
sleep 2
check "the supervisor did not kill the game started after the reload" 'gamealive'
check "VECTREX_v1.CFG was not created" '[ ! -e "$FAT/config/VECTREX_v1.CFG" ]'

echo "== 6. same sidecar again: settings already right, no reload"
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Land.um2
check "started without a reload" 'waitfor "[ \$(runs) -gt $n ]" 6 && [ ! -s "$W/cmds.log" ]'

echo "== 7. read-modify-write: Main saved other options meanwhile; orient=0 only clears orientation"
printf '\x11\x22\x23\xe5\x13\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$FAT/config/Vectrex-PiTrex_v1.CFG"
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Port.um2
check "orient=0 reloads and clears only bits 5:4 of byte 2 (0x23 -> 0x03; view untouched)" 'waitfor "[ \$(runs) -gt $n ]" 12 && [ "$(bytes)" = "112203e5130000000000000000000000" ] && grep -q load_core "$W/cmds.log"'

echo "== 8. core runs as VECTREX: skip, never touch VECTREX_v1.CFG"
echo VECTREX > "$T/CORENAME"
printf '\x00\x00\x00\x00\x00' > "$FAT/config/VECTREX_v1.CFG"; before=$(bytes); sum=$(md5sum < "$FAT/config/VECTREX_v1.CFG")
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Land.um2
check "game still starts" 'waitfor "[ \$(runs) -gt $n ]" 6'
check "no reload, both CFGs unchanged, skip logged" '[ ! -s "$W/cmds.log" ] && [ "$(bytes)" = "$before" ] && [ "$(md5sum < "$FAT/config/VECTREX_v1.CFG")" = "$sum" ] && grep -q "orient/view skipped: the core runs as .VECTREX." "$T/pitrexd.log"'

echo "== 9. a real core change away from ours stops the game"
echo Vectrex-PiTrex > "$T/CORENAME"; sleep 1
check "game running before" 'gamealive'
echo MENU > "$T/CORENAME"; sleep 1.5
check "game stopped by the supervisor" '! gamealive && grep -q "core changed away" "$T/pitrexd.log"'

echo "== 10. card image named in the sidecar is found next to the .um2 file"
echo Vectrex-PiTrex > "$T/CORENAME"; touch "$UD/card.img" "$UD/Card.um2"; echo "sd=card.img" > "$UD/Card.um2.cfg"
sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Card.um2
check "mister_run.sh gets --sd <folder>/card.img" 'waitfor "[ \$(runs) -gt $n ]" 6 && tail -1 "$W/runs.log" | grep -q -- "--sd $UD/card.img"'

echo "== 11. refusals"
echo 0 > "$W/cpus"
sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain.um2; sleep 2
check "one CPU online: refused" '[ $(runs) -eq $n ] && grep -q "REFUSED Plain.um2" "$T/pitrexd.log"'

echo
if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails FAILED"; echo "--- pitrexd.log"; cat "$T/pitrexd.log"; echo "--- pitrexd.out"; cat "$W/pitrexd.out"; fi
exit $(( fails > 0 ))

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Mock test for scripts/pitrexd.sh on a PC (bash, python3 and GNU coreutils; WSL is fine). Nothing touches a MiSTer:
# pitrexd runs against a scratch directory (the PITREXD_* test hooks), with a fake Main that writes /tmp/FILESELECT & co,
# a fake core loader and a fake mister_run.sh that only records how it was called.
#   tests/pitrexd_mock.sh        expected: every line PASS, exit status 0 (takes about 2.5 minutes)
# Overlay art (scripts/art_lib.sh): a reload for orient/view loads the game's art again (an index 2 file line in the MGL, none without art);
# art lookup order (sidecar overlay=, beside the game, Overlays folder); the copy step that puts the art next to a game (nothing for a game
# without art, a .art of yours is kept, a rotated game gets the Tate version or none); make_card.sh leaves .art files off the card.
# Also: a pick that cannot start says so on the Vectrex; our core started without the menu entry is loaded again as Vectrex-PiTrex (the official core is not); Punch-Out needs no sidecar;
# no forks from one poll to the next.
# Covers: a fresh .um2 pick starts; a stale "selected" report (Main restart / core load) is ignored; orient= / view= in the
# sidecar change only Vectrex-PiTrex_v2.CFG (right bits, other bytes kept) and reload the core before um2run starts; the same
# setting twice reloads once; a core running as VECTREX is left alone; the game started after the reload is not killed by the
# core-change supervisor; a real core change still stops the game.
D=$(cd "$(dirname "$0")" && pwd)
PD=$D/../scripts/pitrexd.sh
[ -f "$PD" ] || PD=$D/../pitrex/scripts/pitrexd.sh
LIB=${PD%/*}/art_lib.sh
MC=${PD%/*}/make_card.sh
W=$(mktemp -d /tmp/pitrexd_mock.XXXXXX)
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails+1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1   [$2]"; fi; }

export PITREXD_RT=1 PITREXD_FAT=$W/fat PITREXD_TMP=$W/tmp PITREXD_CMD=$W/cmd PITREXD_CPUS=$W/cpus PITREXD_MAINCMD=$W/maincmd
FAT=$PITREXD_FAT; T=$PITREXD_TMP
mkdir -p "$FAT/pitrex/um2run" "$FAT/pitrex/uvmc2" "$FAT/config" "$FAT/_Console" "$FAT/_Unstable" "$FAT/games/Vectrex-PiTrex/UVMC2" "$T" "$W/bin"
echo 0-1 > "$W/cpus"; : > "$W/cmd"
echo Vectrex-PiTrex > "$T/CORENAME"
# 1.0 layout: the core in _Console; an older beta core left in _Unstable must not be picked
touch "$FAT/_Console/Vectrex-PiTrex_20261027.rbf" "$FAT/_Console/Vectrex-PiTrex_20261029.rbf" "$FAT/_Unstable/Vectrex-PiTrex_20261103.rbf"
UD=$FAT/games/Vectrex-PiTrex/UVMC2

# fake mister_run.sh: record the call, start a "game" (a sleep) and remember its pid
cat > "$FAT/pitrex/um2run/mister_run.sh" <<EOF
#!/bin/sh
echo "run \$* settle=\$SETTLE c1=\$CPU1_AFTER core=\$(cat $T/CORENAME)" >> $W/runs.log
sleep 600 &
echo \$! > $W/game.pid
EOF
# fake um2run: only --message gets here (pitrexd's on-screen errors); records the text
cat > "$FAT/pitrex/um2run/um2run" <<EOF
#!/bin/sh
echo "\$2" >> $W/msgs.log
EOF
chmod +x "$FAT/pitrex/um2run/um2run"
# fake ps: lists the game the way MiSTer's ps shows um2run
cat > "$W/bin/ps" <<EOF
#!/bin/sh
echo "  PID USER TIME COMMAND"
[ -f $W/game.pid ] && kill -0 \$(cat $W/game.pid) 2>/dev/null && echo "\$(cat $W/game.pid) root 0:01 ./um2run --bus --hold game.um2"
exit 0
EOF
# fake chrt: real-time priority needs root, and the tests don't need the priority
printf '#!/bin/sh\nshift 2\nexec "$@"\n' > "$W/bin/chrt"
chmod +x "$W/bin/ps" "$W/bin/chrt"; export PATH=$W/bin:$PATH

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
bytes() { od -An -tx1 -N16 "$FAT/config/Vectrex-PiTrex_v2.CFG" 2>/dev/null | tr -d ' \n'; }
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
check "no core reload without orient/view" '[ ! -s "$W/cmds.log" ] && [ ! -f "$FAT/config/Vectrex-PiTrex_v2.CFG" ]'

echo "== 2. stale report after a Main restart: FILESELECT rewritten, path files old"
sleep 1; n=$(runs)
touch -d '2 minutes ago' "$T/FULLPATH" "$T/CURRENTPATH"
echo -n selected > "$T/FILESELECT"
sleep 2
check "stale report ignored (no new start)" '[ $(runs) -eq $n ]'
check "ignore is logged" 'grep -q "ignored a stale file report" "$T/pitrexd.log"'
echo -n selected > "$T/FILESELECT"; sleep 1.5
check "a second stale report is also ignored" '[ $(runs) -eq $n ]'

echo "== 2b. browse into the folder, wait, pick its first file (path files 9 s older than FILESELECT, newer than the core load)"
sleep 11; n=$(runs); ns=$(grep -c "ignored a stale" "$T/pitrexd.log")
echo -n Vectrex-PiTrex > "$T/CORENAME"; touch -d '12 seconds ago' "$T/CORENAME"
echo -n games/Vectrex-PiTrex/UVMC2 > "$T/FULLPATH"; echo -n Plain.um2 > "$T/CURRENTPATH"
touch -d '9 seconds ago' "$T/FULLPATH" "$T/CURRENTPATH"
echo -n selected > "$T/FILESELECT"
check "slow pick of the first file starts" 'waitfor "[ \$(runs) -gt $n ]" 30'
check "and is not logged as stale" '[ $(grep -c "ignored a stale" "$T/pitrexd.log") -eq $ns ]'

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
check "CFG bits: byte9 0x10 (orientation 2 = cw), byte8 0x02 (open matte), byte14 0x20 (PiTrex mode)" '[ "$(bytes)" = "00000000000000000210000000002000" ]'
check "um2run was started after the CORENAME change (core=Vectrex-PiTrex, takes >= 1 s)" 'tail -1 "$W/runs.log" | grep -q "core=Vectrex-PiTrex" && [ $(( $(date +%s) - t0 )) -ge 1 ]'
check "pitrexd logged the update" 'grep -q "orient=cw view=open: Vectrex-PiTrex_v2.CFG updated" "$T/pitrexd.log"'
sleep 2
check "the supervisor did not kill the game started after the reload" 'gamealive'
check "VECTREX_v1.CFG was not created" '[ ! -e "$FAT/config/VECTREX_v1.CFG" ]'

echo "== 6. same sidecar again: settings already right, no reload"
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Land.um2
check "started without a reload" 'waitfor "[ \$(runs) -gt $n ]" 6 && [ ! -s "$W/cmds.log" ]'

echo "== 7. read-modify-write: Main saved other options meanwhile; orient=0 only clears orientation"
printf '\x11\x22\x33\x44\x55\x66\x77\x88\xe7\x15\x00\x00\x00\x00\x21\x00' > "$FAT/config/Vectrex-PiTrex_v2.CFG"
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Port.um2
check "orient=0 reloads and clears only bits 4:3 of byte 9 (0x15 -> 0x05; view untouched)" 'waitfor "[ \$(runs) -gt $n ]" 12 && [ "$(bytes)" = "1122334455667788e705000000002100" ] && grep -q load_core "$W/cmds.log"'

echo "== 7b. Punch-Out without a sidecar, a sidecar that says otherwise"
touch "$UD/Punchout.um2" "$UD/PUNCH2.um2"; echo "frame_cycles=30000" > "$UD/PUNCH2.um2.cfg"
sleep 11; n=$(runs)
pick games/Vectrex-PiTrex/UVMC2 Punchout.um2
check "Punch-Out gets 25000 with no sidecar" 'waitfor "[ \$(runs) -gt $n ]" 6 && tail -1 "$W/runs.log" | grep -q -- "--frame-cycles 25000"'
sleep 11; n=$(runs)
pick games/Vectrex-PiTrex/UVMC2 PUNCH2.um2
check "a sidecar wins" 'waitfor "[ \$(runs) -gt $n ]" 6 && tail -1 "$W/runs.log" | grep -q -- "--frame-cycles 30000"'

echo "== 7c. picks that cannot start say why on screen"
kill "$(cat "$W/game.pid")" 2>/dev/null; sleep 11; n=$(runs)
echo 0 > "$W/cpus"
pick games/Vectrex-PiTrex/UVMC2 Plain.um2
check "one CPU: refused, logged, message" 'waitfor "grep -q \"ONE CPU\" $W/msgs.log" 6 && [ $(runs) -eq $n ] && grep -q REFUSED "$T/pitrexd.log"'
echo 0-1 > "$W/cpus"
sleep 11
pick games/Vectrex-PiTrex/UVMC2 Missing.um2
check "file gone: logged, message" 'waitfor "grep -q \"NOT FOUND\" $W/msgs.log" 6 && [ $(runs) -eq $n ]'
sleep 1; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain.um2
waitfor "[ \$(runs) -gt $n ]" 6
sleep 11; m=$(wc -l < "$W/msgs.log"); echo 0 > "$W/cpus"
pick games/Vectrex-PiTrex/UVMC2 Port.um2; sleep 2
check "no message over a running game" '[ $(wc -l < "$W/msgs.log") -eq $m ] && gamealive'
echo 0-1 > "$W/cpus"

echo "== 7d. no forks between polls, with our core and with another"
fork_rate() { local a b; a=$(awk '/^processes/ {print $2}' /proc/stat); sleep 3; b=$(awk '/^processes/ {print $2}' /proc/stat); echo $(( b - a )); }
quiet_forks() { local before after; before=$(cat /proc/$DPID/stat | awk '{print $10, $12}'); sleep 3; after=$(cat /proc/$DPID/stat | awk '{print $10, $12}'); [ "$before" = "$after" ]; }
check "our core: the daemon itself reaps no children in 3 s of polling" 'quiet_forks'
echo -n MENU > "$T/CORENAME"; sleep 2
check "another core: same" 'quiet_forks'
echo -n Vectrex-PiTrex > "$T/CORENAME"; sleep 2

echo "== 7e. our core started from the cores list runs as VECTREX: loaded again once, the official core is left alone"
kill "$(cat "$W/game.pid")" 2>/dev/null; sleep 1
: > "$W/cmds.log"
echo -n MENU > "$T/CORENAME"; sleep 2
printf '/media/fat/MiSTer\0/media/fat/_Console/Vectrex_20261018.rbf\0' > "$W/maincmd"
echo -n VECTREX > "$T/CORENAME"; sleep 2
check "official core: no reload" '[ ! -s "$W/cmds.log" ]'
echo -n MENU > "$T/CORENAME"; sleep 2
printf '/media/fat/MiSTer\0/media/fat/_Console/Vectrex-PiTrex_20261029.rbf\0/media/fat/_PiTrex/x.mgl\0' > "$W/maincmd"
echo -n VECTREX > "$T/CORENAME"; sleep 2
check "our rbf through an MGL: no reload" '[ ! -s "$W/cmds.log" ]'
echo -n MENU > "$T/CORENAME"; sleep 2
printf '/media/fat/MiSTer\0/media/fat/_Console/Vectrex-PiTrex_20261029.rbf\0' > "$W/maincmd"
echo -n VECTREX > "$T/CORENAME"
check "our rbf with no MGL: reloaded once and comes up as Vectrex-PiTrex" 'waitfor "grep -q load_core $W/cmds.log" 6 && waitfor "[ \"\$(cat $T/CORENAME)\" = Vectrex-PiTrex ]" 6 && sleep 2 && [ $(grep -c load_core "$W/cmds.log") -eq 1 ] && grep -q "without the menu entry" "$T/pitrexd.log"'
rm -f "$W/maincmd"

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
echo 0-1 > "$W/cpus"

kill "$(cat "$W/game.pid")" 2>/dev/null

echo "== 12. a pick says so when the card is out of date"
touch "$FAT/pitrex/uvmc2/sd.img"; printf '#!/bin/sh\nexit 1\n' > "$FAT/pitrex/make_card.sh"
sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain.um2
check "notice, then the game on the current card" 'waitfor "[ \$(runs) -gt $n ]" 9 && grep -q "CARD OUT OF DATE" "$W/msgs.log" && tail -1 "$W/runs.log" | grep -q -- "--sd $FAT/pitrex/uvmc2/sd.img"'
: > "$W/msgs.log"; printf '#!/bin/sh\nexit 0\n' > "$FAT/pitrex/make_card.sh"
sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain.um2
check "no notice when the card is current" 'waitfor "[ \$(runs) -gt $n ]" 6 && [ ! -s "$W/msgs.log" ]'
rm -f "$FAT/pitrex/make_card.sh"; sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain.um2
check "no notice without the builder" 'waitfor "[ \$(runs) -gt $n ]" 6 && [ ! -s "$W/msgs.log" ]'

echo "== 13. B12 OSD pick: one file type per entry, Main reports the name without its extension"
sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Plain
check "Plain (no .um2) starts Plain.um2" 'waitfor "[ \$(runs) -gt $n ]" 6 && tail -1 "$W/runs.log" | grep -q "Plain.um2"'
sleep 11; n=$(runs); pick games/Vectrex-PiTrex/UVMC2 Nothing; sleep 2
check "a name with no matching file is logged, nothing starts" '[ $(runs) -eq $n ] && grep -q "ignored $UD/Nothing: not a .um2 or .ptx file" "$T/pitrexd.log"'

echo "== 14. a reload for orient/view loads the game's art again (MGL file line, index 2)"
mkdir -p "$FAT/games/Vectrex-PiTrex/Overlays" "$W/elsewhere"
touch "$UD/Art&A.um2" "$UD/NoArt.um2" "$UD/Ord.um2" "$UD/Abs.um2"
echo art > "$UD/Art&A.art"; echo art > "$UD/Ord.art"; echo art > "$FAT/games/Vectrex-PiTrex/Overlays/Special.art"; echo art > "$W/elsewhere/Out.art"
echo "orient=cw" > "$UD/Art&A.um2.cfg"
echo "orient=ccw" > "$UD/NoArt.um2.cfg"
printf 'orient=cw\noverlay=Special.art\n' > "$UD/Ord.um2.cfg"
printf 'orient=ccw\noverlay=%s\n' "$W/elsewhere/Out.art" > "$UD/Abs.um2.cfg"
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 'Art&A.um2'
check "art beside the game: the MGL loads it, path relative to games/Vectrex-PiTrex, & escaped" 'waitfor "[ \$(runs) -gt $n ]" 12 && grep -qF "<file delay=\"1\" type=\"f\" index=\"2\" path=\"UVMC2/Art&amp;A.art\"/>" "$T/pitrex_core.mgl" && grep -q "<rbf>_Console/Vectrex-PiTrex_20261029</rbf>" "$T/pitrex_core.mgl" && [ "$(tail -1 "$T/pitrex_core.mgl")" = "</mistergamedescription>" ]'
check "the log says which art" 'grep -q "reloading the core with overlay Art&A.art" "$T/pitrexd.log"'
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 NoArt.um2
check "no art: the MGL is as before (no file line), the core is still reloaded" 'waitfor "[ \$(runs) -gt $n ]" 12 && grep -q load_core "$W/cmds.log" && ! grep -q "<file" "$T/pitrex_core.mgl" && grep -q "<setname>Vectrex-PiTrex</setname>" "$T/pitrex_core.mgl"'
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Ord.um2
check "overlay= in the sidecar beats the art beside the game (bare name: the Overlays folder)" 'waitfor "[ \$(runs) -gt $n ]" 12 && grep -qF "path=\"Overlays/Special.art\"" "$T/pitrex_core.mgl" && ! grep -q "Ord.art" "$T/pitrex_core.mgl"'
sleep 11; n=$(runs); : > "$W/cmds.log"
pick games/Vectrex-PiTrex/UVMC2 Abs.um2
check "art outside games/Vectrex-PiTrex: absolute path" 'waitfor "[ \$(runs) -gt $n ]" 12 && grep -qF "path=\"$W/elsewhere/Out.art\"" "$T/pitrex_core.mgl"'

echo "== 15. art_lib.sh: lookup order and the copy step"
LF=$W/lib; LG=$LF/games/Vectrex-PiTrex/UVMC2; LO=$LF/games/Vectrex/Overlays; LN=$LF/games/Vectrex-PiTrex/Overlays
mkdir -p "$LG/art" "$LO/Tate" "$LN/Tate"
libt() { ( FAT=$LF; . "$LIB"; "$@" ); }
fa() { libt find_art "$LG/$1.um2" "$LG/$1.um2.cfg"; }
pl() { libt art_place "$LG/$1.um2"; }
touch "$LG/Ord.um2"
check "find_art: no art anywhere finds nothing" '[ -z "$(fa Ord)" ]'
echo c > "$LN/Ord.art"
check "find_art: the Overlays folder, last" '[ "$(fa Ord)" = "$LN/Ord.art" ]'
echo b > "$LG/ORD.ART"
check "find_art: beside the game beats the Overlays folder (extension in any case)" '[ "$(fa Ord)" = "$LG/ORD.ART" ]'
echo a > "$LN/Named.art"; echo "overlay=Named.art" > "$LG/Ord.um2.cfg"
check "find_art: overlay= beats both (bare name: Overlays folder)" '[ "$(fa Ord)" = "$LN/Named.art" ]'
echo a > "$LG/art/Mine.art"; printf 'overlay=art/Mine.art\r\n' > "$LG/Ord.um2.cfg"
check "find_art: overlay= with a path is relative to the game's folder (CRLF sidecar)" '[ "$(fa Ord)" = "$LG/art/Mine.art" ]'
echo "overlay=Gone.art" > "$LG/Ord.um2.cfg"
check "find_art: overlay= naming a missing file falls back to the art beside the game" '[ "$(fa Ord)" = "$LG/ORD.ART" ]'
echo a > "$LO/Old.art"; touch "$LG/Old.um2"; echo "overlay=OLD.ART" > "$LG/Old.um2.cfg"
check "find_art: a bare overlay= name is also found in games/Vectrex/Overlays, in any case" '[ "$(fa Old)" = "$LO/Old.art" ]'
touch "$LG/Tt.um2"; echo p > "$LN/Tt.art"; echo cw > "$LN/Tate/Tt_90CW.art"; echo ccw > "$LN/Tate/Tt_90CCW.art"
check "find_art: no orient: the upright art" '[ "$(fa Tt)" = "$LN/Tt.art" ]'
echo "orient=cw" > "$LG/Tt.um2.cfg"
check "find_art: orient=cw takes the Tate version from the Overlays folder" '[ "$(fa Tt)" = "$LN/Tate/Tt_90CW.art" ]'
echo "orient=ccw" > "$LG/Tt.um2.cfg"
check "find_art: orient=ccw the other one" '[ "$(fa Tt)" = "$LN/Tate/Tt_90CCW.art" ]'

touch "$LG/NoArt.um2"
check "art_place: a game with no art: nothing printed, nothing copied, no error" 'r=$(pl NoArt); [ -z "$r" ] && [ ! -e "$LG/NoArt.art" ]'
touch "$LG/Copy.um2"; echo copyme > "$LO/Copy.art"
check "art_place: copies a same-name file next to the game" 'r=$(pl Copy); [ "$r" = "copied Copy.art" ] && [ "$(cat "$LG/Copy.art")" = copyme ]'
check "art_place: again: our own copy is there, nothing printed" 'r=$(pl Copy); [ -z "$r" ]'
touch "$LG/Ci.um2"; echo ci > "$LN/CI.ART"
check "art_place: the source name matches in any case" 'r=$(pl Ci); [ "$r" = "copied CI.ART" ] && [ "$(cat "$LG/Ci.art")" = ci ]'
touch "$LG/Kept.um2"; echo ours > "$LO/Kept.art"; echo mine > "$LG/Kept.art"
check "art_place: a .art of yours is kept, as it is" 'r=$(pl Kept); [ "$r" = "kept yours" ] && [ "$(cat "$LG/Kept.art")" = mine ]'
touch "$LG/Ov.um2"; echo "overlay=Special.art" > "$LG/Ov.um2.cfg"; echo special > "$LN/Special.art"
check "art_place: overlay= names the source, the copy is named like the game" 'r=$(pl Ov); [ "$r" = "copied Special.art" ] && [ "$(cat "$LG/Ov.art")" = special ]'
touch "$LG/Opn.um2"; echo opn > "$LO/Opn.art"; echo opn > "$LN/Opn.art"; printf 'view=Open\noverlay=Special.art\n' > "$LG/Opn.um2.cfg"
check "view=open: no art (loaded art would select View Area Standard)" 'r=$(pl Opn); [ -z "$r" ] && [ ! -e "$LG/Opn.art" ] && [ -z "$(fa Opn)" ]'
touch "$LG/Ov2.um2"; echo "overlay=Gone.art" > "$LG/Ov2.um2.cfg"; echo ours > "$LO/Ov2.art"
check "art_place: overlay= naming a missing file is said, nothing copied" 'r=$(pl Ov2); [ "$r" = "overlay=Gone.art not found" ] && [ ! -e "$LG/Ov2.art" ]'
touch "$LG/Rot.um2" "$LG/Rot2.um2" "$LG/Rot3.um2" "$LG/Rot4.um2"
for g in Rot Rot3 Rot4; do echo plain > "$LO/$g.art"; done
echo cw > "$LO/Tate/Rot_90CW.art"; echo ccw > "$LO/Tate/Rot_90CCW.art"; echo ccw2 > "$LO/Tate/Rot2_90CCW.art"
echo "orient=cw" > "$LG/Rot.um2.cfg"; echo "orient=ccw" > "$LG/Rot2.um2.cfg"; echo "orient=CW" > "$LG/Rot3.um2.cfg"; echo "orient=0" > "$LG/Rot4.um2.cfg"
check "art_place: orient=cw copies the 90CW version" 'r=$(pl Rot); [ "$r" = "copied Rot_90CW.art" ] && [ "$(cat "$LG/Rot.art")" = cw ]'
check "art_place: orient=ccw copies the 90CCW version (a game with only that one)" 'r=$(pl Rot2); [ "$r" = "copied Rot2_90CCW.art" ] && [ "$(cat "$LG/Rot2.art")" = ccw2 ]'
check "art_place: rotated, upright art only: nothing is copied" 'r=$(pl Rot3); [ -z "$r" ] && [ ! -e "$LG/Rot3.art" ]'
check "art_place: orient=0 is not rotated: the upright art" 'r=$(pl Rot4); [ "$r" = "copied Rot4.art" ] && [ "$(cat "$LG/Rot4.art")" = plain ]'
check "art_place: no half-copied files left behind" '[ -z "$(ls -A "$LG" | grep "\.part$")" ]'

echo "== 16. make_card.sh does not put .art files on the card, nor count them as a change"
CK=$W/ck; mkdir -p "$CK/games/Vectrex-PiTrex/UVMC2" "$CK/pitrex/uvmc2"
touch "$CK/games/Vectrex-PiTrex/UVMC2/G.um2" "$CK/games/Vectrex-PiTrex/UVMC2/G.art"
check "only a game and its art: nothing to build a card from" 'PITREX_FAT=$CK bash "$MC" --check; [ $? = 0 ]'
touch "$CK/games/Vectrex-PiTrex/UVMC2/ROMS.zip"
check "with a card file: a build would be needed" 'PITREX_FAT=$CK bash "$MC" --check; [ $? = 1 ]'

echo "== 17. ASSETS folders: menu pictures and videos stay off the card, a game's sound there goes on it (Vector Tank)"
CK2=$W/ck2; A=$CK2/games/Vectrex-PiTrex/UVMC2/PORTDATA/vt/ASSETS; mkdir -p "$A" "$CK2/pitrex/uvmc2"
touch "$A/vt_preview.mp4" "$A/Menu.PNG"
check "only previews in ASSETS: nothing to build a card from" 'PITREX_FAT=$CK2 bash "$MC" --check; [ $? = 0 ]'
touch "$A/vt.pcm"
check "a sound file in ASSETS: a build would be needed" 'PITREX_FAT=$CK2 bash "$MC" --check; [ $? = 1 ]'

echo
if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails FAILED"; echo "--- pitrexd.log"; cat "$T/pitrexd.log"; echo "--- pitrexd.out"; cat "$W/pitrexd.out"; fi
exit $(( fails > 0 ))

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run on Linux: sh pitrex/tests/card_lock_test.sh
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/card-lock.XXXXXX")
holder=""; builder=""
cleanup() {
	[ -z "$holder" ] || kill "$holder" 2>/dev/null || :
	[ -z "$builder" ] || kill "$builder" 2>/dev/null || :
	rm -rf "$W"
}
trap cleanup EXIT
mkdir -p "$W/bin" "$W/tmp" "$W/fat/pitrex/uvmc2" "$W/fat/games/Vectrex-PiTrex/UVMC2"
FAT=$W/fat; UV=$FAT/pitrex/uvmc2; GAMES=$FAT/games/Vectrex-PiTrex/UVMC2
export PITREX_FAT=$FAT PITREX_TMP=$W/tmp PATH="$W/bin:$PATH"
CARD_SHELL=${CARD_SHELL:-sh}

cat > "$W/lock.c" <<'EOF'
#include "um2card_lock.h"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    return um2card_lock(argv[1]);
}
EOF
${CC:-cc} -std=c99 -Wall -Wextra -I"$P/um2run" "$W/lock.c" -o "$W/lock"
cat > "$W/bin/id" <<'EOF'
#!/bin/sh
echo 0
EOF
cat > "$W/bin/mkfs.vfat" <<'EOF'
#!/bin/sh
if [ -n "${TEST_GATE:-}" ]; then
	touch "$TEST_GATE/ready"
	while [ ! -f "$TEST_GATE/release" ]; do sleep 0.1; done
	exit 1
fi
exit 0
EOF
chmod +x "$W/bin/id" "$W/bin/mkfs.vfat"

wait_for() {
	i=0
	while [ ! -f "$1" ]; do
		i=$((i + 1))
		[ "$i" -lt 100 ] || { echo "timed out waiting for $1"; exit 1; }
		sleep 0.1
	done
}
hold() {
	rm -f "$W/held" "$W/release"
	( flock -n 9; touch "$W/held"; while [ ! -f "$W/release" ]; do sleep 0.1; done ) 9>>"$1.lock" &
	holder=$!
	wait_for "$W/held"
}
release() { touch "$W/release"; wait "$holder"; holder=""; }
refused() {
	if $CARD_SHELL "$P/scripts/make_card.sh" "$@" > "$W/out" 2>&1; then
		echo "make_card unexpectedly succeeded"; exit 1
	fi
	grep -F 'a UVMC2 game is running and may be writing to its card: stop it first (Scripts > PiTrex_Stop)' "$W/out" > /dev/null
}

# Blank-card creation also takes the lock, and reports contention.
hold "$UV/sd.img"
refused --if-needed
set +e
"$W/lock" "$UV/sd.img" 2> "$W/err"
rc=$?
set -e
[ "$rc" = 6 ]
grep -Fx "um2run: card $UV/sd.img is in use (being rebuilt?)" "$W/err" > /dev/null
release
$CARD_SHELL "$P/scripts/make_card.sh" --if-needed > "$W/out"
[ -f "$UV/sd.img" ]
[ -f "$UV/sd.img.lock" ]
"$W/lock" "$UV/sd.img"
echo "PASS: blank card and runner contention"

# Legacy BattleZone and scale destinations are locked before any rebuild begins.
mkdir -p "$UV/card"
echo data > "$UV/card/data"
touch "$GAMES/battlezone.um2" "$GAMES/game.um2"
echo scale=118 > "$GAMES/game.um2.cfg"
for card in "$UV/sd.img" "$UV/bz_sd.img" "$UV/sd_scale118.img"; do
	hold "$card"
	refused
	release
done
echo "PASS: main, BattleZone and scale card contention"

# Scale-only builds must also lock the source and destination.
rm -f "$GAMES/battlezone.um2" "$UV/sd.img.list"
touch "$UV/sd.img"
for card in "$UV/sd.img" "$UV/sd_scale118.img"; do
	hold "$card"
	refused --if-needed
	release
done
echo "PASS: scale-only source and destination contention"
touch "$GAMES/battlezone.um2"

# Stop at formatting and check all locks remain held during the build.
rm -f "$UV/sd.img"
rm -f "$W/ready" "$W/release"
export TEST_GATE=$W
$CARD_SHELL "$P/scripts/make_card.sh" > "$W/out" 2>&1 &
builder=$!
wait_for "$W/ready"
for card in "$UV/sd.img" "$UV/bz_sd.img" "$UV/sd_scale118.img"; do
	set +e
	"$W/lock" "$card" 2> "$W/err"
	rc=$?
	set -e
	[ "$rc" = 6 ]
done
touch "$W/release"
if wait "$builder"; then echo "mock formatter unexpectedly succeeded"; exit 1; fi
builder=""
unset TEST_GATE
for card in "$UV/sd.img" "$UV/bz_sd.img" "$UV/sd_scale118.img"; do "$W/lock" "$card"; done
echo "PASS: locks held during rebuild and released on exit"

# Lock files neither select the legacy folder nor enter the card manifest.
rm -f "$UV/card/data"
touch "$UV/card/unused.img.lock" "$GAMES/other.img.lock"
echo data > "$GAMES/data"
rm -f "$W/ready" "$W/release"
export TEST_GATE=$W
$CARD_SHELL "$P/scripts/make_card.sh" > "$W/out" 2>&1 &
builder=$!
wait_for "$W/ready"
grep -F "card folder: 1 files" "$W/out" > /dev/null
touch "$W/release"
if wait "$builder"; then echo "mock formatter unexpectedly succeeded"; exit 1; fi
builder=""
echo "PASS: lock files ignored in card input"

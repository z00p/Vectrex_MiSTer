#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Card builder and the .conf/ folder of UVMC2 SDK 1.4.17 and later (read before config/, the only one saved to).
# Runs the whole make_card.sh without root: id, mkfs.vfat, losetup, mount and umount are stand-ins, and a card
# image is a tar file of what was on it. It does not test FAT itself: a build on a MiSTer does.
# Run on Linux: bash pitrex/tests/card_conf_test.sh
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/card-conf.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/dev" "$W/mounts"
export TEST_W=$W PATH="$W/bin:$PATH"

cat > "$W/bin/id" <<'EOF'
#!/bin/sh
echo 0
EOF
cat > "$W/bin/mkfs.vfat" <<'EOF'
#!/bin/sh
for a in "$@"; do img=$a; done
: > "$img"
EOF
cat > "$W/bin/fsck.vfat" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$W/bin/losetup" <<'EOF'
#!/bin/sh
case "$1" in
-f) mktemp "$TEST_W/dev/loop.XXXXXX"; exit ;;
-d) exit 0 ;;
-r) shift ;;
esac
echo "$2" > "$1"
EOF
cat > "$W/bin/mount" <<'EOF'
#!/bin/sh
ro=0
shift 2
if [ "$1" = -o ]; then ro=1; shift 2; fi
img=$(cat "$1"); mnt=$2
case "$mnt" in "$TEST_W"/*) ;; *) echo "test mount outside the scratch tree: $mnt"; exit 1 ;; esac
key=$(echo "$mnt" | tr / _)
echo "$ro $img" > "$TEST_W/mounts/$key"
[ ! -s "$img" ] || tar -xf "$img" -C "$mnt"
EOF
cat > "$W/bin/umount" <<'EOF'
#!/bin/sh
for a in "$@"; do mnt=$a; done
case "$mnt" in "$TEST_W"/*) ;; *) echo "test unmount outside the scratch tree: $mnt"; exit 1 ;; esac
key=$(echo "$mnt" | tr / _)
read -r ro img < "$TEST_W/mounts/$key" || exit 1
[ "$ro" = 1 ] || tar -cf "$img" -C "$mnt" . || exit 1
find "$mnt" -mindepth 1 -delete
rm -f "$TEST_W/mounts/$key"
EOF
chmod +x "$W"/bin/*

pass=0; failed=0; n=0
ok() {	# <what> <command...>
	local what=$1; shift
	if "$@"; then echo "PASS: $what"; pass=$((pass + 1)); else echo "FAIL: $what"; failed=$((failed + 1)); fi
}
fresh() {	# an empty MiSTer tree
	n=$((n + 1))
	FAT=$W/t$n/fat; UV=$FAT/pitrex/uvmc2; GAMES=$FAT/games/Vectrex-PiTrex/UVMC2
	mkdir -p "$UV" "$GAMES" "$W/t$n/tmp"
	export PITREX_FAT=$FAT PITREX_TMP=$W/t$n/tmp
	echo data > "$GAMES/data"
}
build() {
	bash "$P/scripts/make_card.sh" "$@" > "$W/out" 2>&1 || { cat "$W/out"; echo "FAIL: make_card did not finish"; exit 1; }
}
edit() {	# <image> <path> <text>: a game writes a file on its card
	local d
	d=$(mktemp -d "$W/edit.XXXXXX")
	tar -xf "$1" -C "$d"
	mkdir -p "$(dirname "$d/$2")"
	printf '%b' "$3" > "$d/$2"
	tar -cf "$1" -C "$d" .
	rm -rf "$d"
}
has() { tar -xOf "$1" "./$2" 2>/dev/null | tr -d '\r' | sed 's/[[:space:]]*$//' | grep -Fx "$3" > /dev/null; }	# <image> <path> <line>
lacks() { ! has "$@"; }
absent() { ! tar -tf "$1" | grep -i "^\./$2" > /dev/null; }
entries() { [ "$(tar -tf "$1" | grep -i -c "^\./$2\$")" = "$3" ]; }	# <image> <path> <how many, ignoring case>
bytes() { [ "$(tar -xOf "$1" "./$2" | wc -c)" = "$3" ]; }

echo "== saves over a rebuild"
for dir in config .conf .CONF PORTDATA/game/.conf; do
	fresh
	mkdir -p "$GAMES/$dir"
	printf 'from the folder\n' > "$GAMES/$dir/uvm2.cfg"
	build
	edit "$UV/sd.img" "$dir/uvm2.cfg" 'changed by the player\n'
	build
	ok "$dir/ from the card folder keeps the player's change" has "$UV/sd.img" "$dir/uvm2.cfg" "changed by the player"
done
fresh
build
edit "$UV/sd.img" .conf/GAME.CFG 'made by the game\n'
build
ok ".conf/ made by a game is kept" has "$UV/sd.img" .conf/GAME.CFG "made by the game"
fresh
mkdir -p "$GAMES/.roms"
echo old > "$GAMES/.roms/set.zip"
build
echo new > "$GAMES/.roms/set.zip"
build
ok ".roms/ is not a save folder: the card folder's new file wins" has "$UV/sd.img" .roms/set.zip new

echo "== scale cards"
scaled() {
	fresh
	touch "$GAMES/game.um2"
	echo scale=118 > "$GAMES/game.um2.cfg"
	build
	SC=$UV/sd_scale118.img
}
scaled
ok "without .conf/, config/ gets the scale" has "$SC" config/uvm2.cfg "scale 118"
ok "without .conf/, none is made" absent "$SC" .conf

scaled
edit "$UV/sd.img" .conf/uvm2.cfg 'scale 160\ndrift_x 7\n'
build
ok ".conf/ from sd.img gets the scale" has "$SC" .conf/uvm2.cfg "scale 118"
ok ".conf/ loses the old scale" lacks "$SC" .conf/uvm2.cfg "scale 160"
ok ".conf/ keeps its other fields" has "$SC" .conf/uvm2.cfg "drift_x 7"
ok ".conf/ is padded to a sector" bytes "$SC" .conf/uvm2.cfg 512
ok "config/ still gets the scale, for older images" has "$SC" config/uvm2.cfg "scale 118"
ok "sd.img's own .conf/ is left alone" has "$UV/sd.img" .conf/uvm2.cfg "scale 160"
ok "the two folders' fields are not mixed" lacks "$SC" config/uvm2.cfg "drift_x 7"

scaled
edit "$SC" .conf/uvm2.cfg 'scale 140\ndrift_y 3\n'
build
ok "the scale card's own .conf/ gets the scale" has "$SC" .conf/uvm2.cfg "scale 118"
ok "the scale card's own calibration is kept" has "$SC" .conf/uvm2.cfg "drift_y 3"

scaled
edit "$UV/sd.img" .CONF/UVM2.CFG 'scale 160\n'
build
ok ".CONF/UVM2.CFG gets the scale" has "$SC" .CONF/UVM2.CFG "scale 118"
ok ".CONF/UVM2.CFG is not doubled" entries "$SC" .conf/uvm2.cfg 1

scaled
edit "$UV/sd.img" .conf/GAME.CFG 'hz 60\n'
build
ok ".conf/ without uvm2.cfg gets none (the game falls back to config/)" absent "$SC" .conf/uvm2.cfg

scaled
edit "$UV/sd.img" .conf/uvm2.cfg 'scale 160\n'
rm -f "$SC" "$SC.list"
build --if-needed
ok "a scale-only build sets .conf/ too" has "$SC" .conf/uvm2.cfg "scale 118"

echo "== $pass passed, $failed failed"
[ "$failed" = 0 ]

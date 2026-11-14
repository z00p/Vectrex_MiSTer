#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# make_card.sh: build the UVMC2 card image (runs on the MiSTer, as root).
#   /media/fat/games/Vectrex-PiTrex/UVMC2/  the files from your real UVMC2 SD card and your .um2 games, all in one folder (input)
#   /media/fat/pitrex/uvmc2/card/   the same input, older installs: used instead, games copied out, while it has files of its own
#   /media/fat/pitrex/uvmc2/sd.img  the card every UVMC2 game without an sd= key in its sidecar sees (output)
# The card is a FAT32 image of a whole card without a partition table ("superfloppy"), the layout um2run
# was tested with. Rebuilding keeps the player's saves: what the games wrote to the old sd.img (every
# file the last build did not put there, and all config and scores folders) is copied into the new one,
# and the old image is kept as sd.img.bak (one backup). sd.img.list records what a build put on the card.
# BattleZone writes to its card: if its image is in games/Vectrex-PiTrex/UVMC2, bz_sd.img is made too
# (same files, its own saves) and, if the image has no sidecar yet, one is written with sd=bz_sd.img.
# A sidecar that names another card is left alone and bz_sd.img is not made.
# Older installs: .um2 games in the card folder are not put on the card: they are copied (subfolders kept) to
# games/Vectrex-PiTrex/UVMC2. Nothing there is overwritten: a different file of the same name is left alone and reported.
# BattleZone's own card is older installs only: in the one-folder layout it shares sd.img, as it shares a real card.
# Scale cards: a game that wants the console calibration's `scale` changed (its sidecar says scale=N, or a built-in one by name, see
# game_scale) gets sd_scale<N>.img: a copy of sd.img with config/uvm2.cfg saying `scale N` (any other fields in that file kept), and
# its own saves. It has to be a card of its own: nearly every UVMC2 image reads config/uvm2.cfg, so on sd.img it would resize them all.
# pitrexd.sh starts such a game on it. A sidecar with sd= (a card named by hand) or scale=0 gets none.
#   make_card.sh               build now
#   make_card.sh --if-needed   build only if sd.img is missing, the card folder changed since the last build, or
#                              BattleZone has no card of its own yet (PiTrex_Setup uses this); quiet when there is
#                              nothing to do. A scale card that is missing or older than sd.img's build is made
#                              on its own, without rebuilding sd.img. The .um2 games are copied either way.
#   make_card.sh --check       only say: exit status 1 if a build would change the card (or a scale card), 0 if not. Changes nothing.
# Overlay art: for each .um2 game the art is copied next to it as <game>.art, where the core's OSD pick finds it (see art_lib.sh), unless a
# .art is there already (yours is never replaced). .art files are not put on the card.
# Needs: bash, mkfs.vfat, losetup, mount, python3 (3.9). No mtools. Undo: delete sd.img (and bz_sd.img, sd_scale*.img).
# PITREX_FAT and PITREX_TMP are test hooks (the PC test points them at a scratch tree); on the MiSTer leave them unset.
FAT=${PITREX_FAT:-/media/fat}
T=${PITREX_TMP:-/tmp}
UV=$FAT/pitrex/uvmc2
OUT=$UV/sd.img
GAMES=$FAT/games/Vectrex-PiTrex/UVMC2
# one folder for everything, unless an older install has files in the card folder
CARD=$GAMES; ONE=1
if [ -n "$(find "$UV/card" -mindepth 1 -maxdepth 1 ! -name README.txt ! -iname '*.lock' 2>/dev/null | head -1)" ]; then CARD=$UV/card; ONE=0; fi
UM2=$FAT/pitrex/um2run
MAX_MIB=16000
BIG_MIB=2000    # above this, 4 KiB clusters: the FAT of a big card with 512-byte clusters would be hundreds of MiB
BLANK_MIB=64
LABEL=UVM2SD
case "$0" in */*) . "${0%/*}/art_lib.sh" ;; *) . ./art_lib.sh ;; esac

say() { echo "make_card: $*"; }
die() { echo "make_card: ERROR: $*"; exit 1; }

IF_NEEDED=0; CHECK=0
case "$1" in
	--if-needed) IF_NEEDED=1 ;;
	--check) CHECK=1 ;;
	"") ;;
	*) echo "usage: $0 [--if-needed | --check]"; exit 2 ;;
esac

# ---------------------------------------------------------------------------------------------
# Python helper (anything fiddly: walking the folder with its skip rules, sizes, copies).
# Runs on the MiSTer's Python 3.9: no newer syntax.
PYHELP=$(cat <<'PYEOF'
import os, sys, shutil

# Not copied to the card: cart images and firmware, menu previews (the pictures and videos in ASSETS folders; other files
# there, such as a game's sound, do go on the card), macOS and Windows clutter,
# and the README.txt PiTrex_Setup leaves in the card folder (it carries the marker line below).
JUNK = {"__macosx", ".ds_store", ".spotlight-v100", ".trashes", ".fseventsd", ".temporaryitems",
        "thumbs.db", "desktop.ini", "system volume information", "$recycle.bin"}
MARK = "(This file is not copied to the card.)"
PREVIEW = (".mp4", ".mov", ".avi", ".mkv", ".webm", ".png", ".jpg", ".jpeg", ".gif", ".bmp")

def why_skip(path, name, isdir, top):
    n = name.lower()
    if n in JUNK or n.startswith("._"):
        return "macOS/Windows files"
    if isdir:
        return None
    if n.endswith(PREVIEW) and os.path.basename(os.path.dirname(path)).lower() == "assets":
        return "menu pictures and videos (ASSETS)"
    if n.endswith((".um2", ".uf2")):
        return ".um2/.uf2 files"
    if n.endswith((".um2.cfg", ".um2.load", ".img", ".img.list", ".img.bak", ".lock")):
        return "game settings and card images"
    if n.endswith(".art"):
        return "overlay art"
    if top and n == "readme.txt":
        try:
            with open(path, "rb") as f:
                if MARK.encode() in f.read(4096):
                    return "README.txt (this folder's notes)"
        except OSError:
            pass
    return None

def walk(src):
    """(relative path, full path, is_dir) for everything that goes on the card, plus a dict of skips."""
    out, skips = [], {}
    for root, dirs, files in os.walk(src):
        top = os.path.samefile(root, src)
        keep = []
        for d in sorted(dirs):
            p = os.path.join(root, d)
            w = why_skip(p, d, True, top)
            if w:
                skips[w] = skips.get(w, 0) + 1
            else:
                keep.append(d)
                out.append((os.path.relpath(p, src), p, True))
        dirs[:] = keep
        for f in sorted(files):
            p = os.path.join(root, f)
            w = why_skip(p, f, False, top)
            if w:
                skips[w] = skips.get(w, 0) + 1
            elif os.path.isfile(p):
                out.append((os.path.relpath(p, src), p, False))
    return out, skips

def fat_bytes(entries):
    """Rough space on a FAT32 card with 512-byte clusters: files rounded up to whole clusters,
    one cluster per folder, 32-byte directory entries plus long-name entries."""
    total = 512  # root directory
    for rel, p, isdir in entries:
        name = os.path.basename(rel)
        total += 32 * (2 + len(name) // 13)
        if isdir:
            total += 512
        else:
            total += (os.path.getsize(p) + 511) // 512 * 512
    return total

def card_mib(fat, max_mib):
    mib = 1 << 20
    need = fat * 5 // 4 + 16 * mib    # contents x 1.25 + 16 MiB for new settings and scores
    card = max(64, (need + mib - 1) // mib)
    # near the limit: the 1.25 headroom may go, the 16 MiB for saves stays
    if card > int(max_mib) and fat + 16 * mib <= int(max_mib) * mib:
        card = int(max_mib)
    return card

def all_files(root):
    out = []
    for r, dirs, files in os.walk(root):
        out += [(os.path.relpath(os.path.join(r, d), root), os.path.join(r, d), True) for d in dirs]
        out += [(os.path.relpath(os.path.join(r, f), root), os.path.join(r, f), False) for f in files]
    return out

def cmd_scan(src, max_mib):
    # prints: files data_size card_mib skips
    entries, skips = walk(src)
    files = [e for e in entries if not e[2]]
    data = sum(os.path.getsize(p) for r, p, d in files)
    mib = 1 << 20
    size = "%d KiB" % ((data + 1023) // 1024) if data < mib else "%d MiB" % ((data + mib // 2) // mib)
    s = ", ".join("%d %s" % (n, w) for w, n in sorted(skips.items())) or "-"
    print(len(files), size.replace(" ", "_"), card_mib(fat_bytes(entries), max_mib), s)

def cmd_size(src, max_mib, *saved):
    # card size for the folder plus the saves carried over from the old cards (the largest set counts)
    base = fat_bytes(walk(src)[0])
    extra = max([fat_bytes(all_files(d)) for d in saved] or [0])
    print(card_mib(base + extra, max_mib))

def cmd_list(src):
    # what the card is built from (compared by --if-needed): size, mtime, path
    entries, skips = walk(src)
    for rel, p, isdir in entries:
        if not isdir:
            st = os.stat(p)
            print("%d %d %s" % (st.st_size, int(st.st_mtime), rel.replace(os.sep, "/")))

def copy_file(s, d):
    # copyfile + mtime only: a FAT file system refuses chmod, which shutil.copy2 would try
    shutil.copyfile(s, d)
    st = os.stat(s)
    os.utime(d, (st.st_atime, st.st_mtime))

def find_ci(parent, name):
    """existing entry in parent whose name matches name ignoring case, else name itself"""
    try:
        for e in os.listdir(parent):
            if e.lower() == name.lower():
                return e
    except OSError:
        pass
    return name

def merge(src, dst):
    """copy the tree src into dst (existing folders reused, matched ignoring case; files replaced)"""
    n = 0
    for e in sorted(os.listdir(src)):
        s = os.path.join(src, e)
        d = os.path.join(dst, find_ci(dst, e))
        if os.path.isdir(s):
            if not os.path.isdir(d):
                os.mkdir(d)
            n += merge(s, d)
        elif os.path.isfile(s):
            copy_file(s, d)
            n += 1
    return n

def cmd_copy(src, dst):
    entries, skips = walk(src)
    total = sum(os.path.getsize(p) for rel, p, isdir in entries if not isdir) or 1
    n = done = shown = 0
    for rel, p, isdir in entries:
        d = os.path.join(dst, rel)
        try:
            if isdir:
                if not os.path.isdir(d):
                    os.mkdir(d)
            else:
                copy_file(p, d)
                n += 1
                # a big card takes many minutes: say how far it has got, every 5%
                done += os.path.getsize(p)
                pct = done * 20 // total * 5
                if pct != shown:
                    shown = pct
                    print("make_card: copying %d pct" % pct, file=sys.stderr, flush=True)
        except OSError as e:
            sys.exit("make_card: ERROR: cannot copy %s to the card: %s" % (rel, e.strerror or e))
    print(n)

def in_save_dir(rel):
    return any(p.lower() in ("config", "scores") for p in rel.split("/")[:-1])

def cmd_saves_out(mnt, dst, listfile, card):
    """Copy the player's files out of an old card into dst (same paths); prints the number of files.
    The games do not all save in the same place (config/ and scores/ at the top, Punch-Out in
    PORTDATA/Punchout/CONFIG, BattleZone makes a BATTLEZONE folder), so kept are:
      - every file in a folder named config or scores (any case, any depth), and
      - if this script built the card (its .list exists): every file and folder it did not put there,
        i.e. what the games wrote;
      - otherwise (a card made by hand): every file and folder the card folder does not provide,
        so nothing of the player's is lost when this script first replaces it.
    Files that are kept win over the card folder's files of the same name."""
    built = set()
    if os.path.isfile(listfile):
        with open(listfile, encoding="utf-8", errors="surrogateescape") as f:
            for line in f:
                parts = line.rstrip("\n").split(" ", 2)
                if len(parts) == 3:
                    built.add(parts[2].lower())
    else:
        built = set(r.replace(os.sep, "/").lower() for r, p, d in walk(card)[0] if not d)
    built_dirs = set()
    for b in built:
        parts = b.split("/")
        for i in range(1, len(parts)):
            built_dirs.add("/".join(parts[:i]))
    n = 0
    for r, dirs, files in os.walk(mnt):
        dirs.sort()
        for d in dirs:
            rel = os.path.relpath(os.path.join(r, d), mnt).replace(os.sep, "/")
            if rel.lower() not in built_dirs:
                os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in sorted(files):
            rel = os.path.relpath(os.path.join(r, f), mnt).replace(os.sep, "/")
            if in_save_dir(rel) or rel.lower() not in built:
                d = os.path.join(dst, rel)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                copy_file(os.path.join(r, f), d)
                n += 1
    print(n)

def cmd_saves_in(src, mnt):
    try:
        print(merge(src, mnt))
    except OSError as e:
        sys.exit("make_card: ERROR: cannot put the saves back: %s" % e)

def cmd_mkimg(path, mib):
    with open(path, "wb") as f:
        f.truncate(int(mib) << 20)

def cmd_free(path):
    st = os.statvfs(path)
    print(st.f_bavail * st.f_frsize >> 20)   # MiB

def cmd_dev(path):
    print(os.stat(path).st_dev)

def cmd_cfg(path, key):
    # same rules as pitrexd's cfg_get: last key=value wins, # comments and a trailing CR ignored
    val = ""
    with open(path, "rb") as f:
        for line in f.read().decode("utf-8", "replace").splitlines():
            line = line.strip()
            if line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.strip() == key:
                val = v.strip()
    print(val)

def same_file(a, b):
    if not os.path.isfile(b) or os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x = fa.read(1 << 20)
            if x != fb.read(1 << 20):
                return False
            if not x:
                return True

def cmd_um2(src, dst):
    """Copy the .um2 files of the card folder (any case, any depth, same skip rules for folders) to the
    games folder, keeping their subfolders. A copy, never a move, and nothing is ever overwritten.
    Prints one line per file: C (copied), S (already there, same), D (a different file is there), E (error)."""
    for root, dirs, files in os.walk(src):
        top = os.path.samefile(root, src)
        dirs[:] = sorted(d for d in dirs if not why_skip(os.path.join(root, d), d, True, top))
        for f in sorted(files):
            if not f.lower().endswith(".um2") or f.startswith("._"):
                continue
            p = os.path.join(root, f)
            if not os.path.isfile(p):
                continue
            rel = os.path.relpath(p, src).replace(os.sep, "/")
            d = os.path.join(dst, rel)
            try:
                if os.path.lexists(d):
                    print("%s %s" % ("S" if same_file(p, d) else "D", rel))
                    continue
                if not os.path.isdir(os.path.dirname(d)):
                    os.makedirs(os.path.dirname(d))
                # under a temporary name first, so an interrupted copy never looks like a game
                tmp = os.path.join(os.path.dirname(d), ".%s.%d.part" % (f, os.getpid()))
                try:
                    copy_file(p, tmp)
                    os.rename(tmp, d)
                finally:
                    if os.path.lexists(tmp):
                        os.remove(tmp)
                print("C %s" % rel)
            except OSError as e:
                print("E %s: %s" % (rel, e.strerror or e))
    sys.stdout.flush()

def cmd_scalecfg(mnt, n):
    """config/uvm2.cfg on a mounted card says `scale n`: the other lines of an existing file are kept (a calibration made
    with a game's wizard, say), any scale line is replaced. Padded to 512 bytes: the SDK's writer only overwrites a file of
    one sector or more, so the wizard can still save to it. Folder and file matched ignoring case, as the games' FAT does."""
    d = os.path.join(mnt, find_ci(mnt, "config"))
    if not os.path.isdir(d):
        os.mkdir(d)
    p = os.path.join(d, find_ci(d, "uvm2.cfg"))
    keep = []
    if os.path.isfile(p):
        with open(p, "rb") as f:
            for line in f.read().decode("latin-1").split("\n"):
                line = line.rstrip("\r")
                if line.strip() and line.split()[0] != "scale":
                    keep.append(line.rstrip())
    text = "\n".join(["scale %d" % int(n)] + keep) + "\n"
    if len(text) < 512:
        text += " " * (511 - len(text)) + "\n"
    with open(p, "wb") as f:
        f.write(text.encode("latin-1"))

cmds = {"scan": cmd_scan, "size": cmd_size, "list": cmd_list, "copy": cmd_copy, "saves_out": cmd_saves_out,
        "saves_in": cmd_saves_in, "mkimg": cmd_mkimg, "free": cmd_free, "dev": cmd_dev, "cfg": cmd_cfg,
        "um2": cmd_um2, "scalecfg": cmd_scalecfg}
cmds[sys.argv[1]](*sys.argv[2:])
PYEOF
)
py() { python3 -c "$PYHELP" "$@"; }

# ---------------------------------------------------------------------------------------------
# Cleanup: whatever happens, unmount, detach the loop devices and remove the half-built files.
MOUNTS=""; LOOPS=""; TMPFILES=""; WORK=""; LOCK=""; SAVES=""
cleanup() {
	local m l f
	for m in $MOUNTS; do umount "$m" 2>/dev/null || umount -l "$m" 2>/dev/null; done
	for l in $LOOPS; do losetup -d "$l" 2>/dev/null; done
	for f in $TMPFILES; do rm -f "$f"; done
	if [ -n "$WORK" ]; then
		for m in "$WORK"/mnt*; do [ -d "$m" ] && rmdir "$m" 2>/dev/null; done
		rm -f "$WORK"/*.log
		rmdir "$WORK" 2>/dev/null
	fi
	# the saves folder is ours (mktemp) and never a mount point
	case "$SAVES" in "$UV"/.make_card.*) rm -rf "$SAVES" ;; esac
	[ -n "$LOCK" ] && rm -rf "$LOCK"
}
trap cleanup EXIT
trap 'echo "make_card: interrupted"; exit 1' INT TERM HUP

# attach <image> <mount point> [ro]: loop device + mount, remembered for cleanup
attach() {
	local img=$1 mnt=$2 ro=$3 dev
	dev=$(losetup -f 2>/dev/null) || dev=""
	[ -n "$dev" ] || die "no free loop device"
	if [ -n "$ro" ]; then losetup -r "$dev" "$img" || die "losetup failed for $img"
	else losetup "$dev" "$img" || die "losetup failed for $img"; fi
	LOOPS="$LOOPS $dev"
	mkdir -p "$mnt"
	if [ -n "$ro" ]; then mount -t vfat -o ro "$dev" "$mnt" 2>"$WORK/mount.log"
	else mount -t vfat "$dev" "$mnt" 2>"$WORK/mount.log"; fi || { cat "$WORK/mount.log"; return 1; }
	MOUNTS="$mnt $MOUNTS"
}
# detach <mount point>: unmount and free its loop device
detach() {
	local mnt=$1 dev m l keep=""
	dev=$(awk -v m="$mnt" '$2 == m { print $1 }' /proc/mounts | tail -1)
	umount "$mnt" || die "cannot unmount $mnt"
	for m in $MOUNTS; do [ "$m" = "$mnt" ] || keep="$keep $m"; done; MOUNTS=$keep
	keep=""
	for l in $LOOPS; do
		if [ "$l" = "$dev" ]; then losetup -d "$l" 2>/dev/null; else keep="$keep $l"; fi
	done
	LOOPS=$keep
}

# resolve <name> <folder of the .um2>: the card pitrexd would use for sd=<name> (same order as pitrexd.sh)
resolve() {
	case "$1" in /*) echo "$1"; return ;; esac
	if [ -f "$2/$1" ]; then echo "$2/$1"
	elif [ -f "$UV/$1" ] || [ ! -f "$UM2/$1" ]; then echo "$UV/$1"
	else echo "$UM2/$1"; fi
}

# Nothing for a card and no card yet: a blank one, so that games can keep scores and settings. It takes no mounting. The empty .list makes
# the next build with files in the card folder replace it, keeping what games saved on it.
blank_card() (
	local free
	trap cleanup EXIT
	[ -f "$OUT" ] && return 0
	command -v python3 > /dev/null && command -v mkfs.vfat > /dev/null || return 0
	mkdir -p "$UV" 2>/dev/null
	exec 9>>"$OUT.lock" || die "cannot open $OUT.lock"
	flock -n 9 || die "a UVMC2 game is running and may be writing to its card: stop it first (Scripts > PiTrex_Stop)"
	[ -f "$OUT" ] && return 0
	free=$(py free "$UV") || return 0
	if [ "$free" -lt $(( BLANK_MIB + 4 )) ]; then say "no room for a blank card in $UV: games play, but cannot save."; return 0; fi
	TMPFILES="$OUT.new"
	py mkimg "$OUT.new" "$BLANK_MIB" && mkfs.vfat -F 32 -s 1 -n "$LABEL" "$OUT.new" > /dev/null 2>&1 || { say "could not make a blank card: games play, but cannot save."; return 0; }
	mv "$OUT.new" "$OUT"
	TMPFILES=""
	: > "$OUT.list"
	say "made a blank card, $OUT, so that games can save. Games that need ROMs want a real one: see ADDING_GAMES.md."
)

# Scale cards (see the header). cfg_get is pitrexd.sh's, so both read a sidecar alike (and no python per game: --check runs at every start).
cfg_get() {
	[ -f "$1" ] || return 0
	tr -d '\r' < "$1" | sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" | tail -1 | sed 's/[[:space:]]*$//'
}
# game_scale <.um2>: the scale a game's card should carry, empty for none. The same rule as start_um2 in pitrexd.sh.
# Built in: Vector Pinball draws its table about 1.35 times too tall for the core at the SDK's default scale (160): the flippers
# fell below the screen. 118 = 160 / 1.35 makes it fit like the console in a video of the game (measured on a MiSTer, 9 Nov 2026).
game_scale() {
	local g=$1 s n
	[ -n "$(cfg_get "$g.cfg" sd)" ] && return 0
	s=$(cfg_get "$g.cfg" scale)
	if [ -z "$s" ]; then n=$(printf '%s' "${g##*/}" | tr '[:upper:]' '[:lower:]'); case "$n" in *vector*pinball*) s=118 ;; esac; fi
	case "$s" in [1-9]|[1-9][0-9]|[1-9][0-9][0-9]) echo "$s" ;; esac
}
SC_SCALES=""
while IFS= read -r g; do
	[ -n "$g" ] || continue
	s=$(game_scale "$g")
	[ -n "$s" ] || continue
	case " $SC_SCALES " in *" $s "*) ;; *) SC_SCALES="$SC_SCALES $s" ;; esac
done <<EOF
$(find "$GAMES" -type f -iname '*.um2' ! -name '._*' 2>/dev/null | sort)
EOF
# scale_cards_ok: every scale card exists and was made from sd.img's current build (same .list; the games' saves do not count)
scale_cards_ok() {
	local n t
	for n in $SC_SCALES; do
		t=$UV/sd_scale$n.img
		[ -f "$t" ] || return 1
		[ "$(cat "$t.list" 2>/dev/null)" = "$(cat "$OUT.list" 2>/dev/null)" ] || return 1
	done
	return 0
}

# --check: nothing to build, or the card (and the scale cards) match the folder: 0. A card made by hand (no .list) is left alone: 0.
if [ "$CHECK" = 1 ]; then
	command -v python3 > /dev/null || exit 0
	[ -n "$(py list "$CARD" 2>/dev/null)" ] || exit 0
	[ -f "$OUT" ] || exit 1
	[ -f "$OUT.list" ] || exit 0
	[ "$(py list "$CARD")" = "$(cat "$OUT.list")" ] || exit 1
	scale_cards_ok || exit 1
	exit 0
fi

# ---------------------------------------------------------------------------------------------
# 1. What is in the card folder
if [ ! -d "$CARD" ] || [ -z "$(ls -A "$CARD" 2>/dev/null)" ]; then
	blank_card || exit $?
	[ "$IF_NEEDED" = 1 ] && exit 0
	say "the card folder $CARD is empty or missing, so there is nothing to put on a card."
	say "Copy the files from your UVMC2 SD card (or just your ROM zips, in a roms folder) into it, then run this again."
	say "See ADDING_GAMES.md in the PiTrex guide. Games that need no card (Veccy Road, the demos) play without one."
	exit 1
fi
command -v python3 > /dev/null || die "python3 not found"

# One folder for the whole SD card: the .um2 games in it are not put on the card, they are copied
# (subfolders kept) to the games folder, where the OSD finds them. Done on every run, also when the card
# needs no rebuild, and before the BattleZone search below, so a BattleZone image that comes this way
# gets its card and sidecar on this run. Nothing in the games folder is ever overwritten.
mkdir -p "$GAMES" 2>/dev/null
NUM2=0; NCOPIED=0
[ "$ONE" = 1 ] || while IFS= read -r l; do
	[ -n "$l" ] || continue
	NUM2=$((NUM2 + 1))
	case "$l" in
		"C "*) NCOPIED=$((NCOPIED + 1)) ;;
		"D "*) say "${l#D }: a different file of that name is already in $GAMES, so it was not copied (left as it is)" ;;
		"E "*) say "could not copy ${l#E } to $GAMES" ;;
	esac
done <<EOF
$(py um2 "$CARD" "$GAMES")
EOF
[ "$NCOPIED" = 0 ] || say "copied $NCOPIED .um2 game(s) from the card folder to $GAMES (the OSD's UVMC2 folder)"

# Overlay art next to each game (no card rebuild for it: .art files are not on the card)
while IFS= read -r g; do
	[ -n "$g" ] || continue
	r=$(art_place "$g")
	[ -z "$r" ] || say "overlay ${g##*/}: $r"
done <<EOF
$(find "$GAMES" -type f -iname '*.um2' ! -name '._*' 2>/dev/null | sort)
EOF

scan=$(py scan "$CARD" "$MAX_MIB") || die "cannot read $CARD"
set -f; set -- $scan; set +f
NFILES=$1; DATA=$(echo "$2" | tr _ ' '); MIB=$3; shift 3; SKIPS="$*"
[ "$SKIPS" = "-" ] && SKIPS="nothing"
if [ "$NFILES" = 0 ]; then
	if [ "$ONE" = 1 ]; then
		blank_card || exit $?
		[ "$IF_NEEDED" = 1 ] && exit 0
		say "no card files in $CARD, only games: nothing to build."
		exit 0
	fi
	if [ "$NUM2" -gt 0 ]; then
		# only games: they are in the games folder now, and there is nothing to make a card from
		blank_card || exit $?
		[ "$IF_NEEDED" = 1 ] && [ "$NCOPIED" = 0 ] && exit 0
		say "the card folder has only .um2 games, which go in the games folder, so there was nothing to build a card from."
		say "Games that need ROMs or other files from the card: copy the rest of your UVMC2 SD card into $CARD too, then run this again."
		exit 0
	fi
	blank_card || exit $?
	[ "$IF_NEEDED" = 1 ] && exit 0
	say "nothing to put on a card in $CARD (skipped: $SKIPS)."
	say "Copy the files from your UVMC2 SD card (or just your ROM zips, in a roms folder) into it, then run this again."
	exit 1
fi

# BattleZone images and the bz_sd.img each would use (normally one, $UV/bz_sd.img)
BZ_IMGS=""
[ "$ONE" = 1 ] || BZ_IMGS=$(find "$GAMES" -type f -iname '*attle*one*.um2' ! -name '._*' 2>/dev/null | sort)
BZ_TARGETS=""
if [ -n "$BZ_IMGS" ]; then
	while IFS= read -r g; do
		t=$(resolve bz_sd.img "${g%/*}")
		# a sidecar that names another card (or none): nothing would use bz_sd.img, so it is not made (reported in step 6)
		if [ -f "$g.cfg" ]; then
			sd=$(py cfg "$g.cfg" sd)
			if [ -z "$sd" ] || [ "$(resolve "$sd" "${g%/*}")" != "$t" ]; then continue; fi
		fi
		# pitrexd refuses card paths with spaces, so such a card would never be used
		case "$t" in *[[:space:]]*) say "BattleZone: $t has a space in its path, which pitrexd does not accept: not made"; continue ;; esac
		case " $BZ_TARGETS " in *" $t "*) ;; *) BZ_TARGETS="$BZ_TARGETS $t" ;; esac
	done <<EOF
$BZ_IMGS
EOF
fi

LIST=$(py list "$CARD") || die "cannot read $CARD"
SC_ONLY=0
if [ "$IF_NEEDED" = 1 ] && [ -f "$OUT" ]; then
	BZ_MISSING=0
	for t in $BZ_TARGETS; do [ -f "$t" ] || BZ_MISSING=1; done
	if [ "$BZ_MISSING" = 1 ]; then
		say "BattleZone has no card of its own yet: building the cards"
	else
		uptodate=0
		if [ -f "$OUT.list" ]; then
			# built by this script: rebuild if the folder's file list (names, sizes, times) changed
			[ "$LIST" = "$(cat "$OUT.list")" ] && uptodate=1
		else
			# a card made some other way: rebuild only if the folder has a file newer than it
			[ -z "$(find "$CARD" -type f ! -iname '*.um2' ! -iname '*.art' ! -iname '*.lock' -newer "$OUT" 2>/dev/null | head -1)" ] && uptodate=1
		fi
		if [ "$uptodate" = 1 ]; then
			# sd.img is fine: only the scale cards, if one is missing or was made from an older sd.img
			scale_cards_ok && exit 0
			say "a game's card with its own scale is missing or older than sd.img: making it"
			SC_ONLY=1
		else
			say "the card folder has changed since sd.img was made: building it again"
		fi
	fi
fi

[ "$SC_ONLY" = 1 ] || say "card folder: $NFILES files, $DATA (skipped: $SKIPS)"
if [ "$SC_ONLY" = 0 ] && [ "$MIB" -gt "$MAX_MIB" ]; then
	say "the card would be $MIB MiB, more than the $MAX_MIB MiB this script makes."
	say "Remove files the games do not need from $CARD (for example ROM sets of games you do not have as .um2), then run this again."
	exit 1
fi

# 2. Checks before anything is written
[ "$(id -u)" = 0 ] || die "run this as root (it mounts the card image)"
for c in mkfs.vfat losetup mount umount flock; do command -v $c > /dev/null || die "$c not found"; done
LOCK=$T/make_card.lock
if ! mkdir "$LOCK" 2>/dev/null; then
	old=$(cat "$LOCK/pid" 2>/dev/null)
	if [ -n "$old" ] && [ -d "/proc/$old" ]; then LOCK=""; die "make_card is already running (process $old)"; fi
	rm -rf "$LOCK"; mkdir "$LOCK" || { LOCK=""; die "cannot create $LOCK"; }
fi
echo $$ > "$LOCK/pid"
mkdir -p "$UV" || die "cannot create $UV"

# Each waiting shell holds its descriptor while the next card is locked and built.
lock_cards() {
	if [ "$#" = 0 ]; then build_cards; return; fi
	local card=$1
	shift
	(
		flock -n 9 || die "a UVMC2 game is running and may be writing to its card: stop it first (Scripts > PiTrex_Stop)"
		lock_cards "$@"
	) 9>>"$card.lock"
}

build_cards() {
trap 'LOCK=""; cleanup' EXIT
# saves folders of a run that was killed hard (we hold the lock, so no other run uses them)
for d in "$UV"/.make_card.*; do [ -d "$d" ] && rm -rf "$d"; done
WORK=$(mktemp -d "$T/make_card.XXXXXX") || die "cannot create a work folder in $T"

# 3. Saves from the old cards (see saves_out above for what is kept). They go to a folder next to the
# cards, not to /tmp: on the MiSTer /tmp is in RAM, and a card made by hand may carry big files.
SAVES=$(mktemp -d "$UV/.make_card.XXXXXX") || die "cannot create a work folder in $UV"
# saves_from <card> <folder>
saves_from() {
	local img=$1 dst=$2 n
	mkdir -p "$dst"
	[ -f "$img" ] || return 0
	attach "$img" "$WORK/mnt_old" ro || die "cannot mount the old ${img##*/} (is it a FAT card image?). Move it away to build a new one."
	n=$(py saves_out "$WORK/mnt_old" "$dst" "$img.list" "$CARD") || die "cannot copy the saves out of ${img##*/}"
	detach "$WORK/mnt_old"
	if [ "$n" = 0 ]; then say "the old ${img##*/} has no saves yet"
	elif [ -f "$img.list" ]; then say "kept the saves of the old ${img##*/} ($n file(s) the games wrote)"
	else say "kept $n file(s) of the old ${img##*/} that are not in the card folder (it was not made by this script)"; fi
}
# Free space: the new card and every BattleZone copy exist next to the old ones until the end.
need_on() { # <folder> <MiB> : enough free space there?
	local free
	free=$(py free "$1") || die "cannot check the free space in $1"
	if [ "$free" -lt $(( $2 + 4 )) ]; then
		say "not enough free space in $1: $2 MiB needed, $free MiB free. Nothing was changed."
		exit 1
	fi
}
# put_saves <card> <saves folder>
put_saves() {
	local n
	[ -n "$(ls -A "$2" 2>/dev/null)" ] || return 0
	attach "$1" "$WORK/mnt_new" || die "cannot mount ${1##*/}"
	n=$(py saves_in "$2" "$WORK/mnt_new") || exit 1
	detach "$WORK/mnt_new"
	[ "$n" = 0 ] || say "put the $n saved file(s) back into ${1%.new}"
}
# 5. (used below) Swap in: old card to .bak (one backup, replaced each time), new card to its name
swap() { # <card>
	local c=$1
	if [ -f "$c" ]; then
		rm -f "$c.bak"
		mv "$c" "$c.bak" || die "cannot rename ${c##*/} to ${c##*/}.bak"
	fi
	mv "$c.new" "$c" || { mv "$c.bak" "$c" 2>/dev/null; die "cannot rename ${c##*/}.new to ${c##*/}"; }
}
# 7. (used below) Scale cards, made from the finished sd.img: its files and saves, then the card's own saves (they win), then
# config/uvm2.cfg with the scale. Defined here so that --if-needed can make them on their own without rebuilding sd.img.
scale_cards() {
	local n t b mib
	for n in $SC_SCALES; do
		t=$UV/sd_scale$n.img
		mib=$(( ( $(wc -c < "$OUT") + 1048575 ) / 1048576 ))
		need_on "$UV" "$mib"
		saves_from "$t" "$SAVES/scale$n"
		rm -f "$t.new"; TMPFILES="$t.new"
		cp "$OUT" "$t.new" || die "cannot write $t.new"
		put_saves "$t.new" "$SAVES/scale$n"
		attach "$t.new" "$WORK/mnt_new" || die "cannot mount ${t##*/}.new"
		py scalecfg "$WORK/mnt_new" "$n" || die "cannot write config/uvm2.cfg on ${t##*/}"
		detach "$WORK/mnt_new"
		if command -v fsck.vfat > /dev/null; then
			fsck.vfat -n "$t.new" > "$WORK/fsck.log" 2>&1 || { cat "$WORK/fsck.log"; die "the new card ${t##*/} has errors (fsck.vfat)"; }
		fi
		b=0; [ -f "$t" ] && b=1
		swap "$t"
		TMPFILES=""
		if [ -f "$OUT.list" ]; then cp "$OUT.list" "$t.list.new" && mv "$t.list.new" "$t.list"; else rm -f "$t.list"; fi
		if [ "$b" = 1 ]; then say "done: $t, the card for games with scale $n (the old one is ${t##*/}.bak)"
		else say "done: $t, the card for games with scale $n"; fi
	done
	sync
}
if [ "$SC_ONLY" = 1 ]; then scale_cards; exit 0; fi
saves_from "$OUT" "$SAVES/main"
i=0
for t in $BZ_TARGETS; do i=$((i + 1)); saves_from "$t" "$SAVES/bz$i"; done

# the card holds the folder's files plus the saves
MIB=$(py size "$CARD" "$MAX_MIB" "$SAVES"/*) || die "cannot work out the card size"
if [ "$MIB" -gt "$MAX_MIB" ]; then
	say "with the saves of the old card, the card would be $MIB MiB, more than the $MAX_MIB MiB this script makes."
	say "Remove files the games do not need from $CARD, then run this again. Nothing was changed."
	exit 1
fi

# Free space (need_on above): the new card and every BattleZone copy exist next to the old ones until the end.
dev_uv=$(py dev "$UV")
need=$MIB
for t in $BZ_TARGETS; do
	if [ "$(py dev "${t%/*}")" = "$dev_uv" ]; then need=$(( need + MIB )); else need_on "${t%/*}" "$MIB"; fi
done
need_on "$UV" "$need"
SECS=1; [ "$MIB" -gt "$BIG_MIB" ] && SECS=8
say "card size: $MIB MiB, FAT32 with $(( SECS * 512 ))-byte clusters"

# 4. Build the new card under a temporary name
# Cluster size: FAT32 needs at least 65525 clusters. With 512-byte clusters (-s 1, the layout um2run and the
# games were tested with) a 64 MiB card has about 129000; at 2000 MiB about 4 million. Bigger cards use 4 KiB.
NEW=$OUT.new
rm -f "$NEW"; TMPFILES="$NEW"
py mkimg "$NEW" "$MIB" || die "cannot create $NEW"
say "formatting"
mkfs.vfat -F 32 -s "$SECS" -n "$LABEL" "$NEW" > "$WORK/mkfs.log" 2>&1 || { cat "$WORK/mkfs.log"; die "mkfs.vfat failed"; }
say "formatted"
attach "$NEW" "$WORK/mnt_new" || die "cannot mount the new card image"
n=$(py copy "$CARD" "$WORK/mnt_new") || exit 1
detach "$WORK/mnt_new"
say "copied $n files"

# BattleZone copies that already exist get the new files plus their own saves: copy the card now, before
# sd.img's saves go in. A BattleZone card made for the first time starts with sd.img's saves (below).
BZ_OLD=""; BZ_FIRST=""; i=0
for t in $BZ_TARGETS; do
	i=$((i + 1))
	if [ -f "$t" ]; then
		rm -f "$t.new"; TMPFILES="$TMPFILES $t.new"
		cp "$NEW" "$t.new" || die "cannot write $t.new"
		BZ_OLD="$BZ_OLD $i"
	else
		BZ_FIRST="$BZ_FIRST $i"
	fi
done

put_saves "$NEW" "$SAVES/main"

i=0
for t in $BZ_TARGETS; do
	i=$((i + 1))
	case " $BZ_FIRST " in *" $i "*)
		rm -f "$t.new"; TMPFILES="$TMPFILES $t.new"
		cp "$NEW" "$t.new" || die "cannot write $t.new" ;;
	esac
	case " $BZ_OLD " in *" $i "*) put_saves "$t.new" "$SAVES/bz$i" ;; esac
done

# check what was built (fsck.vfat is part of dosfstools; skipped if missing)
if command -v fsck.vfat > /dev/null; then
	for f in $NEW $(for t in $BZ_TARGETS; do echo "$t.new"; done); do
		fsck.vfat -n "$f" > "$WORK/fsck.log" 2>&1 || { cat "$WORK/fsck.log"; die "the new card ${f##*/} has errors (fsck.vfat)"; }
	done
	say "checked (fsck.vfat): no errors"
fi

# 5. Swap in (swap above): old card to .bak (one backup, replaced each time), new card to its name
had_old=0; [ -f "$OUT" ] && had_old=1
swap "$OUT"
TMPFILES=""
printf '%s\n' "$LIST" > "$OUT.list.new" && mv "$OUT.list.new" "$OUT.list"
if [ "$had_old" = 1 ]; then say "done: $OUT (the old one is sd.img.bak)"; else say "done: $OUT"; fi
for t in $BZ_TARGETS; do
	TMPFILES="$t.new"
	b=0; [ -f "$t" ] && b=1
	swap "$t"
	TMPFILES=""
	printf '%s\n' "$LIST" > "$t.list.new" && mv "$t.list.new" "$t.list"
	if [ "$b" = 1 ]; then say "done: $t, BattleZone's own card (the old one is ${t##*/}.bak)"; else say "done: $t, BattleZone's own card"; fi
done
sync
scale_cards

# 6. BattleZone sidecars: write one only where there is none; never change an existing one
if [ -n "$BZ_IMGS" ]; then
	while IFS= read -r g; do
		name=${g##*/}
		if [ -f "$g.cfg" ]; then
			sd=$(py cfg "$g.cfg" sd)
			if [ -z "$sd" ]; then
				say "BattleZone: $name.cfg has no sd= line, so this game uses sd.img (shared) and bz_sd.img was not made. To give it its own card, add the line sd=bz_sd.img and build again. The file was left as it is."
			elif [ "$(resolve "$sd" "${g%/*}")" = "$(resolve bz_sd.img "${g%/*}")" ]; then
				say "BattleZone: $name.cfg already says sd=$sd (left as it is)"
			else
				say "BattleZone: $name.cfg says sd=$sd, so bz_sd.img was not made. The file was left as it is; to use a card of its own, change it to sd=bz_sd.img and build again."
			fi
		else
			if printf 'sd=bz_sd.img\n' > "$g.cfg"; then say "BattleZone: wrote $name.cfg with sd=bz_sd.img (its own card)"
			else say "BattleZone: could not write $name.cfg: add a file with the line sd=bz_sd.img next to the game yourself"; fi
		fi
	done <<EOF
$BZ_IMGS
EOF
fi
}

set -- "$OUT"
for t in $BZ_TARGETS; do set -- "$@" "$t"; done
for n in $SC_SCALES; do set -- "$@" "$UV/sd_scale$n.img"; done
lock_cards "$@"

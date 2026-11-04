#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# art_lib.sh: overlay art lookup, shared by pitrexd.sh, make_card.sh and make_menu.sh (sourced, never run; the caller sets FAT first).
# After a pick in the core's OSD, Main loads <picked file>.art from beside it (the core's "f1,ART;" entries), so the art has to be
# there: art_place puts it there. pitrexd reloads the core when a sidecar changes orient/view, which wipes that art: find_art says
# which art the reload should load again.
# Overlay folders: the one overlays.zip is unpacked into for the official core, and the one our MGLs load from.
ART_OLD=$FAT/games/Vectrex/Overlays
ART_NEW=$FAT/games/Vectrex-PiTrex/Overlays

# the same reader as pitrexd.sh's (pitrexd defines its own first); only for the scripts that have none
type cfg_get > /dev/null 2>&1 || cfg_get() {
	[ -f "$1" ] || return 0
	tr -d '\r' < "$1" | sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*//p" | tail -1 | sed 's/[[:space:]]*$//'
}

# art_ci <dir> <name> [-d]: the file (with -d, the folder) in <dir> called <name>, ignoring case; prints its path, else status 1. Shell builtins only.
art_ci() {
	local f b n=${2,,}
	[ "${3:--f}" "$1/$2" ] && { echo "$1/$2"; return 0; }
	for f in "$1"/*; do
		b=${f##*/}
		[ "${b,,}" = "$n" ] && [ "${3:--f}" "$f" ] && { echo "$f"; return 0; }
	done
	return 1
}

# art_path <dir> <name or relative path>: art_ci, with any folders in the name taken as they are
art_path() {
	case "$2" in */*) art_ci "$1/${2%/*}" "${2##*/}" ;; *) art_ci "$1" "$2" ;; esac
}

# art_tate <sidecar>: _90CW / _90CCW when the sidecar rotates the display (orient=cw / ccw), else nothing. Those games take the art
# from the Tate folder of the overlays, where the files are called <name>_90CW.art / <name>_90CCW.art.
art_tate() {
	case "$(cfg_get "$1" orient)" in [Cc][Ww]) echo _90CW ;; [Cc][Cc][Ww]) echo _90CCW ;; esac
}

# art_open <sidecar>: the game asks for view=open. Loaded art makes the core select View Area Standard, so such a game gets no art
# (it would lose its open matte, and pitrexd would reload the core for view=open at every pick).
art_open() {
	local v
	v=$(cfg_get "$1" view)
	[ "${v,,}" = open ]
}

# art_sub <suffix> <name without .art> <overlay folder>...: <name><suffix>.art in the first folder that has it (suffix set: in its Tate folder)
art_sub() {
	local s=$1 n=$2 d
	shift 2
	for d in "$@"; do
		if [ -n "$s" ]; then d=$(art_ci "$d" Tate -d) || continue; fi
		art_ci "$d" "$n$s.art" && return 0
	done
	return 1
}

# art_named <game> <overlay= value>: the file a sidecar names. A path is relative to the game's folder; a bare file name is
# looked up there, then in the two overlay folders. An absolute path is used as it is.
art_named() {
	local d
	[ -n "$2" ] || return 1
	case "$2" in /*) art_ci "${2%/*}" "${2##*/}"; return ;; esac
	art_path "${1%/*}" "$2" && return 0
	case "$2" in */*) return 1 ;; esac
	for d in "$ART_NEW" "$ART_OLD"; do art_ci "$d" "$2" && return 0; done
	return 1
}

# find_art <game file> <sidecar>: the art to load with the game, printed (nothing when there is none). Order:
#   1. overlay= in the sidecar   2. <game>.art beside the game   3. <game>.art in games/Vectrex-PiTrex/Overlays (its Tate version for orient=cw/ccw)
find_art() {
	local n=${1##*/} a
	n=${n%.*}
	art_open "$2" && return 0
	a=$(art_named "$1" "$(cfg_get "$2" overlay)") && { echo "$a"; return 0; }
	art_ci "${1%/*}" "$n.art" && return 0
	art_sub "$(art_tate "$2")" "$n" "$ART_NEW"
}

# art_same <file> <file>: same content (cmp; by size where there is no cmp)
art_same() {
	if type cmp > /dev/null 2>&1; then cmp -s "$1" "$2"; else [ "$(stat -c %s "$1")" = "$(stat -c %s "$2")" ]; fi
}

# art_place <game file>: copy the art for a game next to it as <game>.art, where Main's same-name lookup finds it. Source: overlay= in the
# game's sidecar (<game file>.cfg), else a file with the game's name in games/Vectrex/Overlays or games/Vectrex-PiTrex/Overlays. With
# orient=cw/ccw only the Tate version of the same name counts (a game that is rotated never gets the upright art).
# Prints "copied <art>", "kept yours" (a .art is already there and differs: never replaced), or nothing (no art for the game, or our own
# copy is already there).
art_place() {
	local g=$1 cfg=$1.cfg n=${1##*/} ov s st src dest tmp
	n=${n%.*}
	art_open "$cfg" && return 0
	ov=$(cfg_get "$cfg" overlay)
	s=$(art_tate "$cfg")
	if [ -n "$s" ]; then
		st=$n
		[ -z "$ov" ] || { st=${ov##*/}; st=${st%.*}; }
		src=$(art_sub "$s" "$st" "$ART_OLD" "$ART_NEW") || return 0
	elif [ -n "$ov" ]; then
		src=$(art_named "$g" "$ov") || { echo "overlay=$ov not found"; return 0; }
	else
		src=$(art_sub "" "$n" "$ART_OLD" "$ART_NEW") || return 0
	fi
	if dest=$(art_ci "${g%/*}" "$n.art"); then
		art_same "$src" "$dest" || echo "kept yours"
		return 0
	fi
	dest=${g%/*}/$n.art
	tmp=${g%/*}/.$n.art.part
	if cp "$src" "$tmp" 2> /dev/null && mv "$tmp" "$dest" 2> /dev/null; then
		echo "copied ${src##*/}"
	else
		rm -f "$tmp"
		echo "could not copy ${src##*/}"
	fi
}

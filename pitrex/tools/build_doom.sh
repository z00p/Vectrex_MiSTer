#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build PureDOOM-pitrex (wireframe DOOM) against the MiSTer shim (run in WSL Debian).
#   tools/build_doom.sh [pc|arm|both]     -> build/doom, build/arm/doom
# Upstream renderer.c is used unmodified from upstream/PureDOOM-pitrex; its
# "../../puredoom/PureDOOM.h" include is satisfied by the -I below
# (<dir>/../../puredoom -> upstream/PureDOOM-pitrex/puredoom).
set -e
cd "$(dirname "$0")/.."
U=upstream/PureDOOM-pitrex
which=${1:-both}
export EXTRA_CFLAGS="-I$U/puredoom/src -I$U -I$U/src -Wno-error"
SRCS="games/doom/main_mister.c games/doom/platform_mister.c $U/src/renderer.c"
case $which in
	pc|both)  sh tools/build_prog.sh pc doom $SRCS ;;
esac
case $which in
	arm|both) sh tools/build_prog.sh arm doom $SRCS ;;
esac

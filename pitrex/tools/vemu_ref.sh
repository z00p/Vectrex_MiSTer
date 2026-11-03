#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Reference run of a UVMC2 game in hugonl31's Vectrex_EMU (GPL-3; its own repository, not part of this
# one): builds its headless harness vtest from a clone and runs a script (tools/vemu_*.txt) on a .um2.
# The emulator runs the cartridge in emulated time, so its sound is what the game itself makes; compare
# it with a um2run capture (UM2_WAV=...) using tools/audio_echo.py. Linux or WSL, needs git and cc.
#   tools/vemu_ref.sh BIOS GAME.um2 SD.img SCRIPT.txt OUTDIR
#   e.g. tools/vemu_ref.sh exec_rom.bin pacman-uvmc2-SD-card.um2 uvmc2_sd.img tools/vemu_pacman.txt out
#        python3 tools/audio_echo.py out/pac_ref.wav --stereo --from 12     (audio_echo.py needs numpy)
# BIOS: the Vectrex BIOS, 8192 bytes, CRC32 ba13fb57 (MAME vectrex.zip exec_rom.bin). VEMU_DIR (default
# ~/vemu) keeps the clone and the build; VEMU_TAG picks the version (default v1.5.4, used 3 Nov 2026).
set -e
[ $# -eq 5 ] || { sed -n '7,11p' "$0"; exit 2; }
bios=$(realpath "$1"); game=$(realpath "$2"); sd=$(realpath "$3"); script=$(realpath "$4"); out=$5
V=${VEMU_DIR:-$HOME/vemu}; tag=${VEMU_TAG:-v1.5.4}
[ -d "$V/src/.git" ] || git clone -q --depth 1 --branch "$tag" https://github.com/hugonl31/Vectrex_EMU.git "$V/src"
if [ ! -x "$V/src/core/vectrex_core" ]; then
	(cd "$V/src" && mkdir -p core && cc -O3 -flto -w -ffp-contract=off -fgnu89-inline -o core/vectrex_core \
		src/core/vtest.c src/core/vecx.c src/core/e6809.c src/core/e8910.c src/core/cm33.c src/core/rp2350.c \
		src/core/host.c src/core/arm11.c src/core/usbhost.c -lm -pthread)
fi
mkdir -p "$out"; out=$(realpath "$out")
# the game writes to its card: run on a copy
cp "$sd" "$out/sd.img"
cd "$out" && VT_UVM2_SD="$out/sd.img" "$V/src/core/vectrex_core" "$bios" "$game" "$script" "$out"

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a UVMC2 game (libvpy + the UVM2 runtime, from the UVMC2 team's SDK) for the MiSTer.
#   [UVM2_HZ=0|50] [UVMC2_CFLAGS="..."] tools/build_uvmc2_game.sh <output-name> <game sources...>   (run in WSL)
# Runs tools/patch_uvmc2.py first; needs the ARM beam model (see tools/build_uvmc2.sh).
set -e
cd "$(dirname "$0")/.."
name=$1; shift
python3 -I tools/patch_uvmc2.py > /dev/null
U=upstream/uvmc2-sdk
VXT=${VXDRAW_TARGET:-$HOME/vxdraw-target}
incs=""
for s in "$@"; do incs="$incs -I$(dirname "$s")"; done
CFLAGS="-O2 -DUVM2_HOST -DUVM2_MISTER -DUVM2_PICO_RUNTIME -DUVM2_BENCH_NO_CORE1 -DUVM2_SUBUNITS -DUVM2_HZ=${UVM2_HZ:-50}
  -DUVM2_CMD_CAPACITY=32768u -Dmain=uvm2_game_main -I$U/uvm2-sdk -I$U/vpy-c/include -I$U/vpy-c -I$U/pitrex-sim/include -Isrc -w"
RUNTIME="build/uvmc2/sdk_rp2350.c build/uvmc2/uvm2_svc.c $U/uvm2-sdk/uvm2_draw.c $U/uvm2-sdk/uvm2_text.c
  $U/uvm2-sdk/uvm2_audio.c build/uvmc2/uvm2_smp.c $U/uvm2-sdk/uvm2_input.c
  src/uvm2_bus_mister.c src/uvm2_mister_stubs.c src/ptx_core.c src/ptx_backend_file.c src/ptx_backend_mister.c"
VPY=$(ls $U/vpy-c/*.c)
mkdir -p build/arm
arm-linux-gnueabihf-gcc $CFLAGS $UVMC2_CFLAGS -ffunction-sections -fdata-sections -Wl,--gc-sections -static $incs -o build/arm/$name "$@" $VPY $RUNTIME \
  $VXT/armv7-unknown-linux-gnueabihf/release/libvectrex_draw_cabi.a -lm -lpthread -ldl
echo "built build/arm/$name"

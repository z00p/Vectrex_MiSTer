#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a UVMC2 program (UVM2 runtime from the UVMC2 team's SDK) on the MiSTer stream.
#   tools/build_uvmc2.sh pc|arm <output-name> <source.c> [more sources...]   (run in WSL)
# Needs the beam model built first: (cd upstream/uvmc2-sdk/vectrex-draw/cabi &&
#   cargo build --release --target <x86_64-unknown-linux-gnu|armv7-unknown-linux-gnueabihf>)
set -e
cd "$(dirname "$0")/.."
target=$1; name=$2; shift 2
SDK=upstream/uvmc2-sdk/uvm2-sdk
VXT=${VXDRAW_TARGET:-$HOME/vxdraw-target}
CFLAGS="-O2 -DUVM2_HOST -DUVM2_BENCH_NO_CORE1 -DUVM2_SUBUNITS -DUVM2_HZ=50 -DUVM2_CMD_CAPACITY=32768u -I$SDK -Isrc -w"
SRC="$SDK/uvm2_draw.c src/uvm2_bus_mister.c src/ptx_core.c src/ptx_backend_file.c src/ptx_backend_mister.c"
case $target in
	pc)  out=build/$name; cc=cc; extra=""; lib=$VXT/x86_64-unknown-linux-gnu/release/libvectrex_draw_cabi.a ;;
	arm) out=build/arm/$name; cc=arm-linux-gnueabihf-gcc; extra="-static"; mkdir -p build/arm
	     lib=$VXT/armv7-unknown-linux-gnueabihf/release/libvectrex_draw_cabi.a ;;
	*)   echo "target must be pc or arm" >&2; exit 2 ;;
esac
$cc $CFLAGS $extra -o "$out" "$@" $SRC "$lib" -lm -lpthread -ldl
echo "built $out"

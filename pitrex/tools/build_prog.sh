#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a PiTrex program against the MiSTer shim (run in WSL Debian).
#   tools/build_prog.sh pc|arm <output-name> <source.c> [more sources...]
# PC binaries go to build/<name>, MiSTer (static ARM) binaries to build/arm/<name>.
# Each source's own folder is on the include path (for local .h/.i files).
set -e
cd "$(dirname "$0")/.."
target=$1; name=$2; shift 2
[ -f build/lib/vectrex/vectrexInterface.c ] || python3 -I tools/patch_lib.py
incs=""
for s in "$@"; do incs="$incs -I$(dirname "$s")"; done
CFLAGS="-O2 -DPITREX_MISTER -DPIZERO -DRPI0 -DAVOID_TICKS -DDISABLE_FIQ -Ibuild/lib -Ibuild/lib/vectrex -Isrc -w"
SHIM="src/ptx_core.c src/ptx_backend_file.c src/ptx_backend_mister.c src/pitrexio-mister.c"
LIB="build/lib/vectrex/vectrexInterface.c build/lib/vectrex/osWrapper.c build/lib/vectrex/baremetalUtil.c build/lib/vectrex/ini.c build/lib/pitrex/bcm2835.c"
case $target in
	pc)  out=build/$name; cc=cc; extra="" ;;
	arm) out=build/arm/$name; cc=arm-linux-gnueabihf-gcc; extra="-static"; mkdir -p build/arm ;;
	*)   echo "target must be pc or arm" >&2; exit 2 ;;
esac
$cc $CFLAGS $extra $incs -DSETTINGS_DIR='"./settings"' -o "$out" "$@" $SHIM $LIB -lm
echo "built $out"

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the UVMC2 team's Tac/Scan port for the MiSTer (run in WSL/Linux).
# Needs: upstream/uvmc2-starter-kit (github.com/tullulah/uvmc2-starter-kit) and the ARM beam
# model (see tools/build_uvmc2.sh). Output: build/arm/tacscan (build/pc/tacscan with GAME_TARGET=pc).
# Free refresh (UVM2_HZ=0): the samples are slotted into the drawing list, and a fixed rate
# leaves most of the frame where they can't go (see the game's Makefile).
# The game's include/ (freestanding libc headers for the cartridge) must NOT be on the path.
#
# Z80 core:  TACSCAN_Z80=aae  (default) Tim Cottrill's AAE Z80, The Unlicense  -> shippable
#            TACSCAN_Z80=mz80 the old Neil Bradley mz80 (licence forbids redistributing modified
#                             copies; the AAE tree's mz80.c is modified)       -> NOT shippable
# The aae build compiles third_party/aae_z80/cpu_z80.cpp (C++, needs g++ for the target:
# arm-linux-gnueabihf-g++, or host g++ with GAME_TARGET=pc) plus the adapter
# games/uvmc2_tacscan/z80shim/mz80_aae.cpp, and links no mz80 code at all.
cd "$(dirname "$0")/.." || exit 9
K=upstream/uvmc2-starter-kit
G=$K/game/tacscan
A=$K/third_party/aae
ZS=games/uvmc2_tacscan/z80shim
ZA=third_party/aae_z80
export UVM2_HZ=0
Z80=${TACSCAN_Z80:-aae}
if [ "${GAME_TARGET:-arm}" = pc ]; then CXX=${CXX_PC:-g++}; else CXX=${CXX_ARM:-arm-linux-gnueabihf-g++}; fi
COMMON_CFLAGS="-std=gnu11 -DVPY_RP2350 -DCCNT0(x)=do{}while(0) -include $G/src/aae_compat.h -include src/uvmc2_psram.h -I$G/src -I$A"
case $Z80 in
mz80)
  export UVMC2_CFLAGS="$COMMON_CFLAGS -DMZ80_IDLE_SKIP"
  Z80_SRC="$A/mz80/mz80.c"
  ;;
aae)
  command -v "$CXX" >/dev/null || { echo "build_tacscan: $CXX not found (arm: sudo apt install g++-arm-linux-gnueabihf)" >&2; exit 3; }
  mkdir -p build/z80
  for f in $ZA/cpu_z80.cpp $ZS/mz80_aae.cpp; do
    $CXX -O2 -std=gnu++17 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -w \
      -I$ZS -I$ZA -c $f -o build/z80/$(basename "$f" .cpp).o || exit 4
  done
  export UVMC2_CFLAGS="$COMMON_CFLAGS -include $ZS/mz80_compat.h"
  export UVMC2_LIBS="-lstdc++"
  Z80_SRC="build/z80/cpu_z80.o build/z80/mz80_aae.o"
  ;;
*) echo "TACSCAN_Z80 must be aae or mz80" >&2; exit 2 ;;
esac
exec sh tools/build_uvmc2_game.sh tacscan $G/src/main.c $G/src/aae_stubs.c $G/src/aae_machine.c $G/src/samples.c $G/src/ts_audio.c build/uvmc2/ts_jack.c \
  $A/aae_romload.c $A/aae_romload_gi.c $A/romzip.c $A/puff.c $A/SegaG80.c $A/SegaG80snd.c $Z80_SRC $A/cpuintrf.c $A/cpu_control.c $A/rand.c $A/acommon.c

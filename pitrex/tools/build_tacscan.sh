#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the UVMC2 team's Tac/Scan port for the MiSTer (run in WSL/Linux).
# Needs: upstream/uvmc2-starter-kit (github.com/tullulah/uvmc2-starter-kit) and the ARM beam
# model (see tools/build_uvmc2.sh). Output: build/arm/tacscan.
# Free refresh (UVM2_HZ=0): the samples are slotted into the drawing list, and a fixed rate
# leaves most of the frame where they can't go (see the game's Makefile).
# The game's include/ (freestanding libc headers for the cartridge) must NOT be on the path.
cd "$(dirname "$0")/.." || exit 9
K=upstream/uvmc2-starter-kit
G=$K/game/tacscan
A=$K/third_party/aae
export UVM2_HZ=0
export UVMC2_CFLAGS="-std=gnu11 -DMZ80_IDLE_SKIP -DVPY_RP2350 -DCCNT0(x)=do{}while(0) -include $G/src/aae_compat.h -include src/uvmc2_psram.h -I$G/src -I$A"
exec sh tools/build_uvmc2_game.sh tacscan $G/src/main.c $G/src/aae_stubs.c $G/src/aae_machine.c $G/src/samples.c $G/src/ts_audio.c build/uvmc2/ts_jack.c \
  $A/aae_romload.c $A/aae_romload_gi.c $A/romzip.c $A/puff.c $A/SegaG80.c $A/SegaG80snd.c $A/mz80/mz80.c $A/cpuintrf.c $A/cpu_control.c $A/rand.c $A/acommon.c

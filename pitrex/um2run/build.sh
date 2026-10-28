#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Static ARM (Cortex-A9) build of um2run, on a PC with arm-linux-gnueabihf-gcc (Debian/Ubuntu: apt install gcc-arm-linux-gnueabihf).
# Usage: ./build.sh        (from this directory; the PiTrex sources are found in ../src, or ../../src in the PC project tree)
set -e
S=../src
[ -f $S/ptx_core.c ] || S=../../src
arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-parameter -static -pthread -I$S -o um2run     um2run.c um2bus.c um2sd.c um2aud.c $S/ptx_core.c $S/ptx_backend_file.c $S/ptx_backend_mister.c -lm
# optional seccomp-bpf svc trap variant (needs a kernel with CONFIG_SECCOMP; the MiSTer kernel has none):
# arm-linux-gnueabihf-gcc -O2 -static -DUSE_SECCOMP -o um2run-seccomp um2run.c ...
arm-linux-gnueabihf-size um2run
# host build of the device model alone (replay tool: feeds a capture recorded with `um2run --bus --capture FILE` through the model)
cc -O1 -Wall -Wextra -Wno-unused-parameter -I$S -o um2bus_replay um2bus_replay.c um2bus.c um2sd.c um2aud.c $S/ptx_core.c $S/ptx_backend_file.c $S/ptx_backend_mister.c -lpthread -lm

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run a harness-built program through the file backend and render frames.
#   tools/run_stream.sh <name> <frames> <first> <count> [pots "x1,y1,x2,y2"] [buttons 0xNN] [buttons-from-frame]
cd "$(dirname "$0")/../build" || exit 1
name=$1; frames=$2; first=$3; count=$4; pot=${5:-0,0,0,0}; btn=${6:-0xFF}
mkdir -p frames
PTX_STREAM=$name.ptxs PTX_MAX_FRAMES=$frames PTX_POT=$pot PTX_BUTTONS=$btn PTX_BUTTONS_FROM=${7:-0} timeout 120 ./$name > $name.out 2>&1
echo "$name exit $?"; tail -2 $name.out
./ptxplay $name.ptxs frames/$name $first $count

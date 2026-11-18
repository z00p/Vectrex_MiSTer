#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The ring backend's frame wait against a fake core (tests/ptx_wait_test.c), new wait and old polling side by side. PC (WSL), gcc.
set -e
cd "$(dirname "$0")/.."
for v in predict poll; do
	f=""; [ $v = poll ] && f=-DPTX_WAIT_POLL_ONLY
	gcc -O2 -pthread -DPTX_DDR_BASE=0 -DPTX_DEVMEM='"/tmp/ptx_wait_test.mem"' $f -Isrc -o /tmp/ptx_wait_test_$v \
	    tests/ptx_wait_test.c src/ptx_core.c src/ptx_backend_file.c src/ptx_backend_mister.c -lm
done
for v in predict poll; do echo "== $v, no pause"; PW_NOPAUSE=1 /tmp/ptx_wait_test_$v; echo "== $v, 3 s pause at 3 s"; /tmp/ptx_wait_test_$v; done
rm -f /tmp/ptx_wait_test.mem

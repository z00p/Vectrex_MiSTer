#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic-guest self test for um2run's stub / patch machinery (no private images needed).  Run under WSL on the PC (qemu-user):
#   ./selftest/selftest.sh [um2run-binary]        default binary: ./um2run (the ARM build); test 1 builds its own -DUM2_VFPTEST variant
#   1. vfp:  VFP d0-d7 + FPSCR + flags survive a stub's C helper (the test build's helpers clobber them) and the helper runs on the host stack
#   2. race: both cores run the same 48 hot-patch sites in lock step, 30 runs (an unparked patch showed "STOP: undecodable memory fault" in half of them)
#   3. v8:   LDA/STL/LDAEX/STLEX become stubs with dmb ish and still work (one conditional LDA in an IT block stays on the in-place path)
#   4. pre:  `ldr rX,[pc,#imm]` + a load whose next halfword is a branch target: patched from site - 2 (the access that trapped finishes in the emulation), registers/sp intact
#   5. prim: `mrs rd,PRIMASK ; cpsid i` / `msr PRIMASK,rd` pairs (7 registers incl. r12, lr) become stubs; mask values and registers stay right
#   6. aud:  cart audio: a guest sets up the SDK's PIO2 I2S program + an endless ring DMA like the UVMC2 images and plays a 1 kHz square wave through it; the DMA READ_ADDR
#            must advance at the I2S rate (32 kHz from clk_sys / clkdiv: 62400 frames in ~1950 ms) and the .wav (UM2_WAV) must hold the tone
#   7. vsel: ARMv8 `vsel{eq,vs,ge,gt}.f32` (UNDEFINED on the A9; BattleZone's audio synthesis runs ~60000/s) is emulated at the first trap and patched to a stub
#            (ite cc; vmov; vmov; b.w); 17 sites x 40 iterations with ordered/equal/unordered compares, results and flags must be right
#   8. alarm: TIMER0 alarm path: past targets stored from thread mode (inline fire + retry of the refused self-kick), future targets (irq thread), and the SDK-style repeating
#            alarm re-armed from its own ISR (3000 ISRs); every ISR must come, none may be lost
# Work files go to /tmp/um2self.  Expected: eight PASS lines.
H=$(cd "$(dirname "$0")" && pwd); T=$H/..; S=$T/../../src; [ -f $S/ptx_core.c ] || S=$T/../src; W=/tmp/um2self; mkdir -p $W; cd $W || exit 1
BIN=${1:-$T/um2run}
run() { PTX_STREAM=$W/s.ptxs timeout 60 qemu-arm -cpu cortex-a9 "$@"; }
mark() { grep -E "^ +[0-9]+ +40001000" "$1" | head -1 | sed 's/.*last=//'; }
(cd $T && arm-linux-gnueabihf-gcc -O2 -static -pthread -I$S -I$T -DUM2_VFPTEST -o $W/um2run_vt um2run.c um2bus.c um2sd.c um2aud.c $S/ptx_core.c $S/ptx_backend_file.c $S/ptx_backend_mister.c -lm) || { echo "build failed"; exit 1; }
for g in gen_vfp gen_race gen_v8 gen_pre gen_prim gen_aud gen_vsel gen_alarm; do python3 $H/$g.py >/dev/null 2>&1 || { echo "$g failed"; exit 1; }; done
run $W/um2run_vt --bus --bus-verbose --no-loading --secs 3 --log 100 $W/vfptest.um2 > vfp.out 2> vfp.err
[ "$(mark vfp.out)" = c0de0000 ] && grep -q ", 0 of them on the guest stack" vfp.err && echo "vfp:  PASS" || echo "vfp:  FAIL ($(mark vfp.out))"
ok=0; for i in $(seq 1 30); do
  run "$BIN" --bus --bus-verbose --no-loading --secs 1 --log 100 $W/race.um2 > race.out 2> race.err
  m=$(grep -E "^ +[0-9]+ +4000100[04]" race.out | sed 's/.*last=//' | sort -u | tr '\n' ' ')
  [ "$m" = "c0de0000 c0de0100 " ] && ! grep -q STOP race.out && ok=$((ok+1))
done; [ $ok = 30 ] && echo "race: PASS (30/30)" || echo "race: FAIL ($ok/30)"
run "$BIN" --bus --bus-verbose --no-loading --secs 1 --log 100 $W/v8.um2 > v8.out 2> v8.err
n=$(grep -c "^v8_stub" v8.err); [ "$(mark v8.out)" = c0de0000 ] && [ "$n" = 4 ] && echo "v8:   PASS (4 dmb stubs)" || echo "v8:   FAIL ($(mark v8.out), $n stubs)"
run "$BIN" --bus --bus-verbose --no-loading --secs 1 --log 100 $W/pre.um2 > pre.out 2> pre.err
n=$(grep -c "PRE(site-2)" pre.err); [ "$(mark pre.out)" = c0de0000 ] && [ "$n" -ge 8 ] && echo "pre:  PASS ($n constant-load stubs)" || echo "pre:  FAIL ($(mark pre.out), $n stubs)"
run "$BIN" --bus --bus-verbose --no-loading --secs 1 --log 100 $W/prim.um2 > prim.out 2> prim.err
n=$(grep -c "^prim_patch" prim.err); [ "$(mark prim.out)" = c0de0000 ] && [ "$n" -ge 14 ] && echo "prim: PASS ($n PRIMASK stubs)" || echo "prim: FAIL ($(mark prim.out), $n stubs)"
UM2_WAV=$W/aud.wav run "$BIN" --bus --bus-verbose --no-loading --secs 6 --log 100 $W/aud.um2 > aud.out 2> aud.err
el=$(grep -E "^ +[0-9]+ +40001004" aud.out | head -1 | sed 's/.*last=//')
wv=$(python3 -c "
import wave, struct
w = wave.open('$W/aud.wav'); n = w.getnframes(); x = struct.unpack('<%dh' % n, w.readframes(n))
idx = [i for i, v in enumerate(x) if abs(v) > 2000]
if len(idx) < 20000: print(len(idx), 0)
else:
    seg = x[idx[0]:idx[-1]]; zc = sum(1 for a, b in zip(seg, seg[1:]) if a <= 0 < b); print(len(idx), int(zc * 32000.0 / len(seg)))
" 2>/dev/null)
ms=$(( 0x${el:-0} - 0xe1000000 )); nl=${wv% *}; hz=${wv#* }
[ "$(mark aud.out)" = c0de0000 ] && [ "$ms" -ge 1850 ] && [ "$ms" -le 2100 ] && [ "${nl:-0}" -ge 60000 ] && [ "${hz:-0}" -ge 990 ] && [ "${hz:-0}" -le 1010 ] && echo "aud:  PASS (62400 frames in $ms ms, tone $hz Hz, $nl loud samples)" || echo "aud:  FAIL (result $(mark aud.out), $ms ms, wav '$wv')"
run "$BIN" --bus --bus-verbose --no-loading --secs 2 --log 100 $W/vsel.um2 > vsel.out 2> vsel.err
n=$(grep -c "VSEL.F32 emulated, patched to a stub" vsel.out); [ "$(mark vsel.out)" = c0de0000 ] && [ "$n" -ge 17 ] && echo "vsel: PASS ($n VSEL stubs)" || echo "vsel: FAIL ($(mark vsel.out), $n stubs)"
run "$BIN" --bus --bus-verbose --no-loading --secs 20 --log 100 $W/alarm.um2 > alarm.out 2> alarm.err
am=$(mark alarm.out); ae=$(grep -E "^ +[0-9]+ +40001004" alarm.out | head -1 | sed 's/.*last=//'); ac=$(grep -E "^ +[0-9]+ +40001008" alarm.out | head -1 | sed 's/.*last=//')
ams=$(( 0x${ae:-0} - 0xe1000000 )); acn=$(( 0x${ac:-0} - 0xc0000000 ))
[ "$am" = c0de0000 ] && [ "$acn" -ge 3300 ] && [ "$ams" -ge 0 ] && [ "$ams" -le 15000 ] && echo "alarm: PASS ($acn ISRs, chain of 3000 in $ams ms)" || echo "alarm: FAIL (flags $am, $acn ISRs, $ams ms)"

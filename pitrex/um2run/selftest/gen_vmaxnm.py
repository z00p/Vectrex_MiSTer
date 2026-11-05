# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for VMAXNM/VMINNM.F32 (5 Nov 2026): ARMv8 FP, UNDEFINED on the A9 (Vector Tank clamps with a vmaxnm + vminnm pair).  The first trap emulates the
# instruction on the signal frame's VFP state and patches the site to a stub that calls um2_vmaxnm through the thunk.  10 sites (max and min x 5 register sets,
# Sd = Sm as in Vector Tank, Sd = Sn, odd and even, high registers), each run ITER times over 11 operand pairs (order, +0 / -0, quiet and signalling NaN, infinity,
# equal values); the result bits must match, the flags (Z set before each) and the other VFP registers must survive.  Result word 0xC0DE0000 | error bits at
# 0x40001000 (0 = all good).  Argument: ITER (default 12).
import struct, subprocess, sys
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 12
def f2b(x): return struct.unpack("<I", struct.pack("<f", x))[0]
QNAN, SNAN, PZ, NZ = 0x7FC00001, 0x7F800005, 0x00000000, 0x80000000
PAIRS = [(f2b(1.5), f2b(-2.0)), (f2b(-2.0), f2b(1.5)), (PZ, NZ), (NZ, PZ), (QNAN, f2b(3.0)), (f2b(3.0), QNAN),
         (SNAN, f2b(3.0)), (f2b(3.0), SNAN), (f2b(float("inf")), f2b(5.0)), (f2b(float("-inf")), f2b(5.0)), (f2b(2.25), f2b(2.25))]
def isnan(b): return (b & 0x7FFFFFFF) > 0x7F800000
def tof(b): return struct.unpack("<f", struct.pack("<I", b))[0]
def ref(a, b, mn):                                       # IEEE 754-2008 maxNum / minNum as the ARMv8 FP instructions give it (FPSCR.DN = 0)
    if isnan(a) or isnan(b):
        if isnan(a) and not a & 0x400000: return a | 0x400000
        if isnan(b) and not b & 0x400000: return b | 0x400000
        if isnan(a) and isnan(b): return a
        return b if isnan(a) else a
    x, y = tof(a), tof(b)
    if x == y:
        if x == 0: return NZ if (mn and (a | b) & NZ) or (not mn and a & b & NZ) else PZ
        return a
    return a if (x < y if mn else x > y) else b
S = """
.syntax unified
.cpu cortex-m33
.fpu fpv5-sp-d16
.thumb
.text
vt: .word 0x20082000, start+1
    .rept 14
    .word dummy+1
    .endr
dummy: b .
.thumb_func
start:
    movs r7, #0
    ldr r8, =%d
    vmov.f32 s5, #1.5
    vmov.f32 s20, #-3.0
iter:
""" % ITER
bit = 0
for mn in (0, 1):
    for (sd, sn, sm) in ((15, 16, 15), (15, 15, 14), (2, 3, 4), (30, 17, 9), (0, 31, 1)):
        for (a, b) in PAIRS:
            S += """    ldr r0, =0x%08x
    vmov s%d, r0
    ldr r0, =0x%08x
    vmov s%d, r0
    movs r2, #0
    v%snm.f32 s%d, s%d, s%d
    it ne
    orrne r7, r7, #0x%x
    vmov r0, s%d
    ldr r1, =0x%08x
    cmp r0, r1
    it ne
    orrne r7, r7, #0x%x
""" % (a, sn, b, sm, "min" if mn else "max", sd, sn, sm, 0x8000, sd, ref(a, b, mn), 1 << (bit % 12))
        bit += 1
        S += "    b.w 1f\n.ltorg\n1:\n"                       # keep the literal pool within ldr's reach
S += """    vmov r0, s5
    ldr r1, =0x3fc00000
    cmp r0, r1
    it ne
    orrne r7, r7, #0x4000
    vmov r0, s20
    ldr r1, =0xc0400000
    cmp r0, r1
    it ne
    orrne r7, r7, #0x4000
    subs r8, r8, #1
    bne.w iter
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    ldr r1, =0x40001000
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.ltorg
"""
open("vm.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "vm.o", "vm.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "vm.elf", "vm.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "vm.elf", "vm.bin"])
b = open("vm.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("vmaxnm.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

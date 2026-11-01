# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for VRINTA/VRINTN/VRINTP/VRINTM.F32 (1 Nov 2026): ARMv8 FP, UNDEFINED on the A9.  The first trap emulates the instruction on the signal frame's
# VFP state and patches the site to a stub that calls um2_vrint through the thunk.  12 sites (4 modes x 3 register pairs, odd and even Sd / Sm, high registers), each
# run ITER times over 9 inputs (ties, negatives, fractions below 1, huge, -0.0); the result bits must match Python's, the flags (Z set before each) and the other VFP
# registers must survive.  Result word 0xC0DE0000 | error bits at 0x40001000 (0 = all good).  Argument: ITER (default 12).
import math, struct, subprocess, sys
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 12
def f2b(x): return struct.unpack("<I", struct.pack("<f", x))[0]
def rnd(x, m):
    if x == 0 or abs(x) >= 8388608.0: return x
    if m == "m": r = math.floor(x)
    elif m == "p": r = math.ceil(x)
    elif m == "a": r = math.floor(abs(x) + 0.5) * (1 if x > 0 else -1)
    else: r = round(x)                                    # Python rounds half to even
    return math.copysign(0.0, x) if r == 0 else float(r)
VALS = [2.5, -2.5, 1.2, -1.2, 0.3, -0.3, 3.5, 1e9, -0.0]
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
for mode in "anpm":
    for (sd, sm) in ((30, 17), (2, 3), (9, 24)):
        for x in VALS:
            e = f2b(rnd(x, mode))
            S += """    ldr r0, =0x%08x
    vmov s%d, r0
    movs r2, #0
    vrint%s.f32 s%d, s%d
    it ne
    orrne r7, r7, #0x%x
    vmov r0, s%d
    ldr r1, =0x%08x
    cmp r0, r1
    it ne
    orrne r7, r7, #0x%x
""" % (f2b(x), sm, mode, sd, sm, 0x8000, sd, e, 1 << (bit % 12))
        bit += 1
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
open("vr.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "vr.o", "vr.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "vr.elf", "vr.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "vr.elf", "vr.bin"])
b = open("vr.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("vrint.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

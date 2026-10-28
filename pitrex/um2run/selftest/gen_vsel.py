# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the VSEL.F32 emulation + stub (28 Oct 2026): ARMv8 FP `vsel{eq,vs,ge,gt}.f32` is UNDEFINED on the A9.  The guest sets the flags with vcmp/vmrs (ordered less,
# greater, equal, unordered), executes 14 vsel sites 40 times each and checks every destination (also Sd = Sn and Sd = Sm).  The first trap at a site emulates it and
# patches it to `b.w stub` (ite cc; vmov; vmov; b.w back), so the later iterations also run the stubs and the flags must survive them.
# Result word 0xC0DE0000 | error bits at 0x40001000 (0 = ok).
import struct, subprocess

CC = {"eq": 0, "vs": 1, "ge": 2, "gt": 3}
def vsel(cc, sd, sn, sm):
    h1 = 0xFE00 | ((sd & 1) << 6) | (CC[cc] << 4) | (sn >> 1)
    h2 = ((sd >> 1) << 12) | 0x0A00 | ((sn & 1) << 7) | ((sm & 1) << 5) | (sm >> 1)
    return "    .inst.w 0x%04x%04x    @ vsel%s.f32 s%d, s%d, s%d\n" % (h1, h2, cc, sd, sn, sm)

A, B = 1, 2                                   # s1 = 1.0, s2 = 2.0 ; s20 = NaN ; expected values are the registers holding A / B
cases = [                                      # (compare, [(cc, sd, sn, sm, expect)])  expect: 'A' = value of s1 at the start (1.0), 'B' = s2 (2.0)
    ("vcmp.f32 s1, s2",  [("ge", 3, 1, 2, "B"), ("gt", 4, 1, 2, "B"), ("eq", 5, 1, 2, "B"), ("vs", 6, 1, 2, "B")]),     # 1 < 2: N=1
    ("vcmp.f32 s2, s1",  [("ge", 7, 1, 2, "A"), ("gt", 8, 1, 2, "A"), ("eq", 9, 1, 2, "B"), ("vs", 10, 1, 2, "B")]),    # 2 > 1: C=1
    ("vcmp.f32 s1, s1",  [("ge", 11, 1, 2, "A"), ("gt", 12, 1, 2, "B"), ("eq", 13, 1, 2, "A")]),                        # equal: Z=1
    ("vcmp.f32 s1, s20", [("vs", 14, 1, 2, "A"), ("ge", 15, 1, 2, "B"), ("gt", 16, 1, 2, "B")]),                        # unordered: V=1
]
S = r'''
.syntax unified
.arch armv7-a
.fpu vfpv3-d16
.thumb
.text
vt: .word 0x20082000, start+1
    .rept 14
    .word dummy+1
    .endr
dummy: b .
.thumb_func
start:
    ldr r5, =0x40001000
    movs r6, #0          @ iteration
    movs r7, #0          @ error bits
    ldr r8, =0x3f800000  @ 1.0
    ldr r9, =0x40000000  @ 2.0
    ldr r10, =0x7fc00000 @ NaN
loop:
    vmov s1, r8
    vmov s2, r9
    vmov s20, r10
'''
for cmp_, items in cases:
    S += "    %s\n    vmrs APSR_nzcv, fpscr\n" % cmp_
    for cc, sd, sn, sm, ex in items:
        S += vsel(cc, sd, sn, sm)
    for cc, sd, sn, sm, ex in items:
        S += "    vmov r0, s%d\n    cmp r0, %s\n    it ne\n    orrne r7, r7, #1\n" % (sd, "r8" if ex == "A" else "r9")
# Sd = Sn and Sd = Sm (flags: 2 > 1 -> ge true, gt true)
S += "    vcmp.f32 s2, s1\n    vmrs APSR_nzcv, fpscr\n"
S += vsel("ge", 21, 1, 2) + vsel("ge", 22, 1, 22)       # s21 = A (true: s1); s22 (= s22 as Sm, true: takes s1 into s22)
S += vsel("gt", 2, 1, 2)                                 # Sd = Sm: true -> s2 = s1 (= A)
S += "    vmov r0, s21\n    cmp r0, r8\n    it ne\n    orrne r7, r7, #2\n    vmov r0, s22\n    cmp r0, r8\n    it ne\n    orrne r7, r7, #4\n    vmov r0, s2\n    cmp r0, r8\n    it ne\n    orrne r7, r7, #8\n"
S += '''    adds r6, r6, #1
    cmp r6, #40
    blt loop
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    str r0, [r5]
    str r0, [r5]
    str r0, [r5]
done:
    b done
.ltorg
'''
open("vsel.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "vsel.o", "vsel.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-Ttext=0x20000000", "-o", "vsel.elf", "vsel.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "vsel.elf", "vsel.bin"])
b = open("vsel.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("vsel.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for two-instruction IT blocks (2 Nov 2026): `ite cc ; str<cc> rt,[rn,#imm] ; str<!cc> rt,[rn,#imm2]` is Contrex's bit-banged SPI MOSI (SIO GPIO_HI_OUT_SET /
# _CLR, 3600 traps a frame while it streams music from the card) and its relatives.  A site that is the first or the second instruction of such a block is patched from the
# `it` into a stub (ITE: one stub part per instruction, chosen by the condition; ITT: both instructions on the condition).  The echo device is SIO GPIO_OUT: a store to
# 0xD0000010 reads back from 0xD0000010; 0xD0000018 sets bits (GPIO_OUT_SET), 0xD0000020 clears bits (GPIO_OUT_CLR).  Every site runs ITER times (R4 alternates 0 / 1 so
# the first and the second instruction of every block trap in turn); the guest checks the values, callee-saved registers, sp and the flags the block must preserve.
#   Q1  ite eq: streq SET / strne CLR      Q2  ite ne: strne SET / streq CLR (inverse condition first)      Q3  itt eq: streq SET / ldreq (read back)
#   Q4  ite eq: ldreq / strne      Q5  itt ne: strne CLR / ldrne      Q6  ite mi: strmi SET / strpl CLR (the flags from a `lsls #31` like Contrex's bit extraction)
# Result word 0xC0DE0000 | error bits at 0x40001000 (0 = all good).  Arguments: ITER (default 60).
import struct, subprocess, sys
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 60
S = """
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
.macro SETUP
    ldr r2, =0xD0000000
.endm
.macro CHK reg, val, bit
    cmp \\reg, \\val
    it ne
    orrne r7, r7, #\\bit
.endm
.macro Q1 k
    SETUP
    movs r6, #8
    cmp r4, #1
    ite eq
    moveq r0, #0
    movne r0, #8
    str r0, [r2, #16]
    cmp r4, #1
    ite eq
    streq r6, [r2, #24]
    strne r6, [r2, #32]
    ldr r5, [r2, #16]
    cmp r4, #1
    ite eq
    moveq r1, #8
    movne r1, #0
    CHK r5, r1, 1
    CHK r6, #8, 1
.endm
.macro Q2 k
    SETUP
    movs r6, #8
    cmp r4, #1
    ite ne
    movne r0, #0
    moveq r0, #8
    str r0, [r2, #16]
    cmp r4, #1
    ite ne
    strne r6, [r2, #24]
    streq r6, [r2, #32]
    ldr r5, [r2, #16]
    cmp r4, #1
    ite eq
    moveq r1, #0
    movne r1, #8
    CHK r5, r1, 2
.endm
.macro Q3 k
    SETUP
    movs r6, #8
    movs r0, #0
    str r0, [r2, #16]
    movs r0, #77
    cmp r4, #1
    itt eq
    streq r6, [r2, #24]
    ldreq r0, [r2, #16]
    cmp r4, #1
    ite eq
    moveq r1, #8
    movne r1, #77
    CHK r0, r1, 4
.endm
.macro Q4 k
    SETUP
    movs r6, #8
    cmp r4, #1
    ite eq
    moveq r0, #8
    movne r0, #0
    str r0, [r2, #16]
    movs r0, #55
    cmp r4, #1
    ite eq
    ldreq r0, [r2, #16]
    strne r6, [r2, #24]
    ldr r5, [r2, #16]
    cmp r4, #1
    ite eq
    moveq r1, #8
    movne r1, #8
    CHK r5, r1, 8
    cmp r4, #1
    ite eq
    moveq r1, #8
    movne r1, #55
    CHK r0, r1, 8
.endm
.macro Q5 k
    SETUP
    movs r6, #8
    movs r0, #8
    str r0, [r2, #16]
    movs r0, #66
    cmp r4, #1
    itt ne
    strne r6, [r2, #32]
    ldrne r0, [r2, #16]
    ldr r5, [r2, #16]
    cmp r4, #1
    ite ne
    movne r1, #0
    moveq r1, #8
    CHK r5, r1, 16
    cmp r4, #1
    ite ne
    movne r1, #0
    moveq r1, #66
    CHK r0, r1, 16
.endm
.macro Q6 k
    SETUP
    movs r6, #8
    lsls r3, r4, #31
    ite mi
    movmi r0, #0
    movpl r0, #8
    str r0, [r2, #16]
    lsls r3, r4, #31
    ite mi
    strmi r6, [r2, #24]
    strpl r6, [r2, #32]
    ldr r5, [r2, #16]
    lsls r3, r4, #31
    ite mi
    movmi r1, #8
    movpl r1, #0
    CHK r5, r1, 32
    CHK r6, #8, 32
.endm
.thumb_func
start:
    mov r9, sp
    mov r10, sp
    movs r7, #0
    ldr r8, =%(iter)d
iter:
    and r4, r8, #1
""" % {"iter": ITER}
for k in range(1, 5):
    for m in ("Q1", "Q2", "Q3", "Q4", "Q5", "Q6"):
        S += "    %s %d\n" % (m, k * 10 + int(m[1:]))
S += """    mov r1, sp
    cmp r1, r10
    it ne
    orrne r7, r7, #512
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
open("ite.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "ite.o", "ite.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "ite.elf", "ite.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "ite.elf", "ite.bin"])
b = open("ite.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("ite.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

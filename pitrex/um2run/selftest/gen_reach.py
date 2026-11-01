# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the hot-patch reach of 1 Nov 2026 (docs: um2run.c, hot_patch): a site is now also patched when it is
#   P1  the only instruction of an IT block (`it eq ; streq`), patched from the `it` (the stub starts with the inverse conditional branch)
#   P2  followed by a B<cond>.n        P3  followed by B.n        P4  followed by CBZ / CBNZ      (relocated as branches in the stub)
#   P5  followed by a loop head (branch target) but preceded by a plain ALU halfword (`movs`), which is hoisted into the stub
#   P6  a 16-bit load followed by a 32-bit `str.w rt,[rn,#imm]` (two helper calls)
#   P7  followed by `pop {r6,pc}`      P8  followed by `bx lr`      P9  followed by `ldr/str rt,[sp,#imm]`
#   P10 the DWT cycle counter in the SDK's delay loop (`ldr r1,[r2,#4]` ... `subs ; cmp ; bhi`), and the IO_BANK0 pin loop (`it ne ; strne` in a loop)
# The echo device is SIO GPIO_OUT: a store to 0xD0000010 reads back from 0xD0000014.  Every site runs ITER times (R4 alternates 0 / 1 so both IT outcomes
# and both branch directions are taken); the guest checks the values, callee-saved registers, sp and the flags a site must preserve.
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
    ldr r2, =0xD0000010
    ldr r3, =0xD0000014
.endm
.macro CHK reg, val, bit
    cmp \\reg, \\val
    it ne
    orrne r7, r7, #\\bit
.endm
.macro P1 k
    SETUP
    movs r0, #0
    str r0, [r2, #0]
    movs r1, #\\k
    cmp r4, #1
    it eq
    streq r1, [r2, #0]
    ldr r5, [r3, #0]
    movs r6, #\\k
    cmp r4, #1
    ite eq
    moveq r0, r6
    movne r0, #0
    CHK r5, r0, 1
.endm
.macro P2 k
    SETUP
    movs r0, #0
    str r0, [r2, #0]
    movs r1, #\\k
    movs r6, #0
    cmp r4, #1
    str r1, [r2, #0]
    bne.n 1f
    adds r6, r6, #1
1:  ldr r5, [r3, #0]
    CHK r5, r1, 2
    CHK r6, r4, 4
.endm
.macro P3 k
    SETUP
    movs r1, #\\k
    movs r6, #0
    str r1, [r2, #0]
    b.n 1f
    adds r6, r6, #100
1:  ldr r5, [r3, #0]
    CHK r5, r1, 2
    CHK r6, #0, 4
.endm
.macro P4 k
    SETUP
    movs r1, #\\k
    movs r6, #0
    str r1, [r2, #0]
    cbz r4, 1f
    adds r6, r6, #1
1:  ldr r5, [r3, #0]
    CHK r5, r1, 2
    CHK r6, r4, 4
    movs r6, #0
    str r1, [r2, #0]
    cbnz r4, 2f
    adds r6, r6, #1
2:  eor r0, r4, #1
    CHK r6, r0, 4
.endm
.macro P5 k
    SETUP
    movs r0, #0
    str r0, [r2, #0]
    mov r6, r6
    movs r1, #\\k
    str r1, [r2, #0]
1:  ldr r5, [r3, #0]
    cmp r5, #0
    blt 1b
    CHK r5, r1, 8
.endm
.macro P6 k
    SETUP
    movs r0, #77
    str r0, [r2, #0]
    movs r1, #\\k
    ldr r0, [r3, #0]
    str.w r1, [r2, #0]
    ldr r5, [r3, #0]
    CHK r0, #77, 16
    CHK r5, r1, 16
.endm
.macro P7 k
    SETUP
    movs r1, #\\k
    movs r6, #0x5A
    bl 9f
    b 8f
9:  push {r6, lr}
    movs r6, #0
    str r1, [r2, #0]
    pop {r6, pc}
8:  ldr r5, [r3, #0]
    CHK r5, r1, 32
    CHK r6, #0x5A, 32
.endm
.macro P8 k
    SETUP
    movs r1, #\\k
    bl 9f
    b 8f
9:  str r1, [r2, #0]
    bx lr
8:  ldr r5, [r3, #0]
    CHK r5, r1, 32
.endm
.macro P9 k
    SETUP
    sub sp, sp, #8
    movs r1, #\\k
    str r1, [r2, #0]
    ldr r0, [r3, #0]
    str r0, [sp, #4]
    movs r0, #0
    ldr r5, [sp, #4]
    add sp, sp, #8
    CHK r5, r1, 64
.endm
.macro P10
    ldr r2, =0xE0001000
    ldr r1, [r2, #4]
2:  ldr r3, [r2, #4]
    subs r3, r3, r1
    ldr r9, =3000
    cmp r3, r9
    bcc 2b
    ldr r0, =0xD0000010
    ldr r1, [r2, #4]
    str.w r4, [r0, #0]
3:  ldr r3, [r2, #4]
    subs r3, r3, r1
    ldr r9, =2000
    cmp r9, r3
    bhi 3b
    ldr r5, [r0, #4]
    CHK r5, r4, 128
.endm
.macro P11
    movs r3, #0
    ldr r2, =0x40028004
    movs r6, #6
    ldr r5, =0x5A5A5A5
1:  lsr.w r0, r5, r3
    tst.w r0, #1
    add.w r3, r3, #1
    it ne
    strne r6, [r2, #0]
    cmp r3, #27
    add.w r2, r2, #8
    bne 1b
    movs r3, #0
    ldr r2, =0x40028004
    ldr r1, =0x5A5A5A5
2:  lsr.w r0, r1, r3
    ands r0, r0, #1
    ite ne
    movne r5, #6
    moveq r5, #0
    ldr r6, [r2, #0]
    cmp r6, r5
    it ne
    orrne r7, r7, #256
    adds r3, r3, #1
    cmp r3, #27
    add.w r2, r2, #8
    bne 2b
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
    for m in ("P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9"):
        S += "    %s %d\n" % (m, k * 10 + int(m[1:]))
S += """    P10
    P11
    mov r1, sp
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
open("rc.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "rc.o", "rc.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "rc.elf", "rc.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "rc.elf", "rc.bin"])
b = open("rc.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("reach.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

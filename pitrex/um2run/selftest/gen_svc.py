# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the native SVC entry (1 Nov 2026) and its pc-relative second instruction (2 Nov 2026).
# The UVMC2 runtime's system calls are veneers `svc #n ; bx lr`, and some are `svc #n ; ldr rX,[pc,#imm] ; ...` (Enduro's #7 and #13): the first form became a stub in
# a96489c, the pc-relative form could not be relocated and stayed on the trap path (two signal round trips each).  The guest's SVCall handler reads the call number at the
# stacked pc - 2, returns n * 0x01010101 in the stacked r0 and counts the numbers.  Four veneers (plain, ldr + strb, two ldr + shifts + two strb, ldr into r0) run ITER times;
# the return values, the loaded constants, the stored bytes, the flags (Z set across a plain veneer) and r4-r6 must be right.
# Result word 0xC0DE0000 | error bits at 0x40001000 (0 = all good).  Argument: ITER (default 60).
import struct, subprocess, sys
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 60
S = """
.syntax unified
.cpu cortex-m33
.thumb
.text
vt: .word 0x20082000, start+1
    .rept 9
    .word dummy+1
    .endr
    .word svch+1
    .rept 4
    .word dummy+1
    .endr
dummy: b .
.thumb_func
start:
    movs r7, #0
    movs r4, #%d
    ldr r5, =0xA5A55A5A
    ldr r6, =0x12345678
    ldr r8, =0x20070100
    movs r0, #0
    str r0, [r8]
iter:
    movs r0, #0
    bl v1
    ldr r1, =0x05050505
    cmp r0, r1
    it ne
    orrne r7, r7, #1
    movs r0, #0               @ Z set
    bl v1                     @ the exception return must give the flags back
    it ne
    orrne r7, r7, #2
    ldr r3, =0x77777777
    bl v2
    ldr r1, =0x20070110
    cmp r3, r1
    it ne
    orrne r7, r7, #4
    ldrb r2, [r1]
    cmp r2, #7
    it ne
    orrne r7, r7, #8
    bl v3
    ldr r1, =0x20070114
    cmp r2, r1
    it ne
    orrne r7, r7, #0x10
    ldr r1, =0x20070118
    cmp r3, r1
    it ne
    orrne r7, r7, #0x20
    ldr r1, =0x20070114
    ldrb r2, [r1]
    cmp r2, #13
    it ne
    orrne r7, r7, #0x40
    ldr r1, =0x20070118
    ldrb r2, [r1]
    cmp r2, #13
    it ne
    orrne r7, r7, #0x80
    movs r0, #0
    bl v4
    ldr r1, =0xCAFE0001
    cmp r0, r1
    it ne
    orrne r7, r7, #0x100
    ldr r1, =0xA5A55A5A
    cmp r5, r1
    it ne
    orrne r7, r7, #0x200
    ldr r1, =0x12345678
    cmp r6, r1
    it ne
    orrne r7, r7, #0x400
    subs r4, r4, #1
    bne.w iter
    ldr r1, [r8]               @ the handler's sum of call numbers: (5 + 5 + 7 + 13 + 21) per iteration
    ldr r2, =%d
    cmp r1, r2
    it ne
    orrne r7, r7, #0x800
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    ldr r1, =0x40001000
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.ltorg
.thumb_func
svch:
    ldr r1, [sp, #24]
    bic r1, r1, #1
    ldrb r0, [r1, #-2]
    ldr r2, =0x01010101
    mul r2, r2, r0
    str r2, [sp, #0]
    ldr r3, =0x20070100
    ldr r2, [r3]
    adds r2, r2, r0
    str r2, [r3]
    bx lr
.ltorg
.thumb_func
v1:
    svc #5
    bx lr
.thumb_func
v2:
    svc #7
    ldr r3, =0x20070110
    strb r0, [r3]
    bx lr
.ltorg
.thumb_func
v3:
    svc #13
    ldr r2, =0x20070114
    ldr r3, =0x20070118
    lsrs r1, r0, #24
    lsrs r0, r0, #16
    strb r1, [r2]
    strb r0, [r3]
    bx lr
.ltorg
.thumb_func
v4:
    svc #21
    ldr r0, =0xCAFE0001
    bx lr
.ltorg
""" % (ITER, ITER * (5 + 5 + 7 + 13 + 21))
open("svc.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "svc.o", "svc.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "svc.elf", "svc.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "svc.elf", "svc.bin"])
b = open("svc.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("svc.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

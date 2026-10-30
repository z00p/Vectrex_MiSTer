# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for review finding 2 (30 Oct 2026): the branch-target guard of the hot patcher must see far and 32-bit branches.
# f() is `ldr r1,[r0,#4]` (SIO GPIO_IN) followed by `adds r2,#1` at Y = site+2.  After 10 calls (patched at the 6th), code 400+ bytes away does `b.w Y` (and later
# `beq.w Y`): a patch that took Y along leaves Y in the middle of the b.w.  Result 0x40001000 = 0xC0DE0000 | r2 after the two far entries (expect 2).
import struct, subprocess
S = """
.syntax unified
.cpu cortex-m33
.thumb
.text
vt: .word 0x20082000, start+1
    .rept 14
    .word dummy+1
    .endr
dummy: b .
.thumb_func
start:
    ldr r0, =0xD0000000
    movs r2, #0
    movs r4, #10
loop:
    bl f
    subs r4, r4, #1
    bne loop
    movs r2, #0
    ldr lr, =back1+1
    b.w far1
back1:
    ldr lr, =back2+1
    b.w far2
back2:
    ldr r0, =0xC0DE0000
    orrs r0, r0, r2
    ldr r1, =0x40001000
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.ltorg
.thumb_func
f:
    ldr r1, [r0, #4]
Y:  adds r2, r2, #1
    bx lr
    .space 400
far1:
    ldr r0, =0xD0000000
    b.w Y
    .space 4000
far2:
    ldr r0, =0xD0000000
    cmp r0, r0
    beq.w Y
    b .
.ltorg
"""
open("g.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "g.o", "g.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "g.elf", "g.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "g.elf", "g.bin"])
b = open("g.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("far.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

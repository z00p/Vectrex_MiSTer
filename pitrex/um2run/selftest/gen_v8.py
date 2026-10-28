# SPDX-License-Identifier: GPL-2.0-or-later
import struct, subprocess
S = r'''
.syntax unified
.arch armv7-a
.thumb
.text
vt: .word 0x20082000, start+1
    .rept 14
    .word dummy+1
    .endr
dummy: b .
.thumb_func
start:
    ldr r1, =0x20081800
    movs r2, #0x5A
    movs r7, #0
    movs r6, #0
loop:
    .inst.w 0xE8C12FAF      @ stl r2,[r1]
    movs r0, #0
    .inst.w 0xE8D10FAF      @ lda r0,[r1]
    cmp r0, r2
    it ne
    orrne r7, r7, #1
retry:
    .inst.w 0xE8D10FEF      @ ldaex r0,[r1]
    adds r0, r0, #1
    .inst.w 0xE8C10FE3      @ stlex r3,r0,[r1]
    cmp r3, #0
    bne retry
    ldr r0, [r1]
    adds r4, r2, #1
    cmp r0, r4
    it ne
    orrne r7, r7, #4
    movs r0, #0
    cmp r6, #3
    it eq
    .inst.w 0xE8D10FAF      @ lda r0,[r1] inside an IT block: stays on the in-place path
    cmp r6, #3
    bne skip
    cmp r0, r4
    it ne
    orrne r7, r7, #8
skip:
    adds r6, r6, #1
    cmp r6, #20
    blt loop
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    ldr r1, =0x40001000
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.ltorg
'''
open("v8.s","w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc","-c","-o","v8.o","v8.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld","-Ttext=0x20000000","-o","v8.elf","v8.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy","-O","binary","v8.elf","v8.bin"])
b = open("v8.bin","rb").read(); b += b"\0" * (-len(b) % 4)
open("v8.um2","wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b)//4) + b)
print("words", len(b)//4)

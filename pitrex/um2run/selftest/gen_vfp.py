# SPDX-License-Identifier: GPL-2.0-or-later
# generate a tiny Thumb-2 test guest: VFP state must survive hot-patched peripheral accesses
import struct, subprocess, sys
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
    ldr r4, =0xD0000000
    ldr r5, =0x40001000
    movs r6, #0          @ iteration
    movs r7, #0          @ error bits
    ldr r8, =0x3f800000
loop:
    mov r0, r8
    ldr r1, =0x00010000
'''
for i in range(16):
    S += "    vmov s%d, r0\n    add r0, r0, r1\n" % i
S += '''    ldr r0, =0x60000011
    vmsr fpscr, r0
    ldr r0, [r4, #4]        @ SIO GPIO_IN: faults, hot-patched after a few traps
    lsls r0, r0, #31
    str r1, [r4, #0x10]     @ SIO GPIO_OUT: a store site
    movs r1, #0
    vmrs r0, fpscr
    ldr r1, =0xF0000000
    bics r0, r0, r1
    ldr r1, =0x00000011
    cmp r0, r1
    it ne
    orrne r7, r7, #2
    vmrs r0, fpscr
    lsrs r0, r0, #28
    cmp r0, #6
    it ne
    orrne r7, r7, #4
    mov r2, r8
    ldr r1, =0x00010000
'''
for i in range(16):
    S += "    vmov r0, s%d\n    cmp r0, r2\n    it ne\n    orrne r7, r7, #1\n    add r2, r2, r1\n" % i
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
open("t.s","w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc","-c","-o","t.o","t.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld","-Ttext=0x20000000","-o","t.elf","t.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy","-O","binary","t.elf","t.bin"])
b = open("t.bin","rb").read()
b += b"\0" * (-len(b) % 4)
words = len(b)//4
open("vfptest.um2","wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, words) + b)
print("words", words)

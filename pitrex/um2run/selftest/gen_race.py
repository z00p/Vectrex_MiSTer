# SPDX-License-Identifier: GPL-2.0-or-later
# two-core patch race test guest: both cores run the same 48 SIO CPUID load sites (= distinct hot-patch candidates) in lock step
import struct, subprocess
N = 48; ITER = 80
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
    movs r0, #0
    str r0, [r4, #0x54]
    str r0, [r4, #0x54]
    movs r0, #1
    str r0, [r4, #0x54]
    ldr r0, =0x20000000
    str r0, [r4, #0x54]
    ldr r0, =0x20081000
    str r0, [r4, #0x54]
    ldr r0, =core1entry+1
    str r0, [r4, #0x54]
    movs r6, #0
    b worker
.thumb_func
core1entry:
    ldr r4, =0xD0000000
    movs r6, #1
worker:
    movs r7, #0
    ldr r8, =%d
iter:
''' % ITER
for i in range(N):
    S += "    ldr r0, [r4, #0]\n    cmp r0, r6\n    it ne\n    orrne r7, r7, #1\n"
S += '''    subs r8, r8, #1
    bne.w iter
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    orr r0, r0, r6, lsl #8
    ldr r1, =0x40001000
    add r1, r1, r6, lsl #2
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.ltorg
'''
open("r.s","w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc","-c","-o","r.o","r.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld","-Ttext=0x20000000","-o","r.elf","r.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy","-O","binary","r.elf","r.bin"])
b = open("r.bin","rb").read(); b += b"\0" * (-len(b) % 4)
open("race.um2","wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b)//4) + b)
print("words", len(b)//4)

# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the PRIMASK stubs (28 Oct 2026): `mrs rd, PRIMASK ; cpsid i` ... `msr PRIMASK, rd` pairs with different registers (incl. r12 and lr), 60 iterations.
# After the first traps each pair becomes a stub; the guest checks: rd = 0 (old mask), a plain MRS inside the pair reads 1, one after the MSR reads 0, r6 / r11 / sp unchanged.
# Result word 0xC0DE0000 | flags at 0x40001000 (0 = ok).
import struct, subprocess, sys
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 60
REGS = ["r0", "r2", "r3", "r4", "r5", "r12", "lr"]
T = """    mrs {R}, PRIMASK
    cpsid i
    mrs r1, PRIMASK
    cmp r1, #1
    it ne
    orrne r7, r7, #1
    cmp {R}, #0
    it ne
    orrne r7, r7, #2
    msr PRIMASK, {R}
    mrs r1, PRIMASK
    cmp r1, #0
    it ne
    orrne r7, r7, #4
    cmp r6, #0x55
    it ne
    orrne r7, r7, #8
    cmp r11, r10
    it ne
    orrne r7, r7, #16
"""
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
    mov r9, sp
    movs r7, #0
    movs r6, #0x55
    ldr r10, =0x1234ABCD
    mov r11, r10
    ldr r8, =%d
iter:
""" % ITER
for R in REGS: S += T.format(R=R)
S += """    mov r1, sp
    cmp r1, r9
    it ne
    orrne r7, r7, #32
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
open("pm.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-march=armv8-m.main", "-mthumb", "-c", "-o", "pm.o", "pm.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "pm.elf", "pm.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "pm.elf", "pm.bin"])
b = open("pm.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("prim.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the "constant load + site" hot-patch variant (28 Oct 2026): the site's next halfword is a branch target, so the site cannot be merged with it;
# um2run then patches `ldr rX,[pc,#imm]` (constant pool) + the site from site - 2.  8 load sites (SIO CPUID) and 8 store sites (SIO word), 60 iterations; the guest checks
# the constant register, the loaded value, the stack pointer and a callee-saved register block after every site.  Result word 0xC0DE0000 | flags at 0x40001000 (flags 0 = ok).
import struct, subprocess
import sys
N = 8; ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 60
T = """    ldr r2, lit_sio
    ldr r0, [r2, #0]
w{i}:
    adds r3, r3, #1
    cmp r3, #2
    bne w{i}
    movs r3, #0
    cmp r0, #0
    it ne
    orrne r7, r7, #1
    it ne
    strne r0, [r11]
    ldr r1, lit_sio
    cmp r1, r2
    it ne
    orrne r7, r7, #4
    ldr r1, [sp, #0]
    cmp r1, r10
    it ne
    orrne r7, r7, #8
"""
X = """    ldr r2, lit_gpo
    movs r0, #{v}
    str r0, [r2, #0]
x{i}:
    adds r3, r3, #1
    cmp r3, #2
    bne x{i}
    movs r3, #0
    ldr r1, lit_gpo
    cmp r1, r2
    it ne
    orrne r7, r7, #4
"""
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
.thumb_func
start:
    mov r9, sp
    movs r7, #0
    movs r3, #0
    ldr r11, =0x40001010
    ldr r10, =0x1234ABCD
    push {r10}
    ldr r8, =%d
iter:
""" % ITER
for i in range(N): S += T.format(i=i)
for i in range(N): S += X.format(i=i, v=i + 1)
S += """    mov r1, sp
    mov r0, r9
    subs r0, r0, #4
    cmp r1, r0
    it ne
    orrne r7, r7, #2
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
.balign 4
lit_sio: .word 0xD0000000
lit_gpo: .word 0xD0000010
.ltorg
"""
open("p.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "p.o", "p.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "p.elf", "p.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "p.elf", "p.bin"])
b = open("p.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("pre.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

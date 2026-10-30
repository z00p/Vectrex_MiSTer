# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for review finding 1 (30 Oct 2026): a hot patch must not be written over a return address saved in an exception frame.
# Thread mode calls setalarm() 40 times with a target in the PAST (`str r1,[r0,#16]` to ALARM0, then `adds r2,#1`): the alarm fires inline, so the ISR is entered at the end of
# that very trap with the return address site+2.  The ISR calls the same setalarm() with a FUTURE target, so the site's 6th access (and later retry points) can come from
# inside the ISR while the frame holds site+2.  Without the frame check the patch lands under it and thread mode resumes in the middle of the b.w (one `adds` lost).
# Result 0x40001000 = 0xC0DE0000 | number of calls counted in thread mode (expect 0x28).
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
    .word isr0+1
dummy: b .
.thumb_func
start:
    ldr r0, =0x400b0040
    movs r1, #1
    str r1, [r0]                 @ TIMER0 INTE bit 0
    ldr r0, =0xe000e100
    str r1, [r0]                 @ NVIC ISER0 bit 0
    movs r2, #0
    movs r4, #40
loop:
    ldr r0, =0x400b000c
    ldr r1, [r0]
    subw r1, r1, #1000           @ past
    ldr r0, =0x400b0000
    bl setalarm
    subs r4, r4, #1
    bne loop
    ldr r0, =0xC0DE0000
    orrs r0, r0, r2
    ldr r1, =0x40001000
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.thumb_func
setalarm:
    str r1, [r0, #16]
    adds r2, r2, #1
    bx lr
.thumb_func
isr0:
    push {r4, lr}
    ldr r0, =0x400b003c
    movs r1, #1
    str r1, [r0]                 @ clear INTR bit 0
    ldr r0, =0x400b000c
    ldr r1, [r0]
    ldr r3, =100000000
    add r1, r1, r3               @ future (100 s)
    ldr r0, =0x400b0000
    bl setalarm
    pop {r4, pc}
.ltorg
"""
open("f.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "f.o", "f.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "f.elf", "f.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "f.elf", "f.bin"])
b = open("f.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("frame.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

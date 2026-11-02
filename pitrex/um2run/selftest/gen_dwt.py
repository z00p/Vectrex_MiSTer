# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for DWT_CYCCNT (2 Nov 2026).  The counter steps by edges * clk_sys / 1.5 MHz while the thread polls the E clock (SIO GPIO_IN), so the step between two
# reads with exactly 50 falling edges in between is deterministic: 400 at 12 MHz (clk_sys from the XOSC), 5000 at 150 MHz (PLL_SYS 12 MHz * 125 / (5 * 2)).  The guest alternates
# the two clock set-ups 24 times (writes to CLOCKS / PLL_SYS) and reads the counter before and after 100 GPIO_IN reads each round; the reads become stubs after a few rounds, so
# a stub path that caches the clock rate must notice every change of the clock registers.  A free-running second part reads the counter 2000 times in a row: it must never go backwards.
# Result word 0xC0DE0000 | error bits (bit 0: a step was wrong, bit 1: the counter went backwards, bit 2: a round saw no step) at 0x40001000.  Argument: ROUNDS (default 24).
import struct, subprocess, sys
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 24
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
    movs r7, #0
    ldr r2, =0xE0001000
    ldr r5, =0xD0000000
    ldr r8, =%d
    movs r9, #0
round:
    tst r9, #1
    bne fast
    ldr r0, =0x4001003c       @ CLK_SYS_CTRL: source = clk_ref (XOSC, 12 MHz)
    movs r1, #0
    str r1, [r0]
    ldr r11, =400
    b meas
fast:
    ldr r0, =0x40050000       @ PLL_SYS CS: refdiv 1
    movs r1, #1
    str r1, [r0]
    ldr r0, =0x40050008       @ FBDIV 125
    movs r1, #125
    str r1, [r0]
    ldr r0, =0x4005000c       @ PRIM: postdiv1 5, postdiv2 2
    ldr r1, =0x00052000
    str r1, [r0]
    ldr r0, =0x4001003c       @ CLK_SYS_CTRL: SRC = aux (bit 0), AUXSRC 0 = PLL_SYS
    movs r1, #1
    str r1, [r0]
    ldr r11, =5000
meas:
    ldr r3, [r2, #4]          @ d0
    movs r4, #100
poll:
    ldr r6, [r5, #4]          @ GPIO_IN: one E-clock half period per read
    subs r4, r4, #1
    bne poll
    ldr r6, [r2, #4]          @ d1
    subs r6, r6, r3
    cmp r6, r11
    it ne
    orrne r7, r7, #1
    cmp r6, #0
    it eq
    orreq r7, r7, #4
    adds r9, r9, #1
    cmp r9, r8
    blo round
    ldr r4, =2000
    ldr r3, [r2, #4]
mono:
    ldr r6, [r2, #4]
    subs r1, r6, r3           @ step (unsigned) must be small
    cmp r1, #0x40000000
    it hi
    orrhi r7, r7, #2
    mov r3, r6
    subs r4, r4, #1
    bne mono
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    ldr r1, =0x40001000
    str r0, [r1]
    str r0, [r1]
    str r0, [r1]
spin:
    b spin
.ltorg
""" % ROUNDS
open("dwt.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "dwt.o", "dwt.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "dwt.elf", "dwt.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "dwt.elf", "dwt.bin"])
b = open("dwt.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("dwt.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

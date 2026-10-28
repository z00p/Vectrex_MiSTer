# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the cart-audio model (28 Oct 2026): sets up a PIO2 I2S state machine + an endless 8 KB ring DMA exactly like the SDK audio module of the
# UVMC2 images (Star Wars, Pac-Man, Punch-Out, Veccy Road), then plays the ring writer: it polls DMA READ_ADDR (a plain load from the read-only DMA page), keeps
# the ring ~1600 frames ahead and writes a 1 kHz square wave (period 32 frames).  After 64000 frames it stores
#   0x40001000 = 0xC0DE0000 | flags (bit 0: the reader overtook the writer = the game's "fill > 0x700" resync, bit 1: DMA BUSY not set, bit 2: INTS0 set unexpectedly)
#   0x40001004 = 0xE1000000 | elapsed milliseconds (TIMELR) for the 62400 frames after the first 1600  -> expect ~1950 ms at 32 kHz (clk_sys 300 MHz from the shadow PLL regs, clkdiv 146.484375)
# The harness (selftest.sh) then checks the .wav (UM2_WAV): 1 kHz tone, ~2 s of it.
import struct, subprocess, sys
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
    @ clocks as the shadow sees them: clk_sys = clk_sys_aux (PLL_SYS) = 12 MHz / 1 * 125 / (5 * 1) = 300 MHz
    ldr r0, =0x4001003c
    movs r1, #1
    str r1, [r0]
    ldr r0, =0x40050000
    movs r1, #1
    str r1, [r0]
    movs r1, #125
    str r1, [r0, #8]
    ldr r1, =0x00051000
    str r1, [r0, #12]
    @ PIO2 program (the SDK I2S module: 8 instructions at 24..31), state machine 0
    ldr r0, =0x50400048 + 4*24
    adr r1, prog
    movs r2, #8
1:  ldrh r3, [r1], #2
    str r3, [r0], #4
    subs r2, r2, #1
    bne 1b
    ldr r0, =0x504000c8
    ldr r1, =(146<<16)|(124<<8)
    str r1, [r0]
    ldr r1, =0x0001fc00
    str r1, [r0, #4]
    ldr r1, =0x40060000
    str r1, [r0, #8]
    ldr r1, =0x4010701b
    str r1, [r0, #0x14]
    @ ring (8 KB at 0x20010000) zeroed by the loader (BSS); DMA channel 15: read ring, write PIO2 TXF0, endless, ring 2^13, treq 16
    ldr r4, =0x20010000
    ldr r0, =0x500003c0
    str r4, [r0]
    ldr r1, =0x50400010
    str r1, [r0, #4]
    mvn r1, #0
    str r1, [r0, #8]
    ldr r1, =0x0021ed19
    str r1, [r0, #0xc]
    @ enable SM0 and read the clock
    ldr r1, =0x50400000
    movs r2, #1
    str r2, [r1]
    ldr r1, =0x400b000c
    ldr r9, [r1]
    movs r5, #0              @ write index (frames)
    movs r6, #0              @ frames written
    ldr r8, =64000
    ldr r10, =0x1f401f40     @ +8000 in both halves
    ldr r11, =0xe0c0e0c0     @ -8000
loop:
    ldr r0, =0x500003c0
    ldr r0, [r0]             @ READ_ADDR (read-only page, no trap)
    subs r0, r0, r4
    lsrs r0, r0, #2
    ubfx r0, r0, #0, #11
    subs r1, r5, r0
    ubfx r1, r1, #0, #11
    cmp r6, #0
    beq 3f
    cmp r1, #0x700
    it hi
    orrhi r7, r7, #1
3:  movw r2, #0x640
    subs r2, r2, r1
    ble loop
w:  tst r6, #16
    ite eq
    moveq r3, r10
    movne r3, r11
    str r3, [r4, r5, lsl #2]
    adds r5, r5, #1
    ubfx r5, r5, #0, #11
    adds r6, r6, #1
    subs r2, r2, #1
    bne w
    cmp r6, r8
    blt loop
    ldr r1, =0x400b000c
    ldr r1, [r1]
    subs r1, r1, r9
    movw r2, #1000
    udiv r1, r1, r2
    ldr r0, =0xE1000000
    orrs r1, r1, r0
    @ DMA BUSY must read 1 for the running transfer; INTS0 must be 0
    ldr r0, =0x500003c0
    ldr r0, [r0, #0xc]
    tst r0, #(1<<26)
    it eq
    orreq r7, r7, #2
    ldr r0, =0x5000040c
    ldr r0, [r0]
    cmp r0, #0
    it ne
    orrne r7, r7, #4
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    ldr r2, =0x40001000
    str r0, [r2]
    str r1, [r2, #4]
    str r0, [r2]
    str r1, [r2, #4]
spin:
    b spin
.align 2
prog:
    .hword 0x6001, 0x1058, 0x6001, 0xf02e, 0x6801, 0x185c, 0x6801, 0xf82e
.ltorg
"""
open("aud.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-march=armv8-m.main", "-mthumb", "-c", "-o", "aud.o", "aud.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "aud.elf", "aud.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "aud.elf", "aud.bin"])
b = open("aud.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("aud.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

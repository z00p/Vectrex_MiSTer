# SPDX-License-Identifier: GPL-2.0-or-later
# synthetic guest for the TIMER0 alarm path (28 Oct 2026): the interrupt machinery BattleZone lives on (SDK alarm pool: a repeating 166 us alarm re-armed from its own ISR, the target
# often already in the past).  Phases, all with TIMER0 alarm 0 -> NVIC IRQ 0:
#   A  200 x: thread mode stores an ALARM0 target that is already in the PAST and waits for the ISR (the model fires it itself: no irq-thread round trip; the SIGURG it sends to the
#      calling thread is refused while the store runs in helper code, a retry must deliver it)           flag bit 0 = an ISR never came
#   B  100 x: a target 300 us in the FUTURE (the irq thread has to wake up for it)                         flag bit 1 = an ISR never came
#   C  the repeating alarm: the ISR re-arms prev + 166 us (starts 100 us ahead, falls behind when the emulation is slower than 166 us per ISR) until 3000 ISRs have run
#                                                                                                          flag bit 2 = the chain stopped
# At the end it stores
#   0x40001000 = 0xC0DE0000 | flags,  0x40001004 = 0xE1000000 | elapsed ms of phase C (>= ~500 when ISRs are never faster than the 166 us period of a caught-up chain... or less when it catches up),
#   0x40001008 = 0xC0000000 | ISR count (expect 300 + 3000 = 3300 or a little more)
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
    movs r7, #0
    ldr r5, =0x20010000          @ [r5] ISR count, [r5,#4] chain mode, [r5,#8] chain target, [r5,#12] chain limit
    ldr r0, =0x400b0040
    movs r1, #1
    str r1, [r0]                 @ TIMER0 INTE bit 0
    ldr r0, =0xe000e100
    str r1, [r0]                 @ NVIC ISER0 bit 0
    @ ---- phase A: past targets from thread mode
    movs r8, #200
pa: ldr r6, [r5]
    ldr r0, =0x400b000c
    ldr r1, [r0]
    subw r1, r1, #500
    ldr r0, =0x400b0010
    str r1, [r0]
    ldr r3, =3000000
wa: ldr r2, [r5]
    cmp r2, r6
    bne da
    subs r3, r3, #1
    bne wa
    orr r7, r7, #1
da: subs r8, r8, #1
    bne pa
    @ ---- phase B: future targets from thread mode
    movs r8, #100
pb: ldr r6, [r5]
    ldr r0, =0x400b000c
    ldr r1, [r0]
    addw r1, r1, #300
    ldr r0, =0x400b0010
    str r1, [r0]
    ldr r3, =3000000
wb: ldr r2, [r5]
    cmp r2, r6
    bne db
    subs r3, r3, #1
    bne wb
    orr r7, r7, #2
db: subs r8, r8, #1
    bne pb
    @ ---- phase C: the repeating alarm re-armed from the ISR
    ldr r0, =0x400b000c
    ldr r9, [r0]                 @ start time
    ldr r4, [r5]
    addw r4, r4, #3000
    str r4, [r5, #12]            @ limit
    add r1, r9, #100
    str r1, [r5, #8]
    movs r2, #1
    str r2, [r5, #4]             @ mode 1 = chain
    ldr r0, =0x400b0010
    str r1, [r0]
    ldr r3, =1500000000
wc: ldr r2, [r5]
    ldr r1, [r5, #12]
    cmp r2, r1
    bhs dc
    subs r3, r3, #1
    bne wc
    orr r7, r7, #4
dc: movs r2, #0
    str r2, [r5, #4]
    ldr r0, =0x400b000c
    ldr r1, [r0]
    subs r1, r1, r9
    movw r2, #1000
    udiv r1, r1, r2
    ldr r0, =0xE1000000
    orrs r1, r1, r0
    ldr r0, =0xC0DE0000
    orrs r0, r0, r7
    ldr r2, [r5]
    ldr r3, =0xC0000000
    orrs r2, r2, r3
    ldr r4, =0x40001000
    str r0, [r4]
    str r1, [r4, #4]
    str r2, [r4, #8]
    str r0, [r4]
    str r1, [r4, #4]
    str r2, [r4, #8]
spin:
    b spin
.thumb_func
isr0:
    ldr r0, =0x400b003c          @ INTR: write 1 to clear
    movs r1, #1
    str r1, [r0]
    ldr r0, =0x20010000
    ldr r1, [r0]
    adds r1, r1, #1
    str r1, [r0]
    ldr r2, [r0, #4]
    cmp r2, #1
    bne 9f
    ldr r2, [r0, #12]
    cmp r1, r2
    bhs 9f
    ldr r2, [r0, #8]
    adds r2, r2, #166
    str r2, [r0, #8]
    ldr r3, =0x400b0010
    str r2, [r3]                 @ ALARM0 = previous target + 166 us (may be in the past)
9:  bx lr
.ltorg
"""
open("alarm.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-march=armv8-m.main", "-mthumb", "-c", "-o", "alarm.o", "alarm.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-e", "0", "-Ttext=0x20000000", "-o", "alarm.elf", "alarm.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "alarm.elf", "alarm.bin"])
b = open("alarm.bin", "rb").read(); b += b"\0" * (-len(b) % 4)
open("alarm.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

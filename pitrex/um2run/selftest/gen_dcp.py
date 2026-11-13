# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic guest for the RP2350 DCP canned-sequence emulation. Both cores
# exercise arithmetic, classification, comparisons and conversions.
import math
import struct
import subprocess

ERR_MASK = 1


def f64(bits):
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def b64(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def b32(value):
    try:
        return struct.unpack("<I", struct.pack("<f", value))[0]
    except OverflowError:
        return 0xFF800000 if math.copysign(1.0, value) < 0 else 0x7F800000


def ieee_div(a, b):
    if math.isnan(a) or math.isnan(b):
        return a / b if b else a
    if math.isinf(a) and math.isinf(b):
        return float("nan")
    if b == 0:
        if a == 0:
            return float("nan")
        return math.copysign(float("inf"), a * math.copysign(1.0, b))
    return a / b


def ieee_sqrt(a):
    if a < 0:
        return float("nan")
    return math.sqrt(a)


def nearest_even(value):
    return int(round(value))


def to_int(value, unsigned, rounded=False):
    if math.isnan(value) or math.isinf(value):
        if unsigned:
            return 0 if math.copysign(1.0, value) < 0 else 0xFFFFFFFF
        return 0x80000000 if math.copysign(1.0, value) < 0 else 0x7FFFFFFF
    value = nearest_even(value) if rounded else math.trunc(value)
    if unsigned:
        return min(0xFFFFFFFF, max(0, value))
    return min(0x7FFFFFFF, max(-0x80000000, value)) & 0xFFFFFFFF


def nzcv_compare(a, b):
    if math.isnan(a) or math.isnan(b):
        return 0x30000000
    if a == b:
        return 0x60000000
    return 0x80000000 if a < b else 0x20000000


def nzcv_classify(value):
    bits = b64(value)
    flags = 0x80000000 if bits >> 63 else 0
    if math.isnan(value):
        return flags | 0x30000000
    if math.isinf(value):
        return flags | 0x20000000
    if value == 0:
        return flags | 0x40000000
    return flags


def inst(h1, h2):
    return "    .inst.w 0x%04x%04x\n" % (h1, h2)


def mcrr(op, rt, rt2, cr=0):
    return inst(0xEC40 | rt, (rt2 << 12) | 0x0400 | (op << 4) | cr)


def mrrc(op, rt, rt2, cr=0):
    return inst(0xEC50 | rt, (rt2 << 12) | 0x0400 | (op << 4) | cr)


def mrc(key, rd=0):
    return inst(0xEE10, (rd << 12) | 0x0400 | key)


def mrc2(key, rd=0):
    return inst(0xFE10, (rd << 12) | 0x0400 | key)


def cdp(op, key):
    return inst(0xEE00 | (op << 4), 0x0400 | key)


def load32(reg, value):
    return "    ldr r%d, =0x%08x\n" % (reg, value & 0xFFFFFFFF)


def load64(reglo, reghi, value):
    return load32(reglo, value) + load32(reghi, value >> 32)


def check32(value):
    return (load32(4, value) +
            "    cmp r0, r4\n"
            "    it ne\n"
            "    orrne r7, r7, #%d\n" % ERR_MASK)


def check64(value):
    return (load32(4, value) + load32(5, value >> 32) +
            "    cmp r0, r4\n"
            "    it ne\n"
            "    orrne r7, r7, #%d\n" % ERR_MASK +
            "    cmp r1, r5\n"
            "    it ne\n"
            "    orrne r7, r7, #%d\n" % ERR_MASK)


def pooled(code):
    return code + "    b 1f\n.ltorg\n1:\n"


F64_PAIRS = [
    (b64(1.5), b64(2.25)),
    (0x8000000000000000, 0),
    (0x7FF0000000000000, 0xFFF0000000000000),
    (0x7FF8000000000042, b64(2.0)),
    (b64(2.0), 0x7FF8000000000042),
    (0x7FF0000000000042, b64(3.0)),
    (b64(3.0), 0x7FF0000000000042),
    (0x0010000000000000, 0x0010000000000000),
    (0x7FEFFFFFFFFFFFFF, b64(2.0)),
    (1, b64(2.0)),
    (0x000FFFFFFFFFFFFF, 1),
    (b64(1.0), 0x3CA0000000000000),  # halfway between adjacent doubles
    (b64(1.0), 0),
    (b64(0.0), b64(0.0)),
    (b64(0.0), 0x7FF0000000000000),
]

F32_PAIRS = [
    (0x3FC00000, 0x40100000),
    (0x80000000, 0),
    (0x7F800000, 0xFF800000),
    (0x7FC00042, b32(2.0)),
    (b32(2.0), 0x7FC00042),
    (0x7F800042, b32(3.0)),
    (b32(3.0), 0x7F800042),
    (0x00800000, 0x00800000),
    (0x7F7FFFFF, b32(2.0)),
    (1, b32(2.0)),
    (0x007FFFFF, 1),
    (b32(1.0), 0x33800000),  # halfway between adjacent floats
    (b32(1.0), 0),
    (0, 0),
    (0, 0x7F800000),
]

D64_VALUES = [
    b64(1.25), b64(-2.5), 0, 0x8000000000000000,
    0x7FF0000000000000, 0xFFF0000000000000,
    0x7FF8000000000042, 0x7FF0000000000042,
    0x0010000000000000, 0x7FEFFFFFFFFFFFFF,
    1, 0x000FFFFFFFFFFFFF,
    b64(2.5), b64(-3.5), b64(2147483647.5),
    b64(-2147483648.5), b64(4294967295.5), b64(-1.0),
]

F32_VALUES = [
    b32(1.25), b32(-2.5), 0, 0x80000000,
    0x7F800000, 0xFF800000, 0x7FC00042, 0x7F800042,
    0x00800000, 0x7F7FFFFF, 1, 0x007FFFFF,
    b32(2.5), b32(-3.5), b32(2147483647.5),
    b32(-2147483648.5), b32(4294967295.5), b32(-1.0),
]


def arithmetic(op, a, b):
    if op == "add":
        if math.isinf(a) and math.isinf(b) and math.copysign(1, a) != math.copysign(1, b):
            return float("nan")
        return a + b
    if op == "sub":
        if math.isinf(a) and math.isinf(b) and math.copysign(1, a) == math.copysign(1, b):
            return float("nan")
        return a - b
    if op == "mul":
        if (a == 0 and math.isinf(b)) or (b == 0 and math.isinf(a)):
            return float("nan")
        return a * b
    return ieee_div(a, b)


def dcp_pair(op, a_bits, b_bits, core):
    a, b = f64(a_bits), f64(b_bits)
    if core and a == 1.5 and b == 2.25:
        a, b = 11.5, 12.25
        a_bits, b_bits = b64(a), b64(b)
    s = load64(0, 1, a_bits) + load64(2, 3, b_bits)
    s += mcrr(1, 0, 1) + mcrr(1, 2, 3, 1)
    if op == "add":
        s += cdp(0, 1) + cdp(1, 1) + cdp(8, 0x20) + mrrc(1, 0, 1)
    elif op == "sub":
        s += cdp(0, 1) + cdp(1, 0x21) + cdp(8, 0x20) + mrrc(3, 0, 1)
    elif op == "mul":
        s += (mrrc(0, 4, 5, 4) + mrrc(0, 6, 7, 5) +
              "    umull r8, r9, r4, r6\n"
              "    movs r10, #0\n"
              "    umlal r9, r10, r4, r7\n"
              "    umlal r9, r10, r5, r6\n" +
              mcrr(2, 8, 9) + "    movs r8, #0\n" +
              "    umlal r10, r8, r5, r7\n" +
              mcrr(3, 10, 8) + cdp(8, 0x20) + mrrc(5, 0, 1))
    else:
        s += (mrrc(2, 4, 5, 1) + mcrr(4, 4, 5) +
              cdp(8, 0x20) + mrrc(7, 0, 1))
    value = arithmetic(op, a, b)
    return s + check64(b64(value))


def fcp_pair(op, a_bits, b_bits):
    a, b = f32(a_bits), f32(b_bits)
    s = load32(0, a_bits) + load32(1, b_bits) + mcrr(1, 0, 1, 2)
    if op == "add":
        s += cdp(0, 1) + cdp(1, 1) + cdp(8, 0x22) + mrc(0x12)
    elif op == "sub":
        s += cdp(0, 1) + cdp(1, 0x21) + cdp(8, 0x22) + mrc(0x32)
    elif op == "mul":
        s += (mrrc(1, 4, 5, 1) + "    umull r4, r5, r4, r5\n" +
              mcrr(10, 4, 5) + cdp(8, 0x22) + mrc(0x52))
    else:
        s += (mrrc(2, 4, 5, 1) + "    umull r4, r5, r4, r5\n" +
              mcrr(11, 4, 5) + cdp(8, 0x22) + mrc(0x72))
    return s + check32(b32(arithmetic(op, a, b)))


def dcp_sqrt(bits):
    value = f64(bits)
    s = load64(0, 1, bits) + mcrr(1, 0, 1) + cdp(2, 1)
    s += mrrc(4, 4, 5, 1) + mcrr(5, 4, 5) + cdp(8, 0x20) + mrrc(9, 0, 1)
    return s + check64(b64(ieee_sqrt(value)))


def fcp_sqrt(bits):
    value = f32(bits)
    s = load32(0, bits) + mcrr(1, 0, 0, 2) + cdp(2, 1)
    s += mrrc(4, 4, 5, 1) + mcrr(12, 4, 5) + cdp(8, 0x22) + mrc(0x92)
    return s + check32(b32(ieee_sqrt(value)))


def dcp_conversion(value_bits, unsigned, rounded):
    value = f64(value_bits)
    seq = load64(0, 1, value_bits) + mcrr(8, 0, 1)
    seq += cdp(0, 1) + cdp(1, 1) + cdp(8, 0x60 if rounded else 0x40)
    seq += mrc(0x33 if unsigned else 0x13)
    return seq + check32(to_int(value, unsigned, rounded))


def fcp_conversion(value_bits, unsigned, rounded):
    value = f32(value_bits)
    seq = load32(0, value_bits) + mcrr(9, 0, 0, 2)
    seq += cdp(0, 1) + cdp(1, 1) + cdp(8, 0x60 if rounded else 0x40)
    seq += mrc(0x33 if unsigned else 0x13)
    return seq + check32(to_int(value, unsigned, rounded))


def worker(core):
    global ERR_MASK
    out = "    movs r6, #%d\n    mov r12, r6\n    movs r7, #0\n" % core
    for op in ("add", "sub", "mul", "div"):
        ERR_MASK = {"add": 1, "sub": 2, "mul": 4, "div": 8}[op]
        for a, b in F64_PAIRS:
            out += pooled(dcp_pair(op, a, b, core))
    ERR_MASK = 16
    for bits in D64_VALUES:
        out += pooled(dcp_sqrt(bits))
    for op in ("add", "sub", "mul", "div"):
        ERR_MASK = {"add": 32, "sub": 64, "mul": 128, "div": 256}[op]
        for a, b in F32_PAIRS:
            out += pooled(fcp_pair(op, a, b))
    ERR_MASK = 512
    for bits in F32_VALUES:
        out += pooled(fcp_sqrt(bits))

    ERR_MASK = 1024
    for a_bits, b_bits in F64_PAIRS:
        a, b = f64(a_bits), f64(b_bits)
        if core and a == 1.5 and b == 2.25:
            a, b = 11.5, 12.25
        seq = load64(0, 1, a_bits) + load64(2, 3, b_bits)
        seq += mcrr(1, 0, 1) + mcrr(1, 2, 3, 1) + cdp(0, 1) + mrc(0x30)
        out += pooled(seq + check32(nzcv_compare(a, b)))
    for a_bits, b_bits in F32_PAIRS:
        a, b = f32(a_bits), f32(b_bits)
        seq = load32(0, a_bits) + load32(1, b_bits)
        seq += mcrr(1, 0, 1, 2) + cdp(0, 1) + mrc(0x30)
        out += pooled(seq + check32(nzcv_compare(a, b)))

    ERR_MASK = 2048
    for bits in D64_VALUES:
        value = f64(bits)
        out += load64(0, 1, bits) + mcrr(1, 0, 1) + mrc(0x10, 15)
        out += pooled("    mrs r0, APSR\n    ubfx r0, r0, #28, #4\n    lsls r0, r0, #28\n" +
                      check32(nzcv_classify(value)))
    ERR_MASK = 4096
    for bits in F32_VALUES:
        value = f32(bits)
        out += load32(0, bits) + mcrr(1, 0, 0, 2) + mrc(0x10, 15)
        out += pooled("    mrs r0, APSR\n    ubfx r0, r0, #28, #4\n    lsls r0, r0, #28\n" +
                      check32(nzcv_classify(value)))

    ERR_MASK = 8192
    for bits in D64_VALUES:
        for unsigned in (False, True):
            for rounded in (False, True):
                out += pooled(dcp_conversion(bits, unsigned, rounded))
    ERR_MASK = 16384
    for bits in F32_VALUES:
        for unsigned in (False, True):
            for rounded in (False, True):
                out += pooled(fcp_conversion(bits, unsigned, rounded))

    ERR_MASK = 32768
    for integer in (0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x01000001):
        signed = integer if integer < 0x80000000 else integer - 0x100000000
        out += load32(0, integer) + mcrr(7, 0, 0) + cdp(0, 1) + cdp(1, 0x21)
        out += cdp(8, 0x20) + mrrc(3, 0, 1) + check64(b64(float(signed)))
        out += load32(0, integer) + mcrr(6, 0, 0) + cdp(0, 1) + cdp(1, 0x21)
        out += cdp(8, 0x20) + mrrc(3, 0, 1) + check64(b64(float(integer)))
        out += load32(0, integer) + mcrr(7, 0, 0) + cdp(0, 1) + cdp(1, 0x21)
        out += cdp(8, 0x22) + mrc(0x32) + check32(b32(float(signed)))
        out += load32(0, integer) + mcrr(6, 0, 0) + cdp(0, 1) + cdp(1, 0x21)
        out += cdp(8, 0x22) + mrc(0x32) + check32(b32(float(integer)))
        out += "    b 1f\n.ltorg\n1:\n"

    ERR_MASK = 65536
    for bits in F32_VALUES:
        value = f32(bits)
        out += load32(0, bits) + mcrr(1, 0, 0, 2) + cdp(8, 0x20) + mrrc(11, 0, 1)
        out += pooled(check64(b64(value)))
    ERR_MASK = 131072
    for bits in D64_VALUES:
        value = f64(bits)
        out += load64(0, 1, bits) + mcrr(1, 0, 1) + cdp(8, 0x22) + mrc(0xB2)
        out += pooled(check32(b32(value)))

    ERR_MASK = 262144
    for a_bits, b_bits in F32_PAIRS:
        a, b = f32(a_bits), f32(b_bits)
        seq = load32(0, a_bits) + load32(1, b_bits) + mcrr(1, 0, 1, 2)
        seq += mrrc(1, 4, 5, 1) + "    umull r4, r5, r4, r5\n"
        seq += mcrr(3, 4, 5) + cdp(8, 0x20) + mrrc(5, 0, 1)
        out += pooled(seq + check64(b64(arithmetic("mul", a, b))))

    ERR_MASK = 524288
    out += (load64(0, 1, b64(3.0)) + load64(2, 3, b64(4.0)) +
            mcrr(1, 0, 1) + mcrr(1, 2, 3, 1) + cdp(0, 1) + cdp(1, 1) +
            mrc2(0x30, 3) + load32(4, 0x80000000) + "    cmp r3, r4\n"
            "    it ne\n    orrne r7, r7, #262144\n" +
            mrrc(0, 4, 5, 8) + mrrc(0, 8, 9, 9) + mrrc(0, 10, 11, 10) +
            mrc2(0x30, 3) + "    cmp r3, #0\n"
            "    it ne\n    orrne r7, r7, #262144\n" +
            load64(0, 1, b64(6.0)) + load64(2, 3, b64(7.0)) +
            mcrr(1, 0, 1) + mcrr(1, 2, 3, 1) +
            mcrr(2, 0, 1) + mcrr(3, 2, 3) + cdp(8, 0x20) + mrrc(5, 0, 1) +
            mcrr(0, 4, 5) + mcrr(0, 8, 9, 1) + mcrr(0, 10, 11, 2) +
            cdp(8, 0x20) + mrrc(1, 0, 1) +
            check64(b64(7.0)))

    out += ("    ldr r0, =0xC0DE0000\n"
            "    orr r0, r0, r7\n"
            "    orr r0, r0, r12, lsl #8\n"
            "    ldr r1, =0x40001000\n"
            "    add r1, r1, r12, lsl #2\n"
            "    str r0, [r1]\n"
            "    str r0, [r1]\n"
            "    str r0, [r1]\n"
            "dcp_spin%d:\n    b dcp_spin%d\n" % (core, core))
    return out


S = r"""
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
    b worker0
    .ltorg
.thumb_func
core1entry:
    ldr r4, =worker1+1
    bx r4
    .ltorg
.thumb_func
worker0:
"""
S += worker(0)
S += "\n.thumb_func\nworker1:\n"
S += worker(1)
S += ".ltorg\n"

open("dcp.s", "w").write(S)
subprocess.check_call(["arm-linux-gnueabihf-gcc", "-c", "-o", "dcp.o", "dcp.s"])
subprocess.check_call(["arm-linux-gnueabihf-ld", "-Ttext=0x20000000", "-o", "dcp.elf", "dcp.o"])
subprocess.check_call(["arm-linux-gnueabihf-objcopy", "-O", "binary", "dcp.elf", "dcp.bin"])
b = open("dcp.bin", "rb").read()
b += b"\0" * (-len(b) % 4)
open("dcp.um2", "wb").write(b"2CMU" + struct.pack("<IIII", 0, 1, 0x20000000, len(b) // 4) + b)
print("words", len(b) // 4)

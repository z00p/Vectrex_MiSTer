/* SPDX-License-Identifier: GPL-2.0-or-later
 * Adapter: the mz80 C API the Tac/Scan port and AAE's cpu_control/SegaG80 call, mapped
 * onto AAE's cpu_z80 (third_party/aae_z80/cpu_z80.cpp, The Unlicense).
 *
 * One CPU only (Tac/Scan has a single Z80). The mz80 "context" in this build is just the
 * handler/pointer record (see mz80_compat.h); registers live inside the cpu_z80 object.
 * Objects are placement-constructed in static storage, so no operator new is needed. */
#include <new>
#include <stdlib.h>
#include "deftypes.h"
#include "cpu_z80.h"
#include "mz80_compat.h"

alignas(cpu_z80) static unsigned char g_storage[sizeof(cpu_z80)];
static cpu_z80 *g_cpu = nullptr;
static UINT8   *g_base = nullptr;
static UINT32   g_elapsed = 0;     /* ticks since last clear (kept here: the core's counter is private) */
static unsigned g_idle_pc = 0;     /* 0 = off */

/* HANDLER ADDRESS CONVENTION. mz80 (and every handler in the AAE drivers, e.g. SegaG80.c:
 * `vectorram[address-0xe000]`) receives the ABSOLUTE address. cpu_z80 calls the handler with
 * `addr - lowAddr` (an offset). To keep the drivers unmodified, the core is given private
 * copies of the memory handler tables whose entries point at trampolines that add lowAddr
 * back and call the original handler with the original entry. Port handlers already get the
 * absolute port number in both cores, so they are passed through. */
#define MAX_HANDLERS 32
static MemoryReadByte  g_rd[MAX_HANDLERS + 1];
static MemoryWriteByte g_wr[MAX_HANDLERS + 1];
static MemoryReadByte  *g_rd_src = nullptr;    /* what g_rd was built from */
static MemoryWriteByte *g_wr_src = nullptr;

static UINT8 rd_tramp(UINT32 off, MemoryReadByte *e)
{
    MemoryReadByte *o = (MemoryReadByte *)e->pUserArea;
    return o->memoryCall(off + o->lowAddr, o);
}
static void wr_tramp(UINT32 off, UINT8 v, MemoryWriteByte *e)
{
    MemoryWriteByte *o = (MemoryWriteByte *)e->pUserArea;
    o->memoryCall(off + o->lowAddr, v, o);
}
static MemoryReadByte *wrap_rd(MemoryReadByte *src)
{
    if (!src) return nullptr;
    if (src == g_rd_src) return g_rd;
    int n = 0;
    for (; src[n].lowAddr != 0xffffffffu; n++) {
        if (n >= MAX_HANDLERS) abort();
        g_rd[n].lowAddr = src[n].lowAddr; g_rd[n].highAddr = src[n].highAddr;
        g_rd[n].memoryCall = rd_tramp; g_rd[n].pUserArea = &src[n];
    }
    g_rd[n].lowAddr = 0xffffffffu; g_rd[n].highAddr = 0xffffffffu;
    g_rd[n].memoryCall = nullptr; g_rd[n].pUserArea = nullptr;
    g_rd_src = src;
    return g_rd;
}
static MemoryWriteByte *wrap_wr(MemoryWriteByte *src)
{
    if (!src) return nullptr;
    if (src == g_wr_src) return g_wr;
    int n = 0;
    for (; src[n].lowAddr != 0xffffffffu; n++) {
        if (n >= MAX_HANDLERS) abort();
        g_wr[n].lowAddr = src[n].lowAddr; g_wr[n].highAddr = src[n].highAddr;
        g_wr[n].memoryCall = wr_tramp; g_wr[n].pUserArea = &src[n];
    }
    g_wr[n].lowAddr = 0xffffffffu; g_wr[n].highAddr = 0xffffffffu;
    g_wr[n].memoryCall = nullptr; g_wr[n].pUserArea = nullptr;
    g_wr_src = src;
    return g_wr;
}

extern "C" {

void mz80init(void) {}

void mz80reset(void)
{
    if (g_cpu) g_cpu->mz80reset();
    g_elapsed = 0;
}

void mz80SetContext(void *vctx)
{
    CONTEXTMZ80 *c = (CONTEXTMZ80 *)vctx;
    if (!g_cpu || g_base != c->z80Base) {
        g_cpu = new (g_storage) cpu_z80(c->z80Base, wrap_rd(c->z80MemRead), wrap_wr(c->z80MemWrite),
                                        c->z80IoRead, c->z80IoWrite, 0, 0);
        g_base = c->z80Base;
    }
    g_cpu->z80MemoryRead  = wrap_rd(c->z80MemRead);
    g_cpu->z80MemoryWrite = wrap_wr(c->z80MemWrite);
    g_cpu->z80IoRead      = c->z80IoRead;
    g_cpu->z80IoWrite     = c->z80IoWrite;
    g_cpu->z80intAddr     = (uint16_t)c->z80intAddr;
    g_cpu->z80nmiAddr     = (uint16_t)c->z80nmiAddr;
}

void mz80GetContext(void *vctx)
{
    CONTEXTMZ80 *c = (CONTEXTMZ80 *)vctx;
    if (!g_cpu) return;
    c->z80Base      = g_base;
    c->z80MemRead   = g_rd_src;       /* the caller's own tables, not our wrapped copies */
    c->z80MemWrite  = g_wr_src;
    c->z80IoRead    = g_cpu->z80IoRead;
    c->z80IoWrite   = g_cpu->z80IoWrite;
    c->z80intAddr   = g_cpu->z80intAddr;
    c->z80nmiAddr   = g_cpu->z80nmiAddr;
}

UINT32 mz80GetContextSize(void) { return sizeof(CONTEXTMZ80); }

void mz80_set_idle_pc(unsigned int pc) { g_idle_pc = pc; }

/* Run for at least `cycles` Z80 cycles (an instruction is never split, as in mz80).
 *
 * The idle-spin skip of the old build (-DMZ80_IDLE_SKIP in mz80.c) is reproduced here at
 * the adapter level, so the core stays untouched: the second time the PC reaches the
 * registered address in a slice, the rest of the slice is charged and execution stops.
 * See the long note in game/tacscan/src/aae_machine.c for why this is safe for Tac/Scan. */
UINT32 mz80exec(UINT32 cycles)
{
    if (!g_cpu) return 0x80000000u;
    unsigned long ran = 0;
    int seen = 0;
    while (ran < cycles) {
        if (g_idle_pc && g_cpu->GetPC() == g_idle_pc) {
            if (seen) { ran = cycles; break; }
            seen = 1;
        }
        ran += g_cpu->mz80step();
    }
    g_elapsed += (UINT32)ran;
    return 0x80000000u;               /* "ran to completion", as mz80exec */
}

UINT32 mz80GetElapsedTicks(UINT32 clear)
{
    UINT32 t = g_elapsed;
    if (clear) g_elapsed = 0;
    return t;
}

void mz80ReleaseTimeslice(void) {}
void mz80ClearPendingInterrupt(void) { if (g_cpu) g_cpu->mz80ClearPendingInterrupt(); }
UINT32 mz80int(UINT32 v) { return g_cpu ? g_cpu->mz80int(v) : 0; }
UINT32 mz80nmi(void)     { return g_cpu ? g_cpu->mz80nmi() : 0; }

UINT32 mz80GetRegisterValue(void *, UINT32 reg)
{
    if (reg == CPUREG_PC && g_cpu) {
        /* The Sega G80 driver decrypts a memory write when the byte at PC-3 is 0x32
         * (LD (nn),A). mz80 pushed an interrupt's return address without calling the write
         * handlers; this core sends the push through them, with GetPPC() == -1. Report a PC
         * whose PC-3 byte is not 0x32 then, so a push is never "decrypted" (that corrupted
         * the return address: Tac/Scan reset to its title in stage 2). */
        if (g_cpu->GetPPC() < 0 && g_base) {
            unsigned a = 0;
            while (a < 0xFFF0u && g_base[a] == 0x32) a++;
            return a + 3;
        }
        return g_cpu->GetPC();
    }
    return 0;
}

} /* extern "C" */

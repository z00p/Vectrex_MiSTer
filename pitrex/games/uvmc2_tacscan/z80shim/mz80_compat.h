/* SPDX-License-Identifier: GPL-2.0-or-later
 * mz80-compatible C API in front of AAE's cpu_z80 (third_party/aae_z80).
 *
 * Force-included (-include) into every C translation unit of the Tac/Scan build when
 * TACSCAN_Z80=aae. In C it also defines _MZ80_H_, the include guard of the old
 * third_party/aae/mz80/mz80.h, so that header (Neil Bradley's licence, no modified
 * redistribution) is skipped even though z80/z80.h still `#include`s it: nothing of
 * mz80 is compiled or linked in this configuration. */
#ifndef MZ80_COMPAT_H
#define MZ80_COMPAT_H

#ifndef __cplusplus
#define _MZ80_H_          /* neutralise ../mz80/mz80.h */
#include "osd_cpu.h"      /* UINT8/16/32 (third_party/aae) */
#endif

#define CPUREG_PC 0

#ifndef __cplusplus
/* The port handler records that mz80.h used to define (C side; deftypes.h does it for C++).
 * MemoryReadByte/MemoryWriteByte come from osd_cpu.h. */
struct z80PortWrite {
    UINT16 lowIoAddr;
    UINT16 highIoAddr;
    void (*IOCall)(UINT16, UINT8, struct z80PortWrite *);
    void *pUserArea;
};
struct z80PortRead {
    UINT16 lowIoAddr;
    UINT16 highIoAddr;
    UINT16 (*IOCall)(UINT16, struct z80PortRead *);
    void *pUserArea;
};
struct z80TrapRec {
    UINT16 trapAddr;
    UINT8  skipCnt;
    UINT8  origIns;
};
#endif

/* Only the fields the Tac/Scan port and the AAE drivers touch. */
struct mz80context {
    UINT8 *z80Base;                       /* 64 KB address space (also the "RAM" the core indexes) */
    struct MemoryReadByte  *z80MemRead;
    struct MemoryWriteByte *z80MemWrite;
    struct z80PortRead     *z80IoRead;
    struct z80PortWrite    *z80IoWrite;
    UINT32 z80intAddr;
    UINT32 z80nmiAddr;
};
typedef struct mz80context CONTEXTMZ80;

#ifdef __cplusplus
extern "C" {
#endif
void   mz80init(void);
void   mz80reset(void);
void   mz80SetContext(void *ctx);
void   mz80GetContext(void *ctx);
UINT32 mz80GetContextSize(void);
UINT32 mz80exec(UINT32 cycles);
UINT32 mz80GetElapsedTicks(UINT32 clear);
void   mz80ReleaseTimeslice(void);
void   mz80ClearPendingInterrupt(void);
UINT32 mz80int(UINT32 vector);
UINT32 mz80nmi(void);
UINT32 mz80GetRegisterValue(void *ctx, UINT32 reg);   /* only CPUREG_PC (0) is supported */
void   mz80_set_idle_pc(unsigned int pc);
#ifdef __cplusplus
}
#endif
#endif

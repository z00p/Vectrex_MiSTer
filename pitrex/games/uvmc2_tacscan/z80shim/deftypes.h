/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal shim for third_party/aae_z80/cpu_z80.h (AAE's "deftypes.h").
 * Supplies only what the Z80 core needs: fixed-width UINT types and the MZ80
 * memory/port handler records. Written for this project, not copied from AAE.
 * Layout matches the C-side structs in upstream third_party/aae (osd_cpu.h, z80/z80.h). */
#ifndef Z80SHIM_DEFTYPES_H
#define Z80SHIM_DEFTYPES_H
#include <stdint.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;

struct MemoryWriteByte {
    UINT32 lowAddr;
    UINT32 highAddr;
    void (*memoryCall)(UINT32, UINT8, struct MemoryWriteByte *);
    void *pUserArea;
};

struct MemoryReadByte {
    UINT32 lowAddr;
    UINT32 highAddr;
    UINT8 (*memoryCall)(UINT32, struct MemoryReadByte *);
    void *pUserArea;
};

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

#endif

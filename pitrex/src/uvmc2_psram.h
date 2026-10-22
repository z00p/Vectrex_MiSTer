// SPDX-License-Identifier: GPL-2.0-or-later
/* UVMC2 on MiSTer: force-included into games that address the cartridge's PSRAM and timer
 * through overridable macros (e.g. Tac/Scan's jack mixer, ts_jack.c). The PSRAM becomes a
 * plain buffer and the timer a wall clock, both in src/uvm2_mister_stubs.c. */
#ifndef UVMC2_PSRAM_H
#define UVMC2_PSRAM_H
#include <stdint.h>
extern unsigned char uvmc2_psram[];
uint32_t uvmc2_now_us(void);
#define TS_PSRAM_WRITE (uvmc2_psram)
#define TS_PSRAM_READ  ((const unsigned char *)uvmc2_psram)
#define TS_NOW()       uvmc2_now_us()
#endif

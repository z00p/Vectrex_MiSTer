/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
#ifndef UM2SD_H
#define UM2SD_H
#include <stdint.h>
int um2sd_open(const char *path, int verbose);
int um2sd_present(void);
void um2sd_select(int on);          /* CS asserted */
uint8_t um2sd_xfer(uint8_t mosi);
uint8_t um2sd_peek(void);           /* what the next um2sd_xfer will return */
void um2sd_stats(void);
unsigned long um2sd_reads(void);    /* sector reads so far (loading indicator progress) */
int um2sd_lean_ok(void);            /* the byte path may run from a stub that saved no VFP registers (no -v -v -v CMD log) */
uint16_t um2sd_crc16(const uint8_t *d, int n), um2sd_crc16_bitwise(const uint8_t *d, int n);   /* table and bitwise forms (checked equal) */
#endif

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
void um2sd_stats(void);
unsigned long um2sd_reads(void);    /* sector reads so far (loading indicator progress) */
#endif

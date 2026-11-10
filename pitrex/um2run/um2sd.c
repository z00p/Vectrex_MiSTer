/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2sd.c -- an SD card in SPI mode, backed by a disk image file (um2run --sd IMAGE).
 * Byte level: the PL022 model feeds it one MOSI byte at a time and takes one MISO byte back.
 * SDHC (block addressed), CMD0/8/9/10/12/13/16/17/18/24/25/55/58/59 and ACMD41/23, enough for FatFs (uvm2_sd.c).
 * The image is opened read/write if possible (writes go through), else read-only (writes accepted and dropped).
 * Images of any size up to 2 TiB work (64-bit file offsets); the card builder still makes at most 2000 MiB.
 */
#define _FILE_OFFSET_BITS 64            /* card images of 2 GiB and more: with a 32-bit long, fseek / ftell wrapped (or failed) at 2 GiB: reads above that came back as stale data */
#include "um2sd.h"
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

/* The byte path (um2sd_xfer and what it calls) touches no VFP/NEON register and calls nothing in libc but pread/pwrite (10 Nov 2026): the
 * card stubs then need no VFP save (um2run.c, um2_hthunk_spi*).  So: blocks are read with pread on the descriptor (no stdio seek and buffer,
 * no memcpy), copies and fills are plain loops that GCC must not turn into memcpy/memset calls, and the CRC16 is table driven (the same
 * values as the bitwise form, checked over every byte and random blocks). */
#define NOLIBC __attribute__((optimize("no-tree-loop-distribute-patterns")))
static FILE *g_f; static int g_fd = -1; static int g_rw; static uint32_t g_sectors;
static uint8_t g_cmd[6]; static int g_ncmd;
static uint8_t g_q[600 + 16]; static int g_qh, g_qn;       /* MISO queue */
static int g_multi; static uint32_t g_next;                /* CMD18 streaming: next block */
static int g_wstate; static uint32_t g_wblk; static int g_wmulti; static uint8_t g_wbuf[512]; static int g_wn; /* write: 0 idle, 1 wait token, 2 data, 3 crc */
static int blk_ok(uint32_t b); static void crc_init(void);
static int g_app, g_acmd41_n, g_selected, g_verbose; static unsigned long g_reads, g_writes;
static uint64_t g_hmosi = 0xcbf29ce484222325ull, g_hmiso = 0xcbf29ce484222325ull; static unsigned long g_nxfer;   /* FNV-1a of the bytes the guest sent / got: the stub and trap paths must agree */

int um2sd_open(const char *path, int verbose) {
	g_verbose = verbose;
	g_f = fopen(path, "r+b"); g_rw = g_f != NULL;
	if (!g_f) g_f = fopen(path, "rb");
	if (!g_f) { perror(path); return -1; }
	fseeko(g_f, 0, SEEK_END); off_t n = ftello(g_f); g_sectors = (uint32_t)(n / 512 > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint64_t)n / 512);
	g_fd = fileno(g_f); crc_init();
	fprintf(stderr, "um2sd: %s, %u sectors (%s)\n", path, g_sectors, g_rw ? "read/write" : "read-only");
	return 0;
}
/* CMD log (-v -v -v): out of line, it calls stdio; the lean card stubs are not used at this level (um2sd_lean_ok) */
__attribute__((noinline)) static void um2sd_cmdlog(int app, unsigned c, uint32_t arg) { fprintf(stderr, "um2sd: %sCMD%u %08x\n", app ? "A" : "", c, arg); }
int um2sd_lean_ok(void) { return g_verbose <= 2; }
int um2sd_present(void) { return g_f != NULL; }
static void qpush(uint8_t b) { if (g_qn < (int)sizeof g_q) g_q[g_qn++] = b; }
static void qclear(void) { g_qh = g_qn = 0; }
static int blk_ok(uint32_t b) { return b < g_sectors; }
uint16_t um2sd_crc16_bitwise(const uint8_t *d, int n) { uint16_t c = 0; while (n--) { c ^= (uint16_t)(*d++) << 8; for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); } return c; }
static uint16_t g_crc_t[256]; static int g_crc_init;
static void crc_init(void) { for (int b = 0; b < 256; b++) { uint8_t x = (uint8_t)b; g_crc_t[b] = um2sd_crc16_bitwise(&x, 1); } g_crc_init = 1; }
/* table form of the bitwise CRC16-CCITT above: c' = (c << 8) ^ T[(c >> 8) ^ byte], T[b] = the bitwise CRC of the single byte b (from 0) */
uint16_t um2sd_crc16(const uint8_t *d, int n) { if (!g_crc_init) crc_init(); uint16_t c = 0; while (n--) c = (uint16_t)((c << 8) ^ g_crc_t[((c >> 8) ^ *d++) & 0xFF]); return c; }
#define crc16_ccitt um2sd_crc16
static uint8_t crc7(const uint8_t *d, int n) { uint8_t c = 0; while (n--) { uint8_t b = *d++; for (int i = 0; i < 8; i++) { c <<= 1; if ((b ^ c) & 0x80) c ^= 0x09; b <<= 1; } } return (uint8_t)(c & 0x7F); }
/* pread / pwrite all of n bytes at off (as fread / fwrite do: on to the end, or until an error): what was not read stays as it was */
static size_t rd_at(uint8_t *d, size_t n, off_t off) { size_t got = 0; while (got < n) { ssize_t r = pread(g_fd, d + got, n - got, off + (off_t)got); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += (size_t)r; } return got; }
static void wr_at(const uint8_t *d, size_t n, off_t off) { size_t put = 0; while (put < n) { ssize_t r = pwrite(g_fd, d + put, n - put, off + (off_t)put); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; put += (size_t)r; } }
NOLIBC static void queue_block(uint32_t blk) {
	uint8_t d[512]; for (int i = 0; i < 512; i++) d[i] = 0xFF;
	if (blk < g_sectors) { if (rd_at(d, 512, (off_t)blk * 512) != 512) { /* short read: rest 0xFF */ } }
	qpush(0xFF); qpush(0xFE);
	for (int i = 0; i < 512; i++) qpush(d[i]);
	{ uint16_t c = crc16_ccitt(d, 512); qpush((uint8_t)(c >> 8)); qpush((uint8_t)c); }   /* CRC16: images that switch CRC on with CMD59 verify it */
	g_reads++;
}
NOLIBC static void reg_block(const uint8_t *r16) { uint8_t r[16]; for (int i = 0; i < 16; i++) r[i] = r16[i]; r[15] = (uint8_t)((crc7(r, 15) << 1) | 1); qpush(0xFF); qpush(0xFE); for (int i = 0; i < 16; i++) qpush(r[i]); { uint16_t c = crc16_ccitt(r, 16); qpush((uint8_t)(c >> 8)); qpush((uint8_t)c); } }
static const uint8_t g_cid[16] = { 0x03, 'U', 'M', 'U', 'M', '2', 'S', 'D', 0x10, 0, 0, 0, 1, 0x01, 0x01, 0x01 };
NOLIBC static void do_cmd(void) {
	unsigned c = g_cmd[0] & 0x3F; uint32_t arg = ((uint32_t)g_cmd[1] << 24) | (g_cmd[2] << 16) | (g_cmd[3] << 8) | g_cmd[4];
	int app = g_app; g_app = 0;
	qclear(); qpush(0xFF);                              /* NCR */
	if (g_verbose > 2) um2sd_cmdlog(app, c, arg);
	if (app && c == 41) { qpush(g_acmd41_n++ ? 0x00 : 0x01); return; }
	if (app && c == 23) { qpush(0x00); return; }
	if (app && c == 42) { qpush(0x00); return; }                       /* SET_CLR_CARD_DETECT */
	switch (c) {
	case 0: g_multi = 0; g_wstate = 0; g_acmd41_n = 0; qpush(0x01); break;
	case 8: qpush(0x01); qpush(0); qpush(0); qpush((arg >> 8) & 0xF); qpush(arg & 0xFF); break;
	case 9: { uint32_t cs = g_sectors / 1024; cs = cs ? cs - 1 : 0; uint8_t csd[16];
	          csd[0] = 0x40; csd[1] = 0x0E; csd[2] = 0x00; csd[3] = 0x32; csd[4] = 0x5B; csd[5] = 0x59; csd[6] = 0x00; csd[7] = (uint8_t)((cs >> 16) & 0x3F);
	          csd[8] = (uint8_t)(cs >> 8); csd[9] = (uint8_t)cs; csd[10] = 0x7F; csd[11] = 0x80; csd[12] = 0x0A; csd[13] = 0x40; csd[14] = 0x40; csd[15] = 0x01;
	          qpush(0x00); reg_block(csd); } break;
	case 10: qpush(0x00); reg_block(g_cid); break;
	case 12: g_multi = 0; qclear(); qpush(0xFF); qpush(0x00); qpush(0x00); qpush(0xFF); break;    /* R1b: busy then free */
	case 13: qpush(0x00); qpush(0x00); break;
	case 16: qpush(0x00); break;
	case 17: qpush(blk_ok(arg) ? 0x00 : 0x40); if (blk_ok(arg)) queue_block(arg); break;
	case 18: qpush(blk_ok(arg) ? 0x00 : 0x40); if (blk_ok(arg)) { g_multi = 1; g_next = arg; queue_block(g_next++); } break;
	case 24: qpush(0x00); g_wstate = 1; g_wblk = arg; g_wmulti = 0; break;
	case 25: qpush(0x00); g_wstate = 1; g_wblk = arg; g_wmulti = 1; break;
	case 55: g_app = 1; qpush(g_acmd41_n >= 1 ? 0x00 : 0x01); break;
	case 58: qpush(0x00); qpush(0xC0); qpush(0xFF); qpush(0x80); qpush(0x00); break;   /* OCR: powered up, CCS=1 (SDHC) */
	case 59: qpush(0x00); break;
	default: qpush(0x05); break;                       /* illegal command */
	}
}
static int g_cs_seen;                                  /* 0 until the image drives CS once: BattleZone never does (CS strapped low), the card is then always selected */
void um2sd_select(int on) {
	g_cs_seen = 1;
	if (on == g_selected) return;
	g_selected = on;
	if (!on) { g_ncmd = 0; qclear(); g_multi = 0; g_wstate = 0; }
}
uint8_t um2sd_peek(void) { return (!g_f || (!g_selected && g_cs_seen) || g_qh >= g_qn) ? 0xFF : g_q[g_qh]; }   /* the byte the next um2sd_xfer returns (bit-banged SPI shifts it out before the host byte is complete) */
uint8_t um2sd_xfer(uint8_t out) {
	if (!g_f || (!g_selected && g_cs_seen)) return 0xFF;
	uint8_t in = 0xFF;
	if (g_qh < g_qn) in = g_q[g_qh++];
	g_hmosi = (g_hmosi ^ out) * 0x100000001b3ull; g_hmiso = (g_hmiso ^ in) * 0x100000001b3ull; g_nxfer++;
	if (g_qh >= g_qn) { g_qh = g_qn = 0; if (g_multi) queue_block(g_next++); }
	/* ---- host -> card ---- */
	if (g_wstate == 1) {                                   /* waiting for the data token */
		if (out == 0xFE || out == 0xFC) { g_wstate = 2; g_wn = 0; }
		else if (out == 0xFD && g_wmulti) { g_wstate = 0; qpush(0x00); qpush(0xFF); }
		return in;
	}
	if (g_wstate == 2) { g_wbuf[g_wn++] = out; if (g_wn == 512) g_wstate = 3, g_wn = 0; return in; }
	if (g_wstate == 3) {
		if (++g_wn == 2) {
			if (g_rw && g_wblk < g_sectors) wr_at(g_wbuf, 512, (off_t)g_wblk * 512);
			g_writes++; g_wblk++;
			qclear(); qpush(0x05); qpush(0x00); qpush(0xFF);   /* data accepted, one busy byte */
			g_wstate = g_wmulti ? 1 : 0;
		}
		return in;
	}
	if (g_ncmd == 0) { if ((out & 0xC0) == 0x40) { g_cmd[0] = out; g_ncmd = 1; } return in; }
	g_cmd[g_ncmd++] = out;
	if (g_ncmd == 6) { g_ncmd = 0; do_cmd(); }
	return in;
}
void um2sd_stats(void) { if (g_f) fprintf(stderr, "um2sd: %lu sector reads, %lu writes; %lu byte exchanges, mosi hash %016llx, miso hash %016llx\n", g_reads, g_writes, g_nxfer, (unsigned long long)g_hmosi, (unsigned long long)g_hmiso); }
unsigned long um2sd_reads(void) { return g_reads; }

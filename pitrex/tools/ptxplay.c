/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptxplay.c - PC stand-in for the core's ring reader.
 *
 * Replays a .ptxs stream (raw little-endian 64-bit commands, see PROTOCOL.md)
 * into a small software VIA + Vectrex analog model, cycle by cycle, and writes
 * selected frames as PNG. It also prints per-frame stats, so timing mistakes in
 * the ARM-side shim show up as numbers as well as bent vectors.
 *
 * The analog model follows the usual emulator one (vecx): X integrator input is
 * the DAC; the mux (PB0 low, select PB2:1) samples the DAC into Y / zero-ref /
 * Z; integrators run while RAMP (PB7) is low, reset while ZERO (CA2) is low,
 * and the beam is lit while CB2 is high (or by the shift register output).
 *
 * usage: ptxplay stream.ptxs outprefix [first_frame] [count]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 660
#define H 820
#define ALG_MAX_X 33000
#define ALG_MAX_Y 41000

/* ---- VIA (just what the display needs) --------------------------------- */
static uint8_t ora, orb, ddra, ddrb, acr, pcr, sr;
static uint8_t t1ll, t1lh, t2ll;
static long t1_left = -1;      /* cycles until PB7 returns high (one-shot) */
static long t2_left = -1;
static int sr_bits = 0, sr_phase = 0, sr_out = 1;

/* ---- analog ------------------------------------------------------------ */
static long cx, cy;            /* beam position */
static int xsh, ysh = 128, rsh = 128, zsh;   /* unsigned 0..255 sample-and-holds */

/* ---- frame buffer ------------------------------------------------------ */
static uint16_t fb[H][W];
static unsigned long frame_no, frame_cycles, frame_writes, frame_lit, frame_waits;
static unsigned long max_delta;
static long first = 0, count = 8;
static const char *prefix;

static void plot(long x, long y, int v)
{
	int px = (int)((x + ALG_MAX_X) * W / (2 * ALG_MAX_X));
	int py = (int)((y + ALG_MAX_Y) * H / (2 * ALG_MAX_Y));
	if (px < 0 || py < 0 || px >= W || py >= H) return;
	unsigned s = fb[py][px] + (unsigned)v;
	fb[py][px] = s > 0xFFFF ? 0xFFFF : (uint16_t)s;
}

static void line(long x0, long y0, long x1, long y1, int v)
{
	long dx = labs(x1 - x0), dy = labs(y1 - y0);
	long steps = (dx > dy ? dx : dy) / 40 + 1;   /* ~1 sample per 0.8 px */
	for (long i = 0; i <= steps; i++)
		plot(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, v);
}

static uint32_t crc_table[256];
static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n)
{
	c ^= 0xFFFFFFFFu;
	while (n--) c = crc_table[(c ^ *b++) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8]; be32(hdr, len); memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len) fwrite(data, 1, len, f);
	uint32_t c = crc(0, (const uint8_t *)type, 4);
	c = crc(c, data, len);
	uint8_t cb[4]; be32(cb, c); fwrite(cb, 1, 4, f);
}

/* Greyscale PNG with stored (uncompressed) deflate blocks: no zlib needed. */
static void write_png(const char *path)
{
	size_t raw_len = (size_t)H * (W + 1);
	uint8_t *raw = malloc(raw_len);
	unsigned peak = 1;
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (fb[y][x] > peak) peak = fb[y][x];
	for (int y = 0; y < H; y++) {
		raw[(size_t)y * (W + 1)] = 0;
		for (int x = 0; x < W; x++) {
			unsigned v = fb[y][x] * 255u * 3 / peak;     /* gentle boost */
			raw[(size_t)y * (W + 1) + 1 + x] = (uint8_t)(v > 255 ? 255 : v);
		}
	}
	size_t nblk = (raw_len + 65534) / 65535;
	size_t z_len = 2 + raw_len + nblk * 5 + 4;
	uint8_t *z = malloc(z_len), *p = z;
	*p++ = 0x78; *p++ = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t off = 0; off < raw_len; off += 65535) {
		uint16_t n = (uint16_t)(raw_len - off > 65535 ? 65535 : raw_len - off);
		*p++ = off + n >= raw_len;
		*p++ = n & 0xFF; *p++ = n >> 8; *p++ = ~n & 0xFF; *p++ = (~n >> 8) & 0xFF;
		memcpy(p, raw + off, n); p += n;
	}
	for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	be32(p, (b << 16) | a); p += 4;
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); exit(1); }
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	uint8_t ihdr[13]; be32(ihdr, W); be32(ihdr + 4, H);
	ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, (uint32_t)(p - z));
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw); free(z);
}

static int pb7(void)
{
	if (acr & 0x80) return t1_left > 0 ? 0 : 1;      /* T1 drives PB7 */
	return (ddrb & 0x80) ? (orb >> 7) & 1 : 1;
}
static int ca2(void) { return ((pcr >> 1) & 7) == 6 ? 0 : 1; }
static int cb2(void)
{
	int mode = (acr >> 2) & 7;
	if (mode == 6 || mode == 4 || mode == 5) return sr_out;
	return ((pcr >> 5) & 7) == 6 ? 0 : 1;
}

static void analog_sample(void)
{
	xsh = ora ^ 0x80;
	if ((orb & 1) == 0) {
		switch ((orb >> 1) & 3) {
		case 0: ysh = xsh; break;
		case 1: rsh = xsh; break;
		case 2: zsh = xsh; break;
		default: break;              /* sound */
		}
	}
}

/* advance the machine one E-cycle */
static void tick(void)
{
	long ox = cx, oy = cy;
	if (!ca2()) { cx = 0; cy = 0; }
	else if (!pb7()) {
		cx += (long)xsh - rsh;
		cy += (long)rsh - ysh;
	}
	if (cb2() && zsh > 128 && frame_no >= (unsigned long)first) {
		line(ox, oy, cx, cy, (zsh - 128) * 4);
		frame_lit++;
	}
	if (t1_left > 0) t1_left--;
	if (t2_left > 0) t2_left--;
	if (sr_bits > 0 && ++sr_phase >= 2) {       /* one bit per 2 cycles (vecx) */
		sr_phase = 0;
		sr_out = (sr >> 7) & 1;
		sr = (uint8_t)(sr << 1 | sr_out);
		sr_bits--;
	}
	frame_cycles++;
}

static void via_write(uint16_t addr, uint8_t d)
{
	switch (addr & 0xF) {
	case 0x0: orb = d; analog_sample(); break;
	case 0x1: case 0xF: ora = d; analog_sample(); break;
	case 0x2: ddrb = d; break;
	case 0x3: ddra = d; break;
	case 0x4: case 0x6: t1ll = d; break;
	case 0x5: t1lh = d; t1_left = (long)(t1lh << 8 | t1ll) + 1; break;
	case 0x7: t1lh = d; break;
	case 0x8: t2ll = d; break;
	case 0x9: t2_left = (long)(d << 8 | t2ll) + 1; break;
	case 0xA: sr = d; sr_bits = 8; sr_phase = 0; break;
	case 0xB: acr = d; break;
	case 0xC: pcr = d; break;
	default: break;
	}
}

static void end_frame(void)
{
	if (frame_no >= (unsigned long)first && frame_no < (unsigned long)(first + count)) {
		char path[512];
		snprintf(path, sizeof path, "%s_%04lu.png", prefix, frame_no);
		write_png(path);
		printf("frame %4lu: %6lu cycles (%5.1f Hz), %5lu writes, %6lu lit cycles, %lu waits -> %s\n",
		       frame_no, frame_cycles, 1500000.0 / (frame_cycles ? frame_cycles : 1),
		       frame_writes, frame_lit, frame_waits, path);
	}
	memset(fb, 0, sizeof fb);
	frame_no++;
	frame_cycles = frame_writes = frame_lit = frame_waits = 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s stream.ptxs outprefix [first] [count]\n", argv[0]); return 2; }
	prefix = argv[2];
	if (argc > 3) first = atol(argv[3]);
	if (argc > 4) count = atol(argv[4]);
	crc_init();
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	uint8_t b[8];
	unsigned long total = 0, other_addr = 0;
	while (fread(b, 1, 8, f) == 8) {
		uint64_t w = 0;
		for (int i = 7; i >= 0; i--) w = w << 8 | b[i];
		uint32_t op = (uint32_t)(w >> 56) & 0xFF, delta = (uint32_t)(w >> 40) & 0xFFFF;
		uint16_t addr = (uint16_t)(w >> 24);
		uint8_t data = (uint8_t)(w >> 16);
		if (delta > max_delta) max_delta = delta;
		for (uint32_t i = 0; i < delta; i++) tick();
		total++;
		switch (op) {
		case 0x01:
			if ((addr & 0xF000) == 0xD000) via_write(addr, data); else other_addr++;
			frame_writes++;
			tick();
			break;
		case 0x04: tick(); break;              /* side-effect read: one bus cycle */
		case 0x02: frame_waits++; break;
		case 0x10: end_frame(); break;
		case 0x7F: break;
		default: fprintf(stderr, "unknown op 0x%02x at command %lu\n", op, total); return 1;
		}
	}
	fclose(f);
	printf("%lu commands, %lu frames, max delta %lu, %lu writes outside the VIA\n",
	       total, frame_no, max_delta, other_addr);
	return 0;
}

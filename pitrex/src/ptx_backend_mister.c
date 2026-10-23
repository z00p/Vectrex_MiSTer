/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx_backend_mister.c - DE10-Nano backend: a command ring in the DDR3 that
 * the Vectrex core reads through its DDR arbiter. Same /dev/mem + mmap
 * approach as Main_MiSTer's shmem_map().
 *
 * Layout (byte offsets from PTX_DDR_BASE; see PROTOCOL.md):
 *   0x0000 ARM header : magic, version, log2 entries, wr_idx (u64), heartbeat
 *   0x0040 core status: rd_idx (u64), core_cycles (u64), frames, underruns
 *   0x0080 core input : ptx_input_t
 *   0x1000 ring       : (1 << PTX_RING_LOG2) x 64-bit commands
 *
 * Base: the core's framebuffer + artwork occupy DDR word addresses
 * 0x06000000-0x065CFFFF (bytes 0x30000000-0x32E7FFFF), so the ring sits above
 * them at 0x34000000. To be agreed with Videodr0me.
 */
#include "ptx.h"

#ifdef __linux__
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef PTX_DDR_BASE
#define PTX_DDR_BASE   0x34000000u
#endif
#define PTX_RING_LOG2  16u
#define PTX_RING_N     (1u << PTX_RING_LOG2)
#define PTX_MAGIC      0x58525450u            /* "PTRX" */
#define PTX_VERSION    0u
#define PTX_AUD_LOG2   13u                    /* PCM ring (B6 cores): 8192 words */
#define PTX_AUD_N      (1u << PTX_AUD_LOG2)
#define PTX_AUD_TARGET 800u                   /* words kept queued: 3200 samples = 100 ms */
#define PTX_MAP_SIZE   (0x1000u + PTX_RING_N * 8u + PTX_AUD_N * 8u)
#define PTX_PUBLISH    32u                    /* = one 256-byte DDR burst */
#define PTX_LEAD_FRAMES 2u                    /* frames the ARM may run ahead */

typedef struct {
	volatile uint32_t magic, version, log2n, pad0;
	volatile uint64_t wr_idx;
	volatile uint32_t hb_lo, hb_hi;          /* coded heartbeat: hi = ~lo ^ PTX_MAGIC */
	volatile uint64_t aud_wr;                /* PCM words written (B6) */
} arm_hdr_t;

typedef struct {
	volatile uint64_t rd_idx;
	volatile uint64_t core_cycles;
	volatile uint32_t frames, underruns;
	volatile uint32_t build_magic, build;   /* "PTXC", build number */
	volatile uint64_t aud_rd;               /* PCM words fetched (B6) */
} core_status_t;

static int memfd = -1;
static volatile uint8_t *base;
static arm_hdr_t *hdr;
static core_status_t *st;
static volatile ptx_input_t *in;
static volatile uint64_t *ring;
static volatile uint64_t *aud;         /* PCM ring */
static uint64_t aud_wr;                /* PCM words written */
static uint64_t aud_part;              /* samples collected for the next word */
static unsigned aud_nsamp;
static uint64_t wr, published;
static uint64_t frame_end[8];
static uint32_t n_frames;
static uint32_t hb;
static int core_lost;                  /* set once the core stops answering: never write again */
static uint32_t last_seq;
static long last_seq_ms, last_name_ms;

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* Rule 1 (no writes unless the Vectrex core is running): MiSTer Main names the
   loaded core in /tmp/CORENAME. */
static int vectrex_core_loaded(void)
{
	char name[64] = {0};
	FILE *f = fopen("/tmp/CORENAME", "r");
	if (!f) return 0;
	size_t n = fread(name, 1, sizeof name - 1, f);
	fclose(f);
	while (n && (name[n - 1] == '\n' || name[n - 1] == '\r' || name[n - 1] == ' ')) name[--n] = 0;
	return strcasecmp(name, "Vectrex-PiTrex") == 0 || strcmp(name, "VECTREX") == 0;
}

/* The core writes its status block (signature "PTXC", input sequence counter)
   only while PiTrex Mode is on; a moving counter proves it is live right now,
   which stale RAM cannot. */
static int core_signature(void) { return st->build_magic == 0x43585450u; }

static void check_core_alive(void)
{
	if (core_lost) return;
	long t = now_ms();
	if (in->seq != last_seq) { last_seq = in->seq; last_seq_ms = t; }
	int ok = core_signature() && t - last_seq_ms < 500;
	if (ok && t - last_name_ms >= 1000) { last_name_ms = t; ok = vectrex_core_loaded(); }
	if (!ok) {
		core_lost = 1;
		fprintf(stderr, "ptx: Vectrex core or PiTrex Mode gone - stopping (no further writes)\n");
		exit(3);
	}
}

static void hb_tick(void)
{
	if (core_lost || !hdr) return;
	hb++;
	hdr->hb_lo = hb;
	hdr->hb_hi = ~hb ^ PTX_MAGIC;
}

/* Every wait also ticks the heartbeat, so it keeps changing (at least once per
   frame, and every few hundred microseconds while blocked) for as long as the
   program runs. The core treats "magic valid and heartbeat changed within
   100 ms" as "PiTrex program present"; stale RAM after a crash cannot pass. */
static void nap_us(long us)
{
	struct timespec ts = { 0, us * 1000L };
	nanosleep(&ts, NULL);
	check_core_alive();
	hb_tick();
}

static void publish(void)
{
	__sync_synchronize();           /* commands visible before the index */
	hdr->wr_idx = wr;
	published = wr;
}

/* Ctrl-C, kill or a dropped SSH session: clear the magic so the core hands
   back at once (a real crash is still caught by the 100 ms heartbeat timeout). */
static void on_signal(int sig)
{
	(void)sig;
	if (!core_lost && hdr) hdr->magic = 0;
	_exit(0);
}

static int m_open(void)
{
	/* Launchers start the program, then load the core: PTX_WAIT_CORE=<seconds>
	   lets it wait for the Vectrex core (reading /tmp/CORENAME only). */
	const char *wait_env = getenv("PTX_WAIT_CORE");
	long wait_ms = wait_env ? atol(wait_env) * 1000L : 0;
	for (long t0 = now_ms(); !vectrex_core_loaded(); ) {
		if (now_ms() - t0 >= wait_ms) {
			fprintf(stderr, "ptx: the Vectrex core is not running (see /tmp/CORENAME)\n");
			return -1;
		}
		struct timespec ts = { 0, 200 * 1000000L };
		nanosleep(&ts, NULL);
	}
	memfd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (memfd < 0) { perror("ptx: /dev/mem"); return -1; }
	void *p = mmap(0, PTX_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, PTX_DDR_BASE);
	if (p == MAP_FAILED) { perror("ptx: mmap"); close(memfd); return -1; }
	base = p;
	hdr  = (arm_hdr_t *)(base + 0x0000);
	st   = (core_status_t *)(base + 0x0040);
	in   = (volatile ptx_input_t *)(base + 0x0080);
	ring = (volatile uint64_t *)(base + 0x1000);
	aud  = (volatile uint64_t *)(base + 0x1000 + PTX_RING_N * 8u);
	/* Read-only until the core proves it is live with PiTrex Mode on. */
	long t0 = now_ms(), told = t0;
	last_seq = in->seq;
	for (;;) {
		struct timespec ts = { 0, 5 * 1000000L };
		nanosleep(&ts, NULL);
		if (core_signature() && in->seq != last_seq) break;
		if (!vectrex_core_loaded()) {
			fprintf(stderr, "ptx: the Vectrex core was unloaded\n");
			munmap((void *)base, PTX_MAP_SIZE); base = NULL; close(memfd); return -1;
		}
		if (now_ms() - told >= 5000) {
			told = now_ms();
			fprintf(stderr, "ptx: waiting for the core - turn on OSD > System > PiTrex Mode\n");
		}
	}
	last_seq = in->seq; last_seq_ms = last_name_ms = now_ms();
	wr = published = st->rd_idx;     /* resume wherever the core is */
	hdr->version = PTX_VERSION;
	hdr->log2n = PTX_RING_LOG2;
	hdr->wr_idx = wr;
	aud_wr = st->aud_rd & (2u * PTX_AUD_N - 1);   /* B6 cores: start the PCM ring where the core is */
	aud_nsamp = 0;
	hdr->aud_wr = aud_wr;
	hb_tick();
	__sync_synchronize();
	hdr->magic = PTX_MAGIC;          /* with a moving coded heartbeat: the core takes over */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	return 0;
}

static void m_push(uint64_t cmd)
{
	while (wr - st->rd_idx >= PTX_RING_N - PTX_PUBLISH) {   /* ring full: wait */
		if (published != wr) publish();
		nap_us(50);
	}
	ring[wr & (PTX_RING_N - 1)] = cmd;
	wr++;
	if (wr - published >= PTX_PUBLISH) publish();
}

static void m_frame(uint64_t stream_cycles)
{
	(void)stream_cycles;
	publish();
	check_core_alive();
	hb_tick();
	/* Stay at most PTX_LEAD_FRAMES frames ahead of what the core has fetched
	   (its FIFO is tiny, so fetched ~ played): enough slack for Linux
	   scheduling, little enough that input still feels immediate. */
	frame_end[n_frames % 8] = wr;
	n_frames++;
	if (n_frames <= PTX_LEAD_FRAMES) return;
	uint64_t target = frame_end[(n_frames - 1 - PTX_LEAD_FRAMES) % 8];
	int waited_ms = 0;
	while (st->rd_idx < target) {
		nap_us(200);
		if (++waited_ms == 10000)                /* 2 s without progress */
			fprintf(stderr, "ptx: core not fetching (rd_idx %llu < %llu) - PiTrex Mode on?\n",
			        (unsigned long long)st->rd_idx, (unsigned long long)target);
	}
}

static void m_input(ptx_input_t *o)
{
	o->seq = in->seq;
	if (o->seq == 0) {               /* core has not written an input block yet */
		o->pot[0] = o->pot[1] = o->pot[2] = o->pot[3] = 0;
		o->psg_ioa = 0xFF;            /* nothing pressed */
		return;
	}
	o->pot[0] = in->pot[0]; o->pot[1] = in->pot[1];   /* as the core reports them: up is */
	o->pot[2] = in->pot[2]; o->pot[3] = in->pot[3];   /* positive, as on a real Vectrex  */
	o->psg_ioa = in->psg_ioa;
}

static void m_status(ptx_status_t *o)
{
	o->valid = base != NULL;
	if (!base) return;
	o->rd_idx = st->rd_idx; o->core_cycles = st->core_cycles;
	o->frames = st->frames; o->underruns = st->underruns;
	o->core_build = st->build_magic == 0x43585450u ? (st->build & 0xFF) : 0;
}

static void m_close(void)
{
	if (!base) return;
	if (!core_lost) {
		publish();
		hdr->magic = 0;              /* core hands back to the 6809 at once */
	}
	munmap((void *)base, PTX_MAP_SIZE);
	close(memfd);
	base = NULL;
}

const ptx_backend_t ptx_backend_mister = { m_open, m_push, m_frame, m_input, m_status, m_close };

/* PCM audio (B6 cores): 16-bit mono at 32 kHz into the core's mixer. Samples are packed
 * four to a word (first in bits 15:0); the core reports the words it has fetched in its
 * status block, and its audio FIFO holds 64 words, so "fetched" is within 8 ms of
 * "played". About 100 ms is kept queued. The core's counters are 14 bits (twice the ring),
 * so the distance between them is taken modulo that. */
static uint32_t aud_queued(void)
{
	return (uint32_t)(aud_wr - st->aud_rd) & (2u * PTX_AUD_N - 1);
}

int ptx_audio_ready(void)
{
	return base && !core_lost && st->build_magic == 0x43585450u && (st->build & 0xFF) >= 6;
}

uint32_t ptx_audio_space(void)
{
	if (!ptx_audio_ready()) return 0;
	uint32_t queued = aud_queued();
	if (queued >= PTX_AUD_TARGET) return 0;
	return (uint32_t)(PTX_AUD_TARGET - queued) * 4u - aud_nsamp;
}

void ptx_audio_write(const int16_t *s, uint32_t n)
{
	if (!ptx_audio_ready()) return;
	while (n--) {
		aud_part |= (uint64_t)(uint16_t)*s++ << (16 * aud_nsamp);
		if (++aud_nsamp == 4) {
			if (aud_queued() < PTX_AUD_N - 16) {   /* else full: drop this word */
				aud[aud_wr & (PTX_AUD_N - 1)] = aud_part;
				aud_wr++;
			}
			aud_part = 0;
			aud_nsamp = 0;
		}
	}
	__sync_synchronize();           /* samples visible before the index */
	hdr->aud_wr = aud_wr;
}

#else  /* not Linux: keep the symbol so the harness links everywhere */
static int m_unavailable(void) { return -1; }
static void m_nop_push(uint64_t c) { (void)c; }
static void m_nop_frame(uint64_t c) { (void)c; }
static void m_nop_input(ptx_input_t *o) { (void)o; }
static void m_nop(void) {}
static void m_nop_status(ptx_status_t *o) { o->valid = 0; }
const ptx_backend_t ptx_backend_mister = { m_unavailable, m_nop_push, m_nop_frame, m_nop_input, m_nop_status, m_nop };
int      ptx_audio_ready(void) { return 0; }
uint32_t ptx_audio_space(void) { return 0; }
void     ptx_audio_write(const int16_t *s, uint32_t n) { (void)s; (void)n; }
#endif

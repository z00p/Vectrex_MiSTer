/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2bus.c -- see um2bus.h and DESIGN.md.
 *
 * Register layouts are the RP2350 datasheet / pico-sdk ones (hardware_regs pio.h, dma.h, sio.h).
 */
#define _GNU_SOURCE
#include "um2bus.h"
#include "um2sd.h"
#include "um2aud.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

/* UM2_BUSSTAT=1 (diagnostics): wait for and hold time of the bus lock per access class (printed with the stats at exit) */
static int g_busstat; static __thread uint64_t tl_bl_t0;
static struct { uint64_t n, wait, hold, max, big, bigw; } g_bst[12];
static const char *const g_bst_name[12] = { "sio store", "sio load", "dma store", "dma load", "spi", "qmi", "pio store", "pio load", "audio pump", "cap push", "aud_start lock", "irq_poke" };
static uint64_t bl_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#ifndef UM2BUS_NO_THREADS
static pthread_mutex_t g_bus = PTHREAD_MUTEX_INITIALIZER;   /* PIO, DMA, GPIO, sink */
static pthread_mutex_t g_misc = PTHREAD_MUTEX_INITIALIZER;  /* SIO FIFO */
#define BUS_LOCK()   pthread_mutex_lock(&g_bus)
#define BUS_UNLOCK() pthread_mutex_unlock(&g_bus)
#define BUS_LOCK_T(tag) do { if (g_busstat) { uint64_t a_ = bl_ns(); pthread_mutex_lock(&g_bus); tl_bl_t0 = bl_ns(); g_bst[tag].wait += tl_bl_t0 - a_; if (tl_bl_t0 - a_ > 100000) g_bst[tag].bigw++; } else pthread_mutex_lock(&g_bus); } while (0)
#define BUS_UNLOCK_T(tag) do { if (g_busstat) { uint64_t h_ = bl_ns() - tl_bl_t0; g_bst[tag].n++; g_bst[tag].hold += h_; if (h_ > g_bst[tag].max) g_bst[tag].max = h_; if (h_ > 100000) g_bst[tag].big++; } pthread_mutex_unlock(&g_bus); } while (0)
#define MISC_LOCK()  pthread_mutex_lock(&g_misc)
#define MISC_UNLOCK() pthread_mutex_unlock(&g_misc)
#else
#define BUS_LOCK()
#define BUS_UNLOCK()
#define BUS_LOCK_T(tag)
#define BUS_UNLOCK_T(tag)
#define MISC_LOCK()
#define MISC_UNLOCK()
#endif

static um2bus_cfg_t C;
static FILE *g_cap;
static int g_replaying;

static void vlog(const char *fmt, ...) {
	if (!C.verbose) return;
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------ capture records */
typedef struct { uint8_t op, size; uint16_t pad; uint32_t addr, val; } CapRec;
enum { CAP_PIO_ST = 0, CAP_PIO_PUSH = 1, CAP_SIO_ST = 2, CAP_SIO_GPIN = 3 };
static void cap(int op, int size, uint32_t addr, uint32_t val) {
	if (!g_cap || g_replaying) return;
	CapRec r = { (uint8_t)op, (uint8_t)size, 0, addr, val };
	fwrite(&r, sizeof r, 1, g_cap);
}

/* ------------------------------------------------------------------ statistics */
static struct {
	uint64_t pushes, wr_words, silence_words, park_words, park_cycles, latches_write, latches_other;
	uint64_t sio_writes, sio_reads, sio_edges, frames, forced_frames, dma_runs, dma_words, pio_instrs;
	uint64_t idle_pad_cycles, non_via_writes, unsupported;
	uint64_t frame_cycles_min, frame_cycles_max, frame_cycles_sum;
} S;

/* ------------------------------------------------------------------ sink helpers (bus lock held) */
static uint64_t g_fstart;           /* sink time at the last frame mark */
static int      g_fstarted;

static void frame_mark(int forced);

__attribute__((weak)) uint64_t g_trap_ns, g_trap_n;   /* the loader owns them */
static double g_tw, g_tf, g_wall0, g_wall1; static uint64_t g_frames0;      /* seconds spent in sink writes / ptx_frame (profiling, um2bus_stats) */
static double cpu_s(clockid_t id) { struct timespec t; clock_gettime(id, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static inline double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
/* ------------------------------------------------------------------ bus-cycle decode */
/* UVM2 board pin map (uvm2_bus.h): D0-7 = GP0-7, A0-A13 = GP8-21, A14 = GP24, A15 = GP25, R/W = GP26 */
#define PIN_RW   (1u << 26)
#define PIN_A15  (1u << 25)
static inline uint32_t pins_addr(uint32_t gp) { return ((gp >> 8) & 0x3FFFu) | (((gp >> 24) & 1u) << 14) | (((gp >> 25) & 1u) << 15); }

/* ------------------------------------------------------------------ PIO */
#define NPIO 3
#define FIFO_CAP 65536
typedef struct {
	uint32_t clkdiv, execctrl, shiftctrl, pinctrl, instr_last;
	/* interpreter (only PIO0 SM0 is interpreted) */
	uint32_t pc, x, y, osr, isr, osr_cnt;
	uint64_t t;                 /* PIO cycles, 100 per E period */
	uint32_t pins, pindirs;
	int wr_active;              /* the pin state, if latched at an E boundary, is a bus write */
	int enabled, idle;          /* idle: starved at `pull noblock` (bus parked) */
	uint32_t *fifo; uint32_t head, count;
	uint64_t sink_idx; int started;
	int fast;                   /* canonical bus_stream program, starved at its pull noblock: words are decoded directly */
} Sm;
typedef struct { uint32_t ctrl, gpiobase; uint16_t imem[32]; Sm sm[4]; uint32_t sh[0x200 / 4]; } Pio;
static Pio g_pio[NPIO];
#define SIMPIO (&g_pio[0].sm[0])

static const uint32_t PIO_BASE[NPIO] = { 0x50200000u, 0x50300000u, 0x50400000u };

static void sm_run(Sm *s);
static int prog_canonical(const Sm *s);

/* E-period index accounting: a write latched at boundary `idx` is the last cycle of the gap */
static void sink_sync_to(Sm *s, uint64_t idx) {            /* idle E cycles up to idx (no write) */
	if (!s->started) { s->sink_idx = idx; s->started = 1; return; }
	if (idx > s->sink_idx) { uint64_t d = idx - s->sink_idx; while (d) { uint32_t c = d > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)d; C.advance(c); d -= c; } s->sink_idx = idx; }
}

static void do_latch(Sm *s, uint64_t idx) {
	uint32_t gp = s->pins, addr = pins_addr(gp);
	if (!s->started) { s->sink_idx = idx - 1; s->started = 1; }
	if ((addr & 0xF000u) != 0xD000u) { S.non_via_writes++; return; }   /* stray write outside the VIA: not forwarded */
	if (idx > s->sink_idx + 1) sink_sync_to(s, idx - 1);
	if (C.verbose > 1) { double t0 = tnow(); C.vwrite((uint16_t)addr, (uint8_t)(gp & 0xFF)); g_tw += tnow() - t0; } else C.vwrite((uint16_t)addr, (uint8_t)(gp & 0xFF));
	s->sink_idx = idx;
	S.latches_write++;
}

static void credit_add(uint64_t n);
static inline void tick_to(Sm *s, uint64_t tn) {
	if (tn / 100 > s->t / 100) credit_add(tn / 100 - s->t / 100);
	if (s->wr_active) {
		uint64_t b = (s->t / 100 + 1) * 100;
		for (; b <= tn; b += 100) do_latch(s, b / 100);
	}
	s->t = tn;
}

static void set_pins(Sm *s, uint32_t val, unsigned count, unsigned base) {
	if (count == 0) return;
	uint32_t mask = (count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u));
	uint32_t v = (val & mask) << base, m = mask << base;
	s->pins = (s->pins & ~m) | (v & m);
	/* a write cycle: R/W pin low and driven, A15 driven */
	s->wr_active = ((s->pindirs & PIN_RW) && (s->pindirs & PIN_A15) && !(s->pins & PIN_RW)) ? 1 : 0;
}

static void set_pindirs(Sm *s, uint32_t val, unsigned count, unsigned base) {
	uint32_t mask = (count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u));
	uint32_t m = mask << base;
	s->pindirs = (s->pindirs & ~m) | ((val & mask) << base);
	s->wr_active = ((s->pindirs & PIN_RW) && (s->pindirs & PIN_A15) && !(s->pins & PIN_RW)) ? 1 : 0;
}

static inline uint32_t sm_outcount(const Sm *s) { return (s->pinctrl >> 20) & 31; }
static inline uint32_t sm_outbase(const Sm *s) { return s->pinctrl & 31; }

static uint32_t osr_take(Sm *s, unsigned n) {
	if (n == 0) n = 32;
	uint32_t v;
	if (s->shiftctrl & (1u << 19)) { v = (n == 32) ? s->osr : (s->osr & ((1u << n) - 1u)); s->osr = (n == 32) ? 0 : (s->osr >> n); }
	else { v = (n == 32) ? s->osr : (s->osr >> (32 - n)); s->osr = (n == 32) ? 0 : (s->osr << n); }
	s->osr_cnt += n; if (s->osr_cnt > 32) s->osr_cnt = 32;
	return v;
}

/* one instruction. Returns 0 = done (time advanced), 1 = stalled with no data (state unchanged) */
static int sm_exec(Sm *s, uint16_t ins, int forced) {
	unsigned op = ins >> 13, delay_bits, delay;
	unsigned nss = (s->pinctrl >> 29) & 7;
	delay_bits = 5 - nss; delay = (ins >> 8) & ((1u << delay_bits) - 1u);
	unsigned arg = ins & 0xFF;
	uint32_t next_pc = s->pc + 1;
	uint64_t tn = s->t;                    /* completion time of the stall, if any */
	S.pio_instrs++;
	switch (op) {
	case 0: {                              /* JMP */
		unsigned cond = (arg >> 5) & 7, addr = arg & 31; int take = 0;
		switch (cond) {
		case 0: take = 1; break;
		case 1: take = !s->x; break;
		case 2: take = s->x != 0; s->x--; break;
		case 3: take = !s->y; break;
		case 4: take = s->y != 0; s->y--; break;
		case 5: take = s->x != s->y; break;
		case 6: take = 0; break;           /* pin: not modelled */
		case 7: take = s->osr_cnt < ((((s->shiftctrl >> 25) & 31) ? ((s->shiftctrl >> 25) & 31) : 32)); break;
		}
		if (take) next_pc = addr;
		break; }
	case 1: {                              /* WAIT */
		unsigned pol = (arg >> 7) & 1, src = (arg >> 5) & 3, idx = arg & 31;
		if (src == 0 && idx == 31) {       /* GPIO31: the clock, high for the first half of each 100-cycle period */
			uint64_t ph = s->t % 100;
			if (pol) { if (ph >= 50) tn = (s->t / 100 + 1) * 100; }
			else { if (ph < 50) tn = (s->t / 100) * 100 + 50; }
		} else { S.unsupported++; vlog("um2bus: wait src=%u idx=%u not modelled (treated as satisfied)\n", src, idx); }
		break; }
	case 3: {                              /* OUT */
		unsigned dst = (arg >> 5) & 7, n = arg & 31;
		if (s->shiftctrl & (1u << 17)) { S.unsupported++; vlog("um2bus: autopull not modelled\n"); }
		uint32_t v = osr_take(s, n);
		switch (dst) {
		case 0: tick_to(s, s->t); set_pins(s, v, n ? n : 32, sm_outbase(s)); break;
		case 1: s->x = v; break;
		case 2: s->y = v; break;
		case 3: break;
		case 4: set_pindirs(s, v, n ? n : 32, sm_outbase(s)); break;
		case 5: next_pc = v & 31; break;
		case 6: s->isr = v; break;
		case 7: S.unsupported++; vlog("um2bus: out exec not modelled\n"); break;
		}
		break; }
	case 4: {                              /* PUSH / PULL */
		if (!(arg & 0x80)) { S.unsupported++; vlog("um2bus: push not modelled\n"); break; }
		int if_empty = (arg >> 6) & 1, block = (arg >> 5) & 1;
		unsigned thr = (s->shiftctrl >> 25) & 31; if (!thr) thr = 32;
		if (if_empty && s->osr_cnt < thr) break;
		if (s->count) {
			s->osr = s->fifo[s->head]; s->head = (s->head + 1) % FIFO_CAP; s->count--; s->osr_cnt = 0;
		} else if (!block) {
			if (!forced) {                                /* starved: the bus parks (no time passes in the stream) */
				set_pins(s, s->x >> 1, sm_outcount(s) ? sm_outcount(s) : 32, sm_outbase(s));
				s->idle = 1; return 1;
			}
			s->osr = s->x; s->osr_cnt = 0;
		} else return 1;                                  /* pull block: wait for data */
		break; }
	case 5: {                              /* MOV */
		unsigned dst = (arg >> 5) & 7, mop = (arg >> 3) & 3, src = arg & 7; uint32_t v = 0;
		switch (src) { case 0: v = s->pins; break; case 1: v = s->x; break; case 2: v = s->y; break; case 3: v = 0; break;
			case 5: v = 0; break; case 6: v = s->isr; break; case 7: v = s->osr; break; default: break; }
		if (mop == 1) v = ~v; else if (mop == 2) { uint32_t r = 0; for (int i = 0; i < 32; i++) if (v >> i & 1) r |= 1u << (31 - i); v = r; }
		switch (dst) {
		case 0: tick_to(s, s->t); set_pins(s, v, sm_outcount(s) ? sm_outcount(s) : 32, sm_outbase(s)); break;
		case 1: s->x = v; break;
		case 2: s->y = v; break;
		case 3: set_pindirs(s, v, sm_outcount(s) ? sm_outcount(s) : 32, sm_outbase(s)); break;
		case 4: S.unsupported++; vlog("um2bus: mov exec not modelled\n"); break;
		case 5: next_pc = v & 31; break;
		case 6: s->isr = v; break;
		case 7: s->osr = v; s->osr_cnt = 0; break;
		}
		break; }
	case 7: {                              /* SET */
		unsigned dst = (arg >> 5) & 7, data = arg & 31;
		unsigned sc = (s->pinctrl >> 26) & 7, sb = (s->pinctrl >> 5) & 31;
		switch (dst) {
		case 0: tick_to(s, s->t); set_pins(s, data, sc, sb); break;
		case 1: s->x = data; break;
		case 2: s->y = data; break;
		case 4: set_pindirs(s, data, sc, sb); break;
		default: break;
		}
		break; }
	default:
		S.unsupported++; vlog("um2bus: PIO opcode %u (%04x) not modelled\n", op, ins);
		break;
	}
	tick_to(s, tn);
	if (forced) {
		/* SMx_INSTR: executes now; only a jump moves the PC */
		if (op == 0 || (op == 3 && ((arg >> 5) & 7) == 5) || (op == 5 && ((arg >> 5) & 7) == 5)) s->pc = next_pc;
		s->t += 1;
		return 0;
	}
	tick_to(s, tn + 1 + delay);
	/* wrap */
	if (s->pc == ((s->execctrl >> 12) & 31) && next_pc == s->pc + 1) next_pc = (s->execctrl >> 7) & 31;
	s->pc = next_pc;
	return 0;
}

static void sm_run(Sm *s) {
	if (!s->enabled) return;
	uint64_t guard = 0;
	s->idle = 0;
	/* the interpreter's pc points into the program image of PIO0 */
	for (;;) {
		if (++guard > 400000000ull) { vlog("um2bus: PIO run guard hit\n"); break; }
		uint16_t ins = g_pio[0].imem[s->pc & 31];
		if (sm_exec(s, ins, 0)) break;
	}
	s->fast = (s->idle && s->pc == 7 && s->count == 0 && prog_canonical(s)) ? 1 : 0;
	if (s->idle) {
		/* hardware would now loop `pull noblock -> X -> out pins`: the bus is parked */
		uint32_t xv = s->x >> 1;           /* park word keeps its sentinel in bit 0 */
		(void)xv;
	}
}

/* The program the UVMC2 runtime uploads (vectrex-bus/src/bus_stream.pio, public SDK source; `out` widths patched to 27).
 * When PIO0 SM0 holds exactly this program the interpreter's behaviour per word is known in closed form (see DESIGN.md):
 *   write word   -> one E period, VIA write latched at its end      park N   -> N periods, nothing latched
 *   silence word -> one E period, nothing latched (no repeat: the interpreter starves between pushed words)
 * and the interpreter is skipped (it costs ~40 PIO instructions per word, too slow for the DE10's A9). */
/* recognised by an FNV-1a hash of its 25 instruction words (low byte first) rather than a copy of them */
#define CANON_LEN 25
#define CANON_FNV 0x5106bba9u
static uint32_t prog_fnv(const uint16_t *w, int n) {
	uint32_t h = 0x811c9dc5u;
	for (int i = 0; i < n; i++) { h = (h ^ (w[i] & 0xFFu)) * 0x01000193u; h = (h ^ (w[i] >> 8)) * 0x01000193u; }
	return h;
}
static uint32_t g_credit;
static unsigned g_frames_sm;            /* frame marks caused by PIO SM enables (the PIO-stream images) */
static void credit_add(uint64_t n) { g_credit += n > 6000 ? 6000 : (uint32_t)n; if (g_credit > 6000) g_credit = 6000; }
static int g_nofast;
static int g_edge_time = 1;
static int prog_canonical(const Sm *s) {
	if (g_nofast) return 0;
	if (prog_fnv(g_pio[0].imem, CANON_LEN) != CANON_FNV) return 0;
	return (s->clkdiv >> 16) >= 1 && (s->clkdiv & 0xFFFFu) == 0 && s->execctrl == 0x00018200u && (s->shiftctrl & ((1u << 17) | (1u << 19))) == (1u << 19)
		&& sm_outcount(s) == 27 && sm_outbase(s) == 0;
}
static void fp_word(Sm *s, uint32_t w) {
	uint64_t idx = s->t / 100;
	if (w & 1) {
		set_pins(s, w >> 1, 27, 0);
		idx += 1;
		if (s->wr_active) do_latch(s, idx);
		set_pins(s, s->x >> 1, 27, 0);                  /* starved: parked */
	} else if (w & 2) {
		idx += ((w >> 2) & 0xFFFFFFu) + 1;
		set_pins(s, s->x >> 1, 27, 0);
	} else idx += 1;
	credit_add(idx - s->t / 100);
	s->t = idx * 100 + 17;
}

static void frame_mark(int forced);
static void sm_push(Sm *s, uint32_t w) {
	S.pushes++;
	if (C.verbose && S.pushes <= 40) vlog("um2bus: push #%llu %08x%s\n", (unsigned long long)S.pushes, w, s->enabled ? "" : " (SM off)");
	if (s->enabled && s->fast && !s->count) {
		if (w & 1) S.wr_words++; else if (w & 2) { S.park_words++; S.park_cycles += ((w >> 2) & 0xFFFFFFu) + 1; } else S.silence_words++;
		fp_word(s, w);
		if (C.frame_cycles && g_fstarted && (S.pushes & 1023) == 0 && C.now() - g_fstart > 5ull * C.frame_cycles) frame_mark(1);
		return;
	}
	if (s->count >= FIFO_CAP) { S.unsupported++; vlog("um2bus: TX FIFO overflow\n"); return; }
	s->fifo[(s->head + s->count) % FIFO_CAP] = w;
	s->count++;
	if (w & 1) S.wr_words++; else if (w & 2) { S.park_words++; S.park_cycles += (w >> 2) + 1; } else S.silence_words++;
	if (s->enabled) sm_run(s);
}

/* ---- UVM2 bus engine v2 (UVMC2 BattleZone) --------------------------------------------------------------------------
 * PIO0 SM0/SM1/SM2 + PIO1 SM0/SM1 (the upper address lines are constant on this board: the VIA sits at $D000 and the 12 `out pins`
 * bits are D0..D7 + A0..A3).  Evidence: the words the image pushes (0x00010000, 0x0003ff00, 0x00029f00, 0x000ccc00, 0x000b6000 ... the
 * VIA setup DDRA=ff DDRB=9f PCR=cc ACR=60) and the program (SM0: pull; out y,1; out null,7; wait E; `out pins,12` with R/W from the side-set
 * (read: R/W high + `irq 0` so that SM2 samples D0..D7 with `in pins,8`; write: R/W low); then `out y,12` and a y-- loop of E periods).
 *   TX word (SM0, shift right):  bit 0 = read, bits 8..15 = D0..D7, bits 16..19 = A0..A3 (VIA register), bits 20..31 = idle E periods after
 *   RX word (SM2 RXF2):          the byte read, in bits 0..7
 * Timing: one E period for the access, plus `delay` more (HYPOTHESIS: the list builder's {delay, reg, data} maps 1:1 as in v1).  The interpreter
 * is not run for this program (it would need the IRQ flags and four cooperating SMs); the words are decoded directly. */
static const uint8_t V2IDX[] = { 16, 17, 18, 21, 23, 25, 27, 31 };   /* instruction slots sampled; their words hash to V2FNV (FNV-1a, low byte first) */
#define V2FNV 0xf8f2faaau
static int g_v2;                                          /* PIO0 holds the v2 engine */
static uint32_t g_v2_rx[16]; static unsigned g_v2_rn;     /* SM2 RX FIFO */
static unsigned long long g_v2_words, g_v2_reads, g_v2_cycles;
static uint8_t g_v2_last[16];                            /* last value written per VIA register (log only) */
static int v2_detect(void) {
	uint16_t w[sizeof V2IDX];
	for (unsigned i = 0; i < sizeof V2IDX; i++) w[i] = g_pio[0].imem[V2IDX[i]];
	if (prog_fnv(w, sizeof V2IDX) != V2FNV) return 0;
	return ((g_pio[0].sm[0].shiftctrl & 0x400c0000u) == 0x400c0000u) && !getenv("UM2BUS_NOV2");
}
static void frame_mark(int forced);
static void v2_word(uint32_t w) {
	unsigned rd = w & 1, reg = (w >> 16) & 0xF, data = (w >> 8) & 0xFF, n = w >> 20;
	g_v2_words++;
	if (rd) {
		if (C.frame_cycles && !g_fstarted) { g_fstarted = 1; g_fstart = C.now(); }
		/* start of a between-frames window (controllers): the frame mark, as for the SIO images */
		else if (C.frame_cycles && C.now() - g_fstart >= 3000) frame_mark(0);
		uint8_t v = C.vread((uint16_t)(0xD000u | reg));
		if (g_v2_rn < 16) g_v2_rx[g_v2_rn++] = v;
		g_v2_reads++;
		if (C.verbose > 1) { static unsigned shown; if (shown++ < (getenv("UM2_V2LOG") ? 20000u : 300u)) vlog("um2bus: v2 read reg %x -> %02x (t=%llu) orb=%02x ora=%02x\n", reg, v, (unsigned long long)C.now(), g_v2_last[0], g_v2_last[1]); }
	} else {
		if (!g_fstarted) { g_fstarted = 1; g_fstart = C.now(); }
		C.vwrite((uint16_t)(0xD000u | reg), (uint8_t)data);
		g_v2_last[reg] = (uint8_t)data;
		S.latches_write++;
	}
	if (n) { C.advance(n); }
	g_v2_cycles += 1u + n;
	if (C.frame_cycles && g_fstarted && (g_v2_words & 255) == 0 && C.now() - g_fstart > 5ull * C.frame_cycles) frame_mark(1);
}

static struct { uint64_t cpu_words, starts, blocks, irqs, steps, words, aborts, late[5], maxlate; double fps[NPIO][4]; int bits[NPIO][4]; } AS;   /* cart audio statistics */
static void pio_txf_push(int pio, int smn, uint32_t w) {
	cap(CAP_PIO_PUSH, 4, (uint32_t)(pio * 4 + smn), w);
	if (pio == 0 && smn == 0) { if (g_v2 && (g_pio[0].ctrl & 1)) { S.pushes++; v2_word(w); return; } sm_push(SIMPIO, w); }
	/* the other state machines (PIO1/PIO2: the jack's I2S output, codec control) are not interpreted.  Cart audio arrives by DMA (paced, see
	 * "cart audio" below); words the CPU pushes (BattleZone's SM3 codec control, SM2 data would be a new case) are only counted. */
	else if (pio > 0) {
		if (AS.cpu_words++ < 8 && C.verbose) vlog("um2bus: CPU push to PIO%d TXF%d: %08x (not modelled)\n", pio, smn, w);
	}
}

static void sm_enable_changed(int pio, int smn, int on) {
	Sm *s = &g_pio[pio].sm[smn];
	int was = s->enabled; s->enabled = on;
	if (pio > 0 && on && !was && C.verbose) {
		vlog("um2bus: PIO%d SM%d enabled: clkdiv=%08x execctrl=%08x shiftctrl=%08x pinctrl=%08x\n", pio, smn, s->clkdiv, s->execctrl, s->shiftctrl, s->pinctrl);
		unsigned top = (s->execctrl >> 12) & 31, bot = (s->execctrl >> 7) & 31;
		for (unsigned i = bot; i <= top && i < 32; i++) { char b[64]; vlog("  imem[%2u] = %04x  %s\n", i, g_pio[pio].imem[i], um2bus_pio_dis(g_pio[pio].imem[i], b, sizeof b)); }
	}
	if (pio == 0 && smn == 0 && on && !was) { g_v2 = v2_detect(); if (g_v2) vlog("um2bus: UVM2 bus engine v2 recognised (direct word decode)\n"); }
	if (pio == 0 && smn == 0 && g_v2) return;
	if (pio == 0 && smn == 0 && on && !was) {
		static int shown;
		if (!shown) {
			shown = 1;
			vlog("um2bus: first enable: clkdiv=%08x execctrl=%08x shiftctrl=%08x pinctrl=%08x pc=%u, %u queued, pindirs=%08x\n", s->clkdiv, s->execctrl, s->shiftctrl, s->pinctrl, s->pc, s->count, s->pindirs);
			for (int i = 0; i < 32; i++) { char b[64]; vlog("  imem[%2d] = %04x  %s\n", i, g_pio[0].imem[i], um2bus_pio_dis(g_pio[0].imem[i], b, sizeof b)); }
		} else if (C.verbose > 1) vlog("um2bus: PIO0 SM0 enabled (pc=%u, %u queued)\n", s->pc, s->count);
		/* a new list starts here: frame mark (not for the very first enable, the preamble comes next) */
		if (g_fstarted) { g_frames_sm++; frame_mark(0); } else { g_fstarted = 1; g_fstart = C.now(); }
		sm_run(s);
	}
}

static uint32_t pio_fstat(int pio) {
	uint32_t v = (0xFu << 24) | (0xFu << 8);  /* TXEMPTY and RXEMPTY for all four, nothing full */
	if (pio == 0 && g_v2_rn) v &= ~(1u << (8 + 2));    /* SM2 has read data */
	return v;
}

static void pio_sm_store(int pio, int smn, unsigned reg, uint32_t v) {
	Sm *s = &g_pio[pio].sm[smn];
	switch (reg) {
	case 0x00: s->clkdiv = v; break;
	case 0x04: s->execctrl = v; break;
	case 0x08: s->shiftctrl = v; break;
	case 0x0C: break;                       /* ADDR read-only */
	case 0x10: s->instr_last = v;
		if (pio == 0 && smn == 0) {
			uint16_t ins = (uint16_t)v;
			/* `jmp 0` is how vbus_install restarts the program.  The images push their first list (22 words) before this
			 * point; real hardware can only have taken 4 of them into the TX FIFO, and they would be eaten as the
			 * preamble (junk pin directions / park pattern).  Dropping them gives the intended preamble. */
			if (ins == 0x0000 && s->count) { if (C.verbose) vlog("um2bus: jmp 0 with %u stale words in the TX FIFO: dropped\n", s->count); s->head = 0; s->count = 0; }
			sm_exec(s, ins, 1);
		}
		break;
	case 0x14: s->pinctrl = v; break;
	}
}

static int pio_store(int pio, uint32_t off, int size, uint32_t val) {
	Pio *p = &g_pio[pio];
	unsigned alias = (off >> 12) & 3; off &= 0xFFF;
	if (!(off >= 0x10 && off < 0x20) || alias) cap(CAP_PIO_ST, size, PIO_BASE[pio] + (alias << 12) + off, val);
	/* byte/half stores to the TX FIFO are replicated by hardware; the runtime only uses words */
	if (off >= 0x10 && off < 0x20 && alias == 0) { pio_txf_push(pio, (off - 0x10) >> 2, val); return 1; }
	/* atomic aliases: read-modify-write on the readable registers we hold */
	if (alias) {
		uint32_t cur;
		if (off == 0x00) cur = p->ctrl;
		else if (off >= 0xC8 && off < 0xC8 + 0x18 * 4) { Sm *s = &p->sm[(off - 0xC8) / 0x18]; unsigned r = (off - 0xC8) % 0x18; cur = r == 0 ? s->clkdiv : r == 4 ? s->execctrl : r == 8 ? s->shiftctrl : r == 0x14 ? s->pinctrl : 0; }
		else cur = p->sh[off >> 2];
		val = alias == 1 ? cur ^ val : alias == 2 ? cur | val : cur & ~val;
		if (off >= 0xC8 && off < 0xC8 + 0x18 * 4 && ((off - 0xC8) % 0x18) == 0x10) return 1;   /* no atomic exec */
	}
	if (pio == 0) SIMPIO->fast = 0;   /* any configuration change: re-evaluate when the SM next starves */
	if (off == 0x00) {
		uint32_t old = p->ctrl;
		p->ctrl = val & 0xF;
		if (val & 0xF0) {                   /* SM_RESTART: clears OSR/ISR/counters, pc keeps */
			for (int i = 0; i < 4; i++) if (val & (0x10u << i)) { Sm *s = &p->sm[i]; s->osr = s->isr = 0; s->osr_cnt = 32; s->idle = 0; }
		}
		for (int i = 0; i < 4; i++) if (((old ^ p->ctrl) >> i) & 1) sm_enable_changed(pio, i, (p->ctrl >> i) & 1);
		return 1;
	}
	if (off >= 0x48 && off < 0xC8) { p->imem[(off - 0x48) >> 2] = (uint16_t)val; return 1; }
	if (off >= 0xC8 && off < 0xC8 + 0x18 * 4) { pio_sm_store(pio, (off - 0xC8) / 0x18, (off - 0xC8) % 0x18, val); return 1; }
	if (off < 0x200) { p->sh[off >> 2] = val; return 1; }
	return 0;
}

static int pio_load(int pio, uint32_t off, int size, uint32_t *v) {
	(void)size;
	Pio *p = &g_pio[pio];
	off &= 0xFFF;
	if (off == 0x00) { *v = p->ctrl; return 1; }
	if (off == 0x04) { *v = pio_fstat(pio); return 1; }
	if (off == 0x0C) { *v = (pio == 0) ? (g_v2_rn << 8) : 0; return 1; }       /* FLEVEL: RX level of SM2 */
	if (off == 0x44) { *v = (4u << 0) | (4u << 8) | (32u << 16); return 1; }   /* DBG_CFGINFO: fifo depth 4, 4 SMs, 32 instrs */
	if (off == 0x28 && pio == 0 && g_v2_rn) { *v = g_v2_rx[0]; memmove(g_v2_rx, g_v2_rx + 1, (size_t)(--g_v2_rn) * sizeof g_v2_rx[0]); return 1; }
	if (off >= 0x20 && off < 0x30) { *v = 0; return 1; }                      /* RX FIFOs empty */
	if (off >= 0x48 && off < 0xC8) { *v = p->imem[(off - 0x48) >> 2]; return 1; }
	if (off >= 0xC8 && off < 0xC8 + 0x18 * 4) {
		Sm *s = &p->sm[(off - 0xC8) / 0x18]; unsigned r = (off - 0xC8) % 0x18;
		*v = r == 0 ? s->clkdiv : r == 4 ? s->execctrl : r == 8 ? s->shiftctrl : r == 0xC ? (pio == 0 && (off - 0xC8) / 0x18 == 0 ? s->pc : 0) : r == 0x10 ? s->instr_last : s->pinctrl;
		return 1;
	}
	if (off < 0x200) { *v = p->sh[off >> 2]; return 1; }
	return 0;
}

/* ------------------------------------------------------------------ DMA */
typedef struct { uint32_t read, write, count, ctrl; unsigned busy_polls; } Dch;
static Dch g_dma[16];
static uint32_t g_dma_sh[0x400];

static int guest_read(uint32_t a, int size, uint32_t *v) {
	uint8_t *p = C.host_ptr ? C.host_ptr(a, (uint32_t)size) : NULL;
	if (p) { uint32_t t = 0; memcpy(&t, p, (size_t)size); *v = t; return 1; }
	*v = C.ext_load ? C.ext_load(a, size) : 0;
	return 0;
}
static void guest_write(uint32_t a, int size, uint32_t v) {
	uint8_t *p = C.host_ptr ? C.host_ptr(a, (uint32_t)size) : NULL;
	if (p) { memcpy(p, &v, (size_t)size); return; }
	/* PIO TX FIFO? */
	for (int i = 0; i < NPIO; i++)
		if (a >= PIO_BASE[i] + 0x10 && a < PIO_BASE[i] + 0x20) {
			uint32_t w = size == 4 ? v : size == 2 ? (v & 0xFFFF) * 0x10001u : (v & 0xFF) * 0x01010101u;
			pio_txf_push(i, (int)(a - PIO_BASE[i] - 0x10) >> 2, w);
			return;
		}
	if (C.ext_store) C.ext_store(a, size, v);
}

/* ---- guest-visible DMA register page (optional, see um2bus_cfg_t.dma_mirror)
 * The runtime polls DMA CTRL_TRIG (BUSY) once per pushed bus word: 4700 SIGSEGV traps per frame.  With the page mapped
 * read-only at 0x50000000 over a shared memory alias, reads cost nothing and only the stores trap.  The model keeps the page
 * up to date; BUSY is set on a PIO-TX trigger and cleared ~25 us later by a helper thread (the DMA is paced by the SM on
 * hardware, so words accumulate into a batch meanwhile). */
static volatile uint32_t *g_mir;
static pthread_mutex_t g_busy_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_busy_c = PTHREAD_COND_INITIALIZER;
static int g_busy_req;
static volatile uint32_t g_busy_mask;
static volatile uint32_t g_aud_busy;     /* channels with a paced cart-audio transfer running */
static void dma_sync(int ch) {
	if (!g_mir) return;
	Dch *d = &g_dma[ch];
	volatile uint32_t *m = g_mir + ch * 16;
	uint32_t c = d->ctrl | (((g_busy_mask | g_aud_busy) >> ch & 1) ? (1u << 26) : 0);
	m[0] = d->read; m[1] = d->write; m[2] = d->count; m[3] = c;
	m[4] = c; m[5] = d->read; m[6] = d->write; m[7] = d->count;
	m[8] = c; m[9] = d->count; m[10] = d->read; m[11] = d->write;
	m[12] = c; m[13] = d->write; m[14] = d->count; m[15] = d->read;
}
__attribute__((weak)) void um2_rt_boost(int delta) { (void)delta; }   /* um2run overrides it */
static void *busy_thread(void *arg) {
	(void)arg;
	pthread_setname_np(pthread_self(), "u2busy");
	um2_rt_boost(2);
	for (;;) {
		pthread_mutex_lock(&g_busy_m);
		while (!g_busy_req) pthread_cond_wait(&g_busy_c, &g_busy_m);
		g_busy_req = 0;
		pthread_mutex_unlock(&g_busy_m);
		struct timespec ts = { 0, 25000 }; nanosleep(&ts, NULL);
		uint32_t m = __sync_fetch_and_and(&g_busy_mask, 0);
		for (int ch = 0; ch < 16; ch++) if (m >> ch & 1) {
			volatile uint32_t *w = g_mir + ch * 16;
			w[3] &= ~(1u << 26); w[4] &= ~(1u << 26); w[8] &= ~(1u << 26); w[12] &= ~(1u << 26);
		}
	}
	return NULL;
}
static void busy_request(void) {
	pthread_mutex_lock(&g_busy_m); g_busy_req = 1; pthread_cond_signal(&g_busy_c); pthread_mutex_unlock(&g_busy_m);
}

/* ---- SPI0 (PL022) behind the SD card; its DREQs (RP2350: 24 = SPI0 TX, 25 = SPI0 RX) pair two DMA channels as in carlk3's spi_transfer():
 * both are started by one MULTI_CHAN_TRIGGER write.  The TX channel clocks bytes out (memory -> DR) and the card's answers wait in a queue
 * that the RX channel (DR -> memory) drains; TX channels run before RX channels, whatever their numbers. */
static struct { uint32_t cr1; uint8_t rx[8]; int rxn; } g_spi0;
static uint8_t g_spidq[1 << 16]; static unsigned g_spidn;
static uint8_t spi_xfer_byte(uint8_t b) { return ((g_spi0.cr1 & 2) && um2sd_present()) ? um2sd_xfer(b) : 0xFF; }
static void spi_dma_run(Dch *d, unsigned treq, uint32_t n, int ir, int iw) {
	for (uint32_t i = 0; i < n; i++) {
		if (treq == 24) {
			uint32_t v; guest_read(d->read, 1, &v);
			uint8_t r = spi_xfer_byte((uint8_t)v);
			if (g_spidn < sizeof g_spidq) g_spidq[g_spidn++] = r;
			if (ir) d->read++;
		} else {
			uint8_t r = 0xFF;
			if (g_spidn) { r = g_spidq[0]; if (--g_spidn) memmove(g_spidq, g_spidq + 1, g_spidn); }
			guest_write(d->write, 1, r);
			if (iw) d->write++;
		}
	}
}
/* ---- cart audio ---------------------------------------------------------------------------------------------------------------------------
 * The jack output of a UVMC2 image is a PIO2 state machine running a 16-bit I2S program (data GPIO27, LRCLK GPIO28, BCLK GPIO29: out pins,1 with a
 * 2-bit side-set, 64 PIO cycles per stereo frame, shiftctrl = autopull at 32 bits, MSB first: one 32-bit TX word = one frame, left in bits 31:16).
 * A DMA channel with TREQ = DREQ_PIO2_TXn feeds it.  The SDK audio module (Star Wars, Pac-Man, Punch-Out, Veccy Road) runs ONE endless transfer over an
 * 8 KB ring and finds its fill level from the channel's READ_ADDR; BattleZone runs 96 half-word blocks (ping-pong) and refills in the DMA_IRQ_0 handler.
 * The model makes such a channel a paced transfer instead of a synchronous one: the frame rate is clk_sys / PIO clkdiv / 64 (the image calibrates clk_sys
 * from the E clock and sets clkdiv so that the SDK module gets 2.048 MHz, i.e. 32 kHz), words are consumed in wall-clock time by a helper thread that
 * also keeps the channel's registers (READ_ADDR, TRANS_COUNT, BUSY) and the completion interrupt (INTR/INTS, NVIC IRQ 10) current.  The consumed
 * frames go to um2aud (resample to 32 kHz mono, ptx_audio_write).  No trap per sample: the ring is read in bulk from guest RAM.
 * UM2_NOAUDIO=1 drops all of it (words vanish as before). */
#define AUD_TICK_NS 4000000ull            /* pacing tick of an endless transfer; READ_ADDR is published half a tick ahead (stale by <= 2 ms either way) */
#define AUD_TOL_NS  30000000ull           /* a restart this late after a block ended still continues its timeline (the hardware's IRQ latency is microseconds, ours up to milliseconds: the late block then plays back to back with the one before, as a FIFO would) */
typedef struct {
	int act, endless, ir, pio, sm; unsigned bytes; uint32_t rd, rbase, rmask;
	uint64_t left, words, t_last, gap_ns, blk; double acc;
} Adma;
static Adma g_ad[16];
static uint64_t g_sm_end[NPIO][4]; static double g_sm_acc[NPIO][4];          /* timeline end of the last transfer per state machine */
static pthread_mutex_t g_aud_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aud_c;
/* Audio thread wake-ups (28 Oct 2026).  aud_start used to signal the thread for every block (the thread then ran, found nothing due, and slept again: two extra context switches
 * on the guest's CPU per millisecond, inside the lock the DMA store holds).  Now a block's completion time is predicted when the previous block ends (same size, timeline
 * continues), the thread sleeps until then, and aud_start only signals if the thread would sleep past the new block's end.  g_aud_gen: a start while the thread is unlocked
 * (pump) makes it rescan instead of sleeping on a stale deadline.  UM2_NOAUDPRED=1: signal every time, as before. */
static uint64_t g_aud_sleep_until; static int g_aud_sleeping, g_aud_pred = 1; static uint32_t g_aud_gen; static unsigned long g_aud_sig, g_aud_skip;
static int g_aud_used; static uint32_t g_dbg_wr, g_dbg_dump; static struct { uint64_t n, bad, t0; int mn, mx; double sum; FILE *log; } DF;
static volatile uint32_t g_dma_intr; static uint32_t g_dma_inte[4], g_dma_intf[4];    /* DMA IRQ registers (INTR, INTEn, INTFn); INTSn is derived */
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint32_t dma_ints(int g) { return (g_dma_intr | g_dma_intf[g]) & g_dma_inte[g]; }
static void dma_irq_sync(void) {            /* guest-visible copies (the page is read-only for the guest, so INTS0 polls in an ISR are plain loads) */
	if (!g_mir) return;
	g_mir[0x400 / 4] = g_dma_intr;
	for (int g = 0; g < 4; g++) { volatile uint32_t *m = g_mir + (0x404 + 0x10 * g) / 4; m[0] = g_dma_inte[g]; m[1] = g_dma_intf[g]; m[2] = dma_ints(g); }
}
static void irq_poke(void) { if (!C.irq_poke) return; if (g_busstat) { uint64_t a_ = bl_ns(); C.irq_poke(); uint64_t w_ = bl_ns() - a_; g_bst[11].n++; g_bst[11].wait += w_; if (w_ > g_bst[11].max) g_bst[11].max = w_; if (w_ > 100000) g_bst[11].big++; } else C.irq_poke(); }
static int i2s_cpf(int p, int s, int *bits) {           /* PIO cycles per stereo frame of the I2S program in the state machine's wrap window */
	const Sm *m = &g_pio[p].sm[s]; unsigned top = (m->execctrl >> 12) & 31, bot = (m->execctrl >> 7) & 31; int outs = 0, x = -1;
	for (unsigned i = bot; i <= top && i < 32; i++) { uint16_t w = g_pio[p].imem[i]; if ((w & 0xE0FF) == 0x6001) outs++; else if ((w & 0xE0E0) == 0xE020) x = w & 31; }   /* out pins,1 / set x,N (delay + side-set bits ignored) */
	if (outs == 4 && x >= 0) { *bits = x + 2; return 4 * (x + 2); }
	*bits = 0; return 64;                                 /* not the known program: assume 16-bit stereo anyway (logged) */
}
static double aud_fps(int p, int s) {
	const Sm *m = &g_pio[p].sm[s];
	unsigned di = m->clkdiv >> 16, df = (m->clkdiv >> 8) & 255; double div = di ? di + df / 256.0 : 65536.0;
	double sys = C.sysclk ? C.sysclk() : 300e6; int bits, cpf = i2s_cpf(p, s, &bits);
	double f = sys / div / cpf;
	AS.fps[p][s] = f; AS.bits[p][s] = bits;
	return f;
}
static int aud_dest(const Dch *d, unsigned treq, int *pio, int *sm) {
	for (int p = 1; p <= 2; p++) if (d->write >= PIO_BASE[p] + 0x10 && d->write < PIO_BASE[p] + 0x20) {
		int s = (int)(d->write - PIO_BASE[p] - 0x10) >> 2;
		if (treq == (unsigned)(p == 1 ? 8 : 16) + (unsigned)s) { *pio = p; *sm = s; return 1; }       /* DREQ_PIO1_TX0.. = 8, DREQ_PIO2_TX0.. = 16 */
	}
	return 0;
}
static void aud_start(int ch, int p, int s, unsigned bytes, uint32_t n, int endless, int ir, unsigned ring_sz, int ring_sel) {
	Dch *d = &g_dma[ch]; uint64_t now = mono_ns();
	pthread_mutex_lock(&g_aud_m);
	if (g_busstat) { uint64_t w_ = bl_ns() - now; g_bst[10].n++; g_bst[10].wait += w_; if (w_ > g_bst[10].max) g_bst[10].max = w_; if (w_ > 100000) g_bst[10].big++; }
	Adma *a = &g_ad[ch];
	uint64_t te = g_sm_end[p][s]; double acc = g_sm_acc[p][s];
	if (a->act) { te = a->t_last; acc = a->acc; }                                  /* restarted while running: keep its timeline */
	a->act = 1; a->endless = endless; a->ir = ir; a->pio = p; a->sm = s; a->bytes = bytes; a->rd = d->read; a->rbase = 0; a->rmask = 0;
	if (ring_sz && !ring_sel && ir) { a->rmask = (1u << ring_sz) - 1; a->rbase = d->read & ~a->rmask; }
	a->left = n; a->words = 0; a->blk = n;
	if (te && now >= te) { uint64_t late = now - te; int b = late < 1000000 ? 0 : late < 3000000 ? 1 : late < 10000000 ? 2 : late < AUD_TOL_NS ? 3 : 4; AS.late[b]++; if (late > AS.maxlate) AS.maxlate = late;
		{ static int lt = -2; static uint64_t t0; if (lt == -2) { const char *e = getenv("UM2_AUDLATE"); lt = e ? atoi(e) : -1; t0 = now; } if (lt >= 0 && late >= (uint64_t)lt * 1000000ull) fprintf(stderr, "audlate: block restart %.1f ms late at t=%.3f s (ch%d)\n", late / 1e6, (double)(now - t0) * 1e-9, ch); } }
	if (te && now >= te && now - te < AUD_TOL_NS) { a->t_last = te; a->acc = acc; a->gap_ns = 0; }
	else { a->t_last = now; a->acc = 0; a->gap_ns = te && now > te ? now - te : 0; }
	if (C.verbose && AS.starts < 6) vlog("um2bus: cart audio: DMA ch%d -> PIO%d SM%d, %s %u x%u bytes, ring %u, read %08x, %.2f frames/s\n", ch, p, s, endless ? "endless" : "block", (unsigned)n, bytes, ring_sz && !ring_sel ? 1u << ring_sz : 0, d->read, aud_fps(p, s));
	if (g_dbg_dump && (AS.starts % g_dbg_dump) == 0 && AS.starts < 40 * g_dbg_dump && C.host_ptr) {          /* UM2_AUDDUMP=n: the first words of every n-th transfer (diagnostics) */
		const uint8_t *q = C.host_ptr(d->read, bytes * 16);
		if (q) { fprintf(stderr, "um2bus: audio block #%llu at %08x:", (unsigned long long)AS.starts, d->read); for (int i = 0; i < 16; i++) { if (bytes == 2) { int16_t h; memcpy(&h, q + 2 * i, 2); fprintf(stderr, " %d", h); } else { uint32_t w; memcpy(&w, q + 4 * i, 4); fprintf(stderr, " %08x", w); } } fprintf(stderr, " ...\n"); }
	}
	AS.starts++; g_aud_used = 1;
	__sync_fetch_and_or(&g_aud_busy, 1u << ch);
	g_aud_gen++;
	{ int sig = 1;
	  if (g_aud_pred && g_aud_sleeping) {
		  double f = aud_fps(p, s); uint64_t end = endless || f < 1 ? now + AUD_TICK_NS : a->t_last + (uint64_t)(((double)n - a->acc) / f * 1e9);
		  if (g_aud_sleep_until <= end + 100000) sig = 0;                         /* the thread wakes before (or just after) the new block ends */
	  }
	  if (sig) { g_aud_sig++; pthread_cond_signal(&g_aud_c); } else g_aud_skip++; }
	pthread_mutex_unlock(&g_aud_m);
}
/* consume what the state machine has taken since the last step; returns 1 if a completion interrupt was raised */
static int aud_step(int ch, uint64_t now, uint64_t *dl) {
	Adma *a = &g_ad[ch]; Dch *d = &g_dma[ch]; int irq = 0;
	double fps = (g_pio[a->pio].ctrl >> a->sm & 1) ? aud_fps(a->pio, a->sm) : 0;
	AS.steps++;
	if (fps < 1) { a->t_last = now; *dl = now + AUD_TICK_NS; return 0; }          /* state machine stopped: the FIFO fills up and the transfer waits */
	if (fps > 4e5) fps = 4e5;
	double due = a->acc + (double)(now - a->t_last) * 1e-9 * fps;
	uint64_t w = (uint64_t)due; a->acc = due - (double)w; a->t_last = now;
	int fin = 0; double over = 0;
	if (!a->endless && w >= a->left) { over = (double)(w - a->left) + a->acc; w = a->left; fin = 1; }
	uint64_t gap = a->gap_ns; a->gap_ns = 0;
	while (w) {
		int16_t buf[1024]; uint32_t k = w > 1024 ? 1024 : (uint32_t)w, run = k;
		if (a->rmask) { uint32_t to_end = (a->rbase + a->rmask + 1 - a->rd) / a->bytes; if (to_end && to_end < run) run = to_end; }
		if (!a->ir) run = 1;
		const uint8_t *src = C.host_ptr ? C.host_ptr(a->rd, run * a->bytes) : NULL;
		for (uint32_t i = 0; i < run; i++) {
			uint32_t v = 0;
			if (src) { const uint8_t *q = src + (a->ir ? i * a->bytes : 0); if (a->bytes == 4) memcpy(&v, q, 4); else if (a->bytes == 2) { uint16_t h; memcpy(&h, q, 2); v = h * 0x10001u; } else v = q[0] * 0x01010101u; }
			buf[i] = (int16_t)(((int32_t)(int16_t)(v >> 16) + (int32_t)(int16_t)v) / 2);
		}
		um2aud_put(buf, run, (uint32_t)(fps + 0.5), gap); gap = 0;
		if (a->ir) { a->rd += run * a->bytes; if (a->rmask) a->rd = a->rbase | (a->rd & a->rmask); }
		w -= run; a->words += run; AS.words += run;
		if (!a->endless) a->left -= run;
	}
	if (fin) {
		uint64_t te = now - (uint64_t)(over / fps * 1e9);
		g_sm_end[a->pio][a->sm] = te; g_sm_acc[a->pio][a->sm] = 0;
		a->act = 0; __sync_fetch_and_and(&g_aud_busy, ~(1u << ch));
		d->count = 0; d->read = a->rd; dma_sync(ch); AS.blocks++;
		if (g_aud_pred && !a->endless) { uint64_t nx = te + (uint64_t)((double)a->blk / fps * 1e9); if (nx > now) *dl = nx; }         /* the game restarts a block of the same size: wake at its end */
		if (!(d->ctrl & (1u << 23))) {                                              /* IRQ_QUIET off: the block's completion interrupt */
			__sync_fetch_and_or(&g_dma_intr, 1u << ch); dma_irq_sync(); AS.irqs++; irq = 1;
		}
		return irq;
	}
	if (g_dbg_wr && a->endless && a->rmask && C.host_ptr) {                          /* UM2_AUDFILL=<hex addr>: the guest's ring write index (frames); fill level statistics (SW: 2007c808) */
		const uint8_t *q = C.host_ptr(g_dbg_wr, 4); uint32_t wr, nw = (a->rmask + 1) / a->bytes;
		if (q) { memcpy(&wr, q, 4); int fl = (int)((wr - (a->rd - a->rbase) / a->bytes) & (nw - 1)); if (!DF.n || fl < DF.mn) DF.mn = fl; if (fl > DF.mx) DF.mx = fl; DF.n++; DF.sum += fl; if (fl > (int)(nw * 7 / 8)) DF.bad++; if (DF.log) fprintf(DF.log, "%.4f %d\n", (double)(now - DF.t0) * 1e-9, fl); }
	}
	if (a->endless) {                                                               /* READ_ADDR half a tick ahead of the true position */
		uint32_t ahead = (uint32_t)(fps * (AUD_TICK_NS / 2) * 1e-9), pr = a->rd;
		if (a->ir) { pr += ahead * a->bytes; if (a->rmask) pr = a->rbase | (pr & a->rmask); }
		d->read = pr; dma_sync(ch);
		*dl = now + AUD_TICK_NS;
	} else {
		d->read = a->rd; d->count = (uint32_t)a->left; dma_sync(ch);
		double need = ((double)a->left - a->acc) / fps; if (need < 0.0001) need = 0.0001;
		*dl = now + (uint64_t)(need * 1e9);
	}
	return 0;
}
static int aud_trylock(void) {
#ifndef UM2BUS_NO_THREADS
	int r_ = pthread_mutex_trylock(&g_bus); if (!r_ && g_busstat) { tl_bl_t0 = bl_ns(); g_bst[8].wait += 0; } return r_;
#else
	return 0;
#endif
}
static void aud_unlock(void) { BUS_UNLOCK_T(8); }
static void *aud_thread(void *arg) {
	(void)arg;
	pthread_setname_np(pthread_self(), "u2aud");
	um2_rt_boost(2);
	pthread_mutex_lock(&g_aud_m);
	for (;;) {
		uint64_t now = mono_ns(), dl = now + AUD_TICK_NS; int any = 0, irq = 0; uint32_t gen = g_aud_gen;
		for (int ch = 0; ch < 16; ch++) if (g_ad[ch].act) { uint64_t d1 = dl; any = 1; irq |= aud_step(ch, now, &d1); if (d1 < dl) dl = d1; }
		pthread_mutex_unlock(&g_aud_m);
		if (irq) irq_poke();
		um2aud_pump(0);
		pthread_mutex_lock(&g_aud_m);
		int act = 0; for (int ch = 0; ch < 16; ch++) act |= g_ad[ch].act;
		if (g_aud_pred && g_aud_gen != gen) continue;                              /* a block was started while the lock was open: rescan */
		if (!act && !any) {
			struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); g_aud_sleeping = 1;
			if (g_aud_used) { g_aud_sleep_until = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + 50000000ull; ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; } pthread_cond_timedwait(&g_aud_c, &g_aud_m, &ts); }
			else { g_aud_sleep_until = ~0ull; pthread_cond_wait(&g_aud_c, &g_aud_m); }
			g_aud_sleeping = 0; continue; }
		if (dl < now + 100000) dl = now + 100000;
		struct timespec ts = { (time_t)(dl / 1000000000ull), (long)(dl % 1000000000ull) };
		g_aud_sleep_until = dl; g_aud_sleeping = 1;
		pthread_cond_timedwait(&g_aud_c, &g_aud_m, &ts);
		g_aud_sleeping = 0;
	}
	return NULL;
}
static void aud_abort(uint32_t mask) {
	pthread_mutex_lock(&g_aud_m);
	for (int ch = 0; ch < 16; ch++) if ((mask >> ch & 1) && g_ad[ch].act) {
		g_ad[ch].act = 0; g_sm_end[g_ad[ch].pio][g_ad[ch].sm] = g_ad[ch].t_last; g_sm_acc[g_ad[ch].pio][g_ad[ch].sm] = g_ad[ch].acc;
		__sync_fetch_and_and(&g_aud_busy, ~(1u << ch)); AS.aborts++;
	}
	pthread_mutex_unlock(&g_aud_m);
}
static void dma_irq_store(unsigned o, unsigned alias, uint32_t v) {          /* INTR / INTEn / INTFn / INTSn at 0x400.. (alias: 0 write, 1 xor, 2 set, 3 clear) */
	if (o == 0x400) __sync_fetch_and_and(&g_dma_intr, ~v);                  /* raw status: write 1 to clear */
	else if (o < 0x440) {
		unsigned g = (o - 0x404) >> 4, r = (o - 0x404) & 0xF;
		uint32_t *p = r == 0 ? &g_dma_inte[g] : r == 4 ? &g_dma_intf[g] : NULL;
		if (p) *p = (alias == 0 ? v : alias == 1 ? *p ^ v : alias == 2 ? *p | v : *p & ~v) & 0xFFFFu;
		else if (r == 8) __sync_fetch_and_and(&g_dma_intr, ~v);             /* INTS: write 1 to clear the raw bits */
	}
	dma_irq_sync(); irq_poke();
}

static void dma_run(int ch, int depth) {
	Dch *d = &g_dma[ch];
	if (!(d->ctrl & 1) || depth > 16) return;
	unsigned size = (d->ctrl >> 2) & 3, bytes = 1u << size;
	if (size == 3) { S.unsupported++; return; }
	int ir = (d->ctrl >> 4) & 1, iw = (d->ctrl >> 6) & 1;
	unsigned ring_sz = (d->ctrl >> 8) & 15, ring_sel = (d->ctrl >> 12) & 1;
	uint32_t n = d->count & 0x0FFFFFFFu, mode = d->count >> 28;
	uint32_t treq = (d->ctrl >> 17) & 0x3F;
	{ int ap, as;
	  if (um2aud_enabled() && aud_dest(d, treq, &ap, &as)) {                      /* cart audio: paced, not synchronous */
		  if (((d->ctrl >> 13) & 15) != (unsigned)ch) vlog("um2bus: cart audio DMA ch%d chains to ch%u: chaining not modelled for paced transfers\n", ch, (d->ctrl >> 13) & 15);
		  if (d->ctrl & (1u << 24)) vlog("um2bus: cart audio DMA ch%d: BSWAP not modelled\n", ch);
		  aud_start(ch, ap, as, bytes, n, mode == 0xF, ir, ring_sz, (int)ring_sel);
		  dma_sync(ch); return; } }
	if (mode == 0xF) { S.unsupported++; vlog("um2bus: DMA endless mode not modelled\n"); n = 0; }
	int to_pio = (d->write >= 0x50200010u && d->write < 0x50200020u) || (d->write >= 0x50300010u && d->write < 0x50300020u) || (d->write >= 0x50400010u && d->write < 0x50400020u);
	if ((treq == 24 || treq == 25) && bytes == 1) {
		if (C.verbose > 1) vlog("um2bus: DMA ch%d SPI0 %s %u bytes  %08x -> %08x\n", ch, treq == 24 ? "TX" : "RX", n, d->read, d->write);
		spi_dma_run(d, treq, n, ir, iw);
		d->count = 0; dma_sync(ch);
		return;
	}
	S.dma_runs++; S.dma_words += n;
	if (C.verbose > 1) vlog("um2bus: DMA ch%d %u x%u  %08x -> %08x  treq=%u ctrl=%08x\n", ch, n, bytes, d->read, d->write, treq, d->ctrl);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v; guest_read(d->read, (int)bytes, &v);
		guest_write(d->write, (int)bytes, v);
		if (ir) d->read += bytes;
		if (iw) d->write += bytes;
		if (ring_sz) {
			uint32_t m = (1u << ring_sz) - 1u;
			if (ring_sel) d->write = (d->write & ~m) | (d->write & m); else d->read = (d->read & ~m) | (d->read & m);
		}
	}
	d->count = 0;
	d->busy_polls = (to_pio && !g_mir) ? 70u : 0u;
	if (g_mir && to_pio) { __sync_fetch_and_or(&g_busy_mask, 1u << ch); busy_request(); }
	dma_sync(ch);
	unsigned chain = (d->ctrl >> 13) & 15;
	if (chain != (unsigned)ch) dma_run((int)chain, depth + 1);
}

static int dma_store(uint32_t off, int size, uint32_t v) {
	(void)size;
	if (off < 0x4000 && (off & 0xFFF) >= 0x400 && (off & 0xFFF) < 0x440) { dma_irq_store(off & 0xFFF, (off >> 12) & 3, v); return 1; }
	if (off < 0x400) {
		int ch = (int)(off >> 6); unsigned r = off & 0x3F;
		Dch *d = &g_dma[ch];
		int trig = 0;
		switch (r) {
		case 0x00: d->read = v; break;
		case 0x04: d->write = v; break;
		case 0x08: d->count = v; break;
		case 0x0C: d->ctrl = v; trig = 1; break;
		case 0x10: d->ctrl = v; break;
		case 0x14: d->read = v; break;
		case 0x18: d->write = v; break;
		case 0x1C: d->count = v; trig = 1; break;
		case 0x20: d->ctrl = v; break;
		case 0x24: d->count = v; break;
		case 0x28: d->read = v; break;
		case 0x2C: d->write = v; trig = 1; break;
		case 0x30: d->ctrl = v; break;
		case 0x34: d->write = v; break;
		case 0x38: d->count = v; break;
		case 0x3C: d->read = v; trig = 1; break;
		}
		if (trig) dma_run(ch, 0);
		dma_sync(ch);
		return 1;
	}
	if (off == 0x450 || off == 0x430) {
		for (int i = 0; i < 16; i++) if ((v >> i & 1) && ((g_dma[i].ctrl >> 17) & 0x3F) == 24) dma_run(i, 0);      /* SPI0 TX before its RX */
		for (int i = 0; i < 16; i++) if ((v >> i & 1) && ((g_dma[i].ctrl >> 17) & 0x3F) != 24) dma_run(i, 0);
		return 1; }
	if (off == 0x444) return 1;                       /* CHAN_ABORT (RP2040 offset): nothing is ever busy */
	if (off == 0x464) { aud_abort(v); return 1; }     /* CHAN_ABORT (RP2350) */
	if (off < 0x1000) { g_dma_sh[off >> 2] = v; return 1; }
	return 0;
}
static int dma_load(uint32_t off, int size, uint32_t *v) {
	(void)size;
	if (off < 0x400) {
		int ch = (int)(off >> 6); unsigned r = off & 0x3F; Dch *d = &g_dma[ch];
		switch (r) {
		case 0x00: case 0x14: case 0x28: case 0x3C: *v = d->read; break;
		case 0x04: case 0x18: case 0x2C: case 0x34: *v = d->write; break;
		case 0x08: case 0x1C: case 0x24: case 0x38: *v = d->count; break;
		default:
			/* Transfers are synchronous, but BUSY (bit 26 on RP2350) is reported for a number of polls after a trigger
			 * that targets a PIO TX FIFO: on hardware the DMA is paced by the SM (one word per E period), so callers
			 * accumulate words into batches while it is busy.  Reporting idle at once would turn every push into its
			 * own 1-word DMA (about 7 MMIO traps per bus word here). */
			if (d->busy_polls) { d->busy_polls--; *v = d->ctrl | (1u << 26); }
			else *v = (g_aud_busy >> ch & 1) ? d->ctrl | (1u << 26) : d->ctrl & ~(1u << 26);
			break;
		}
		return 1;
	}
	if (off >= 0x400 && off < 0x440) {
		if (off == 0x400) *v = g_dma_intr;
		else { unsigned g = (off - 0x404) >> 4, r = (off - 0x404) & 0xF; *v = r == 0 ? g_dma_inte[g] : r == 4 ? g_dma_intf[g] : r == 8 ? dma_ints((int)g) : 0; }
		return 1;
	}
	if (off == 0x464) { *v = 0; return 1; }
	if (off == 0x448) { *v = 16; return 1; }
	if (off == 0x440) { *v = 0; return 1; }
	if (off < 0x1000) { *v = g_dma_sh[off >> 2]; return 1; }
	return 0;
}

/* ------------------------------------------------------------------ SIO */
static uint32_t g_sio_out, g_sio_oe;
static int g_clk;                       /* GPIO31 as the software sees it */
static int g_rd_active; static uint8_t g_rd_data;
static uint32_t g_spin;                 /* bit n: spinlock n is held */
#define FQ 16
static uint32_t g_q[2][FQ]; static unsigned g_qh[2], g_qc[2];   /* [0] core0 -> core1, [1] core1 -> core0 */
static int g_core1_running; static unsigned g_lseq; static uint32_t g_lw[6];

/* SIO-driven bus (single accesses; and whole lists in the older single-core images, whose executor bit-bangs every
 * write).  The clock input toggles on every GPIO_IN read, so a polled E period is two reads.  A falling edge latches the bus
 * state if it is a VIA cycle (accounted by the sink: one cycle); an edge that latches nothing is an idle E period and advances
 * stream time, except for `credit`: periods the PIO stream already accounted for, which drain() polls out again on hardware. */
static __thread uint64_t tl_edges;       /* E periods this thread has polled (the guest's DWT counter is per core) */
static int gpio_edge_fall(void) {
	S.sio_edges++; tl_edges++;
	uint32_t gp = g_sio_out & g_sio_oe;
	if (!(g_sio_oe & PIN_RW) || !(g_sio_oe & PIN_A15)) return 0;
	uint32_t addr = pins_addr(g_sio_out);
	if ((addr & 0xF000u) != 0xD000u) {
		if (!(g_sio_out & PIN_RW)) S.non_via_writes++;
		return 0;
	}
	if (!(g_sio_out & PIN_RW)) {
		C.vwrite((uint16_t)addr, (uint8_t)(gp & 0xFF)); S.sio_writes++;
	} else {
		/* start of the between-frames window (controllers): the frame mark for images that draw over SIO */
		if (!g_frames_sm && C.frame_cycles && C.now() - g_fstart >= 3000) { g_fstarted = 1; frame_mark(0); }
		g_rd_data = C.vread((uint16_t)addr); g_rd_active = 1; S.sio_reads++;
		if (C.verbose > 1 && (addr & 0xF) == 1) { static unsigned shown; if (shown++ < 400) vlog("um2bus: read %04x -> %02x (t=%llu)\n", addr, g_rd_data, (unsigned long long)C.now()); }
	}
	return 1;
}

static uint32_t gpio_in(void) {
	g_clk ^= 1;
	if (!g_clk && !gpio_edge_fall()) { if (g_credit) g_credit--; else if (g_edge_time) C.advance(1); }
	uint32_t v = 0xFFFFFFFFu & ~0x80000000u;
	v = (v & ~g_sio_oe) | (g_sio_out & g_sio_oe);
	if (!(g_sio_oe & 0xFF) && g_rd_active) v = (v & ~0xFFu) | g_rd_data;
	if (g_clk) v |= 0x80000000u;
	return v;
}

static uint32_t g_hi_out;       /* GPIO32..47 output latch: the SD card's CS is GPIO39 = bit 7, active low */
static void sio_gpio_set(uint32_t off, uint32_t v) {
	switch (off) {
	case 0x14: g_hi_out = v; um2sd_select(!(g_hi_out & 0x80)); return;
	case 0x1C: g_hi_out |= v; um2sd_select(!(g_hi_out & 0x80)); return;
	case 0x24: g_hi_out &= ~v; um2sd_select(!(g_hi_out & 0x80)); return;
	case 0x2C: g_hi_out ^= v; um2sd_select(!(g_hi_out & 0x80)); return;
	case 0x10: g_sio_out = v; break;
	case 0x18: g_sio_out |= v; break;
	case 0x20: g_sio_out &= ~v; break;
	case 0x28: g_sio_out ^= v; break;
	case 0x30: g_sio_oe = v; break;
	case 0x38: g_sio_oe |= v; break;
	case 0x40: g_sio_oe &= ~v; break;
	case 0x48: g_sio_oe ^= v; break;
	}
	if (off != 0x30 && off != 0x40) { /* output changed: a held read result no longer applies */ }
	g_rd_active = g_rd_active && (off == 0x38 || off == 0x40);
}

static int sio_store(uint32_t off, int size, uint32_t v, int core) {
	(void)size;
	if (off >= 0x10 && off <= 0x4C) { cap(CAP_SIO_ST, size, 0xD0000000u + off, v); sio_gpio_set(off, v); return 1; }
	if (off == 0x54) {                                          /* FIFO_WR */
		MISC_LOCK();
		if (core == 0 && !g_core1_running) {
			static const uint32_t exp[3] = { 0, 0, 1 };
			g_q[1][(g_qh[1] + g_qc[1]) % FQ] = v; if (g_qc[1] < FQ) g_qc[1]++;     /* the bootrom echoes every word */
			if (g_lseq < 3 ? v == exp[g_lseq] : 1) { g_lw[g_lseq++] = v; } else { g_lseq = (v == 0) ? 1 : 0; if (g_lseq) g_lw[0] = 0; }
			if (g_lseq == 6) {
				g_core1_running = 1; MISC_UNLOCK();
				vlog("um2bus: core 1 launch vtor=%08x sp=%08x entry=%08x\n", g_lw[3], g_lw[4], g_lw[5]);
				if (C.launch_core1) C.launch_core1(g_lw[3], g_lw[4], g_lw[5]);
				return 1;
			}
		} else {
			int q = core == 0 ? 0 : 1;
			if (g_qc[q] < FQ) { g_q[q][(g_qh[q] + g_qc[q]) % FQ] = v; g_qc[q]++; }
		}
		MISC_UNLOCK();
		return 1;
	}
	if (off == 0x50) return 1;                                  /* FIFO_ST: write clears ROE/WOF */
	if (off >= 0x100 && off < 0x180) { __sync_fetch_and_and(&g_spin, ~(1u << ((off - 0x100) >> 2))); return 1; }
	return 0;
}

static int sio_load(uint32_t off, int size, uint32_t *v, int core) {
	(void)size;
	if (off == 0x00) { *v = (uint32_t)core; return 1; }
	if (off == 0x04) { *v = gpio_in(); cap(CAP_SIO_GPIN, 4, 0xD0000004u, 0); return 1; }
	if (off >= 0x10 && off <= 0x2C) { *v = g_sio_out; return 1; }
	if (off >= 0x30 && off <= 0x4C) { *v = g_sio_oe; return 1; }
	if (off == 0x50) { MISC_LOCK(); int rq = core == 0 ? 1 : 0; *v = (g_qc[rq] ? 1u : 0u) | 2u; MISC_UNLOCK(); return 1; }
	if (off == 0x58) {
		MISC_LOCK(); int rq = core == 0 ? 1 : 0;
		if (g_qc[rq]) { *v = g_q[rq][g_qh[rq]]; g_qh[rq] = (g_qh[rq] + 1) % FQ; g_qc[rq]--; } else *v = 0;
		MISC_UNLOCK(); return 1;
	}
	if (off == 0x5C) { *v = g_spin; return 1; }
	if (off >= 0x100 && off < 0x180) {
		unsigned n = (off - 0x100) >> 2;
		uint32_t bit = 1u << n;
		uint32_t old = __sync_fetch_and_or(&g_spin, bit);
		*v = (old & bit) ? 0 : bit;
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ frames */
static void frame_mark(int forced) {
	Sm *s = SIMPIO;
	if (s->started) sink_sync_to(s, s->t / 100);
	uint64_t used = C.now() - g_fstart;
	if (C.frame_cycles && used + 2 < C.frame_cycles) { uint64_t pad = C.frame_cycles - used - 2; S.idle_pad_cycles += pad; while (pad) { uint32_t c = pad > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)pad; C.advance(c); pad -= c; } }
	if (um2aud_enabled()) um2aud_pump(1);   /* a frame mark is a good moment for the audio too (the bus lock is held here, and ptx_frame may now sleep for pacing) */
	C.vwrite(0xD008, 0x30);                 /* T2 reload: the core takes accepted T2 reloads as frame boundaries */
	C.vwrite(0xD009, 0x75);
	if (C.verbose > 1) { double t0 = tnow(); C.frame(); g_tf += tnow() - t0; } else C.frame();
	uint64_t fc = C.now() - g_fstart;
	S.frames++; if (forced) S.forced_frames++;
	S.frame_cycles_sum += fc;
	if (!S.frame_cycles_min || fc < S.frame_cycles_min) S.frame_cycles_min = fc;
	if (fc > S.frame_cycles_max) S.frame_cycles_max = fc;
	g_fstart = C.now();
	{ static int first; double tt = tnow(); if (!first) { first = 1; g_wall0 = tt; g_frames0 = S.frames; } g_wall1 = tt; }
	if (C.verbose > 1) {
		static uint64_t lp; static struct timespec lt;
		struct timespec nt; clock_gettime(CLOCK_MONOTONIC, &nt);
		double ms = (nt.tv_sec - lt.tv_sec) * 1e3 + (nt.tv_nsec - lt.tv_nsec) * 1e-6; lt = nt;
		vlog("um2bus: mark %llu used=%llu words=%llu wall=%.1fms sinkw=%.2fs frame=%.2fs thr=%.2fs proc=%.2fs traps=%llu in-handler=%.2fs\n", (unsigned long long)S.frames, (unsigned long long)used, (unsigned long long)(S.pushes - lp), ms, g_tw, g_tf, cpu_s(CLOCK_THREAD_CPUTIME_ID), cpu_s(CLOCK_PROCESS_CPUTIME_ID), (unsigned long long)g_trap_n, g_trap_ns * 1e-9);
		lp = S.pushes;
	}
	if (C.verbose && (S.frames <= 5 || S.frames % 100 == 0)) vlog("um2bus: frame %llu used=%llu cycles (list+singles), pad=%llu\n", (unsigned long long)S.frames, (unsigned long long)used, (unsigned long long)S.idle_pad_cycles);
}

/* RESETS.RESET (0x40020000, atomic aliases at +0x1000/2000/3000): asserting a block's reset clears it.
 * pico-sdk's install code does this to PIO0 and the DMA before programming them, so words pushed earlier vanish. */
static void pio_reset(int p) {
	Pio *pp = &g_pio[p];
	for (int j = 0; j < 4; j++) {
		Sm *s = &pp->sm[j];
		uint32_t *f = s->fifo; uint64_t si = s->sink_idx; int st = s->started;
		memset(s, 0, sizeof *s);
		s->fifo = f; s->sink_idx = si; s->started = st; s->osr_cnt = 32;
		s->clkdiv = 0x00010000;
	}
	pp->ctrl = 0; pp->gpiobase = 0; memset(pp->imem, 0, sizeof pp->imem); memset(pp->sh, 0, sizeof pp->sh);
	if (C.verbose) vlog("um2bus: PIO%d reset\n", p);
}
static void resets_store(uint32_t addr, uint32_t val) {
	if ((addr & 0xFFF) != 0) return;
	unsigned alias = (addr >> 12) & 3;
	if (alias == 3 || alias == 1) return;          /* clear = release; xor not modelled */
	BUS_LOCK();
	if (val & (1u << 11)) pio_reset(0);
	if (val & (1u << 12)) pio_reset(1);
	if (val & (1u << 13)) pio_reset(2);
	if (val & (1u << 2)) { aud_abort(0xFFFF); memset(g_dma, 0, sizeof g_dma); g_dma_intr = 0; memset(g_dma_inte, 0, sizeof g_dma_inte); memset(g_dma_intf, 0, sizeof g_dma_intf); for (int i = 0; i < 16; i++) dma_sync(i); dma_irq_sync(); if (C.verbose) vlog("um2bus: DMA reset\n"); }
	BUS_UNLOCK();
}

/* ------------------------------------------------------------------ SPI0 (PL022) with an SD card behind it */
static uint64_t g_spi_h = 0xcbf29ce484222325ull; static unsigned long g_spi_sr, g_spi_drr, g_spi_drw;   /* SR polls, DR reads/writes, FNV-1a of the DR values the guest read */
static int spi_store(uint32_t addr, uint32_t val) {
	uint32_t off = addr & 0xFFF; unsigned alias = (addr >> 12) & 3;
	if (off == 0x08 && alias == 0) {
		uint8_t r = spi_xfer_byte((uint8_t)val); g_spi_drw++;
		if (g_spi0.rxn < 8) g_spi0.rx[g_spi0.rxn++] = r;
		return 1;
	}
	if (off == 0x04) { uint32_t c = g_spi0.cr1; g_spi0.cr1 = alias == 0 ? val : alias == 1 ? c ^ val : alias == 2 ? c | val : c & ~val; return 0; }
	return 0;
}
static int spi_load(uint32_t addr, uint32_t *v) {
	uint32_t off = addr & 0xFFF;
	if (off == 0x08) { uint32_t r = 0; if (g_spi0.rxn) { r = g_spi0.rx[0]; memmove(g_spi0.rx, g_spi0.rx + 1, (size_t)--g_spi0.rxn); } *v = r; g_spi_drr++; g_spi_h = (g_spi_h ^ r) * 0x100000001b3ull; return 1; }
	if (off == 0x0C) { g_spi_sr++; *v = 0x03u | (g_spi0.rxn ? 4u : 0u) | (g_spi0.rxn >= 8 ? 8u : 0u); return 1; }   /* TFE|TNF, RNE, RFF; never BSY */
	return 0;
}

/* ------------------------------------------------------------------ QMI direct mode with an APS6404L-style PSRAM on CS1
 * The Star Wars image (like other UVMC2 images that stage a romset in PSRAM) resets the PSRAM through QMI direct mode (0x66, 0x99), reads
 * its ID (0x9F + 3 dummy bytes, then MF ID, KGD) and only treats the PSRAM as present when the first ID byte is 0x0D (AP Memory).  The
 * generic MMIO shadow returns 0 for DIRECT_RX, so the image concluded "no PSRAM" and never even opened the ROM zip ("NO PSRAM FOR THE
 * ROM ZIP").  Model: every DIRECT_TX byte pushes one DIRECT_RX byte; after a 0x9F command the 5th.. bytes of the transaction carry
 * the ID (0D 5D 52 ...), everything else reads 0xFF.  CSR/TX stay in the shadow too (store returns 0), so the busy/level bits it polls
 * keep reading what was written (0 = not busy, TX not full, RX not empty). */
static struct { uint8_t rx[16]; int rxn; int txn; uint8_t cmd; uint32_t csr; } g_qmi;
static int qmi_store(uint32_t addr, uint32_t val) {
	uint32_t off = addr & 0xFFF; unsigned alias = (addr >> 12) & 3;
	if (off == 0x00) {                                      /* DIRECT_CSR: CS1 (bit 3) asserted starts a transaction */
		uint32_t c = g_qmi.csr; g_qmi.csr = alias == 0 ? val : alias == 1 ? c ^ val : alias == 2 ? c | val : c & ~val;
		if (!(c & 8) && (g_qmi.csr & 8)) { g_qmi.txn = 0; g_qmi.rxn = 0; }
		return 0;
	}
	if (off == 0x04 && alias == 0) {                        /* DIRECT_TX */
		uint8_t b = (uint8_t)val, r = 0xFF; static const uint8_t id[] = { 0x0D, 0x5D, 0x52, 0x57, 0x50, 0x00, 0x00, 0x00 };
		if (g_qmi.txn == 0) g_qmi.cmd = b;
		if (g_qmi.cmd == 0x9F && g_qmi.txn >= 4) r = id[(g_qmi.txn - 4) & 7];
		g_qmi.txn++;
		if (g_qmi.rxn < (int)sizeof g_qmi.rx) g_qmi.rx[g_qmi.rxn++] = r;
		return 0;
	}
	return 0;
}
static int qmi_load(uint32_t addr, uint32_t *v) {
	if ((addr & 0xFFF) == 0x08) { uint32_t r = 0xFF; if (g_qmi.rxn) { r = g_qmi.rx[0]; memmove(g_qmi.rx, g_qmi.rx + 1, (size_t)--g_qmi.rxn); } *v = r; return 1; }   /* DIRECT_RX */
	return 0;
}

/* ------------------------------------------------------------------ entry points */
int um2bus_store(uint32_t addr, int size, uint32_t val, int core) {
	uint32_t base = addr & ~0xF000u;
	if (addr >= 0xD0000000u && addr < 0xD0000200u) {
		uint32_t off = addr - 0xD0000000u;
		if (off >= 0x10 && off <= 0x4C) { BUS_LOCK_T(0); int r = sio_store(off, size, val, core); BUS_UNLOCK_T(0); return r; }
		return sio_store(off, size, val, core);
	}
	if ((addr & 0xFFFF0000u) == 0x50000000u) { BUS_LOCK_T(2); int r = dma_store(addr & 0xFFFF, size, val); BUS_UNLOCK_T(2); return r; }
	(void)base;
	if ((addr & 0xFFFFC000u) == 0x40080000u) { BUS_LOCK_T(4); int r = spi_store(addr, val); BUS_UNLOCK_T(4); return r; }
	if ((addr & 0xFFFFC000u) == 0x400D0000u) { BUS_LOCK_T(5); int r = qmi_store(addr, val); BUS_UNLOCK_T(5); return r; }
	if ((addr & 0xFFFFC000u) == 0x40020000u) { resets_store(addr, val); return 0; }   /* RESETS: the shadow still sees it */
	if (addr == 0x4001B004u && (val & (1u << 24))) {      /* PSM FRCE_OFF cleared for PROC1: the bootrom on core 1 posts a 0 to core 0 */
		MISC_LOCK(); g_core1_running = 0; g_lseq = 0; g_qc[1] = 0; g_qh[1] = 0; g_q[1][0] = 0; g_qc[1] = 1; MISC_UNLOCK();
		return 0;                                       /* the shadow still sees the store */
	}
	for (int i = 0; i < NPIO; i++)
		if ((addr & 0xFFFF0000u) == PIO_BASE[i]) { BUS_LOCK_T(6); int r = pio_store(i, addr & 0xFFFF, size, val); BUS_UNLOCK_T(6); return r; }
	return 0;
}

int um2bus_load(uint32_t addr, int size, uint32_t *val, int core) {
	if ((addr & 0xFFFFC000u) == 0x40080000u) { BUS_LOCK_T(4); int r = spi_load(addr, val); BUS_UNLOCK_T(4); return r; }
	if ((addr & 0xFFFFC000u) == 0x400D0000u) { BUS_LOCK_T(5); int r = qmi_load(addr, val); BUS_UNLOCK_T(5); return r; }
	if (addr >= 0xD0000000u && addr < 0xD0000200u) {
		uint32_t off = addr - 0xD0000000u;
		if (off == 0x04 || (off >= 0x10 && off <= 0x4C)) { BUS_LOCK_T(1); int r = sio_load(off, size, val, core); BUS_UNLOCK_T(1); return r; }
		return sio_load(off, size, val, core);
	}
	if ((addr & 0xFFFF0000u) == 0x50000000u) { BUS_LOCK_T(3); int r = dma_load(addr & 0xFFFF, size, val); BUS_UNLOCK_T(3); return r; }
	for (int i = 0; i < NPIO; i++)
		if ((addr & 0xFFFF0000u) == PIO_BASE[i]) { BUS_LOCK_T(7); int r = pio_load(i, addr & 0xFFFF, size, val); BUS_UNLOCK_T(7); return r; }
	return 0;
}

void um2bus_init(const um2bus_cfg_t *cfg) {
	C = *cfg;
	memset(&S, 0, sizeof S);
	for (int i = 0; i < NPIO; i++) for (int j = 0; j < 4; j++) g_pio[i].sm[j].fifo = NULL;
	static uint32_t fifo0[FIFO_CAP];   /* static: a big malloc is an mmap, which qemu-user may place in the RP2350 peripheral window */
	SIMPIO->fifo = fifo0;
	for (int j = 0; j < 4; j++) for (int i = 0; i < NPIO; i++) g_pio[i].sm[j].osr_cnt = 32;
	if (C.capture_path) g_cap = fopen(C.capture_path, "wb");
	g_nofast = getenv("UM2BUS_NOFAST") != NULL; g_aud_pred = getenv("UM2_NOAUDPRED") == NULL; g_busstat = getenv("UM2_BUSSTAT") != NULL;
	if (C.dma_mirror) { g_mir = C.dma_mirror; pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 16); if (!pthread_create(&th, &at, busy_thread, NULL)) pthread_detach(th); else g_mir = NULL; }
	if (getenv("UM2BUS_NOEDGE")) g_edge_time = 0;
	{ const char *e = getenv("UM2_AUDDUMP"); if (e) g_dbg_dump = (uint32_t)atoi(e); }
	{ const char *e = getenv("UM2_AUDFILL"); if (e) g_dbg_wr = (uint32_t)strtoul(e, NULL, 16); e = getenv("UM2_AUDFILL_LOG"); if (e && g_dbg_wr) { DF.log = fopen(e, "w"); DF.t0 = mono_ns(); } }
	um2aud_init(aud_trylock, aud_unlock);
	if (um2aud_enabled()) {         /* created here, not at the first transfer: it must inherit the main thread's CPU affinity (core 1 may be pinned next to Main) */
		pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&g_aud_c, &ca);
		pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 16);
		if (!pthread_create(&th, &at, aud_thread, NULL)) pthread_detach(th); else perror("um2bus: audio thread");
	}
}

uint64_t um2bus_irq_lines(void) {
	uint64_t m = 0;
	for (int g = 0; g < 4; g++) if (dma_ints(g)) m |= 1ull << (10 + g);              /* DMA_IRQ_0..3 = NVIC 10..13 */
	return m;
}
uint64_t um2bus_edges(void) { return tl_edges; }

void um2bus_dump_pio(FILE *f) {
	static int once; if (__sync_fetch_and_add(&once, 1)) return;
	static char buf[16384]; int n = 0;
	for (int i = 0; i < NPIO; i++) {
		Pio *p = &g_pio[i];
		n += snprintf(buf + n, sizeof buf - n, "PIO%d ctrl=%x\n", i, p->ctrl);
		for (int j = 0; j < 32; j++) { char b[64]; if (p->imem[j]) n += snprintf(buf + n, sizeof buf - n, "  imem[%2d] = %04x  %s\n", j, p->imem[j], um2bus_pio_dis(p->imem[j], b, sizeof b)); }
		for (int j = 0; j < 4; j++) { Sm *s = &p->sm[j]; n += snprintf(buf + n, sizeof buf - n, "  SM%d clkdiv=%08x execctrl=%08x shiftctrl=%08x pinctrl=%08x instr=%04x pc=%u en=%d txq=%u\n", j, s->clkdiv, s->execctrl, s->shiftctrl, s->pinctrl, s->instr_last, s->pc, s->enabled, s->count); }
	}
	fwrite(buf, 1, (size_t)n, f);
}

void um2bus_stats(FILE *f) {
	if (getenv("UM2_PIODUMP")) um2bus_dump_pio(f);
	/* may run from atexit() inside a sink call that holds the lock: do not block */
#ifndef UM2BUS_NO_THREADS
	int locked = pthread_mutex_trylock(&g_bus) == 0;
#endif
	fprintf(f, "um2bus: words pushed %llu (write %llu, silence %llu, park %llu = %llu periods)  pio instrs %llu\n",
		(unsigned long long)S.pushes, (unsigned long long)S.wr_words, (unsigned long long)S.silence_words, (unsigned long long)S.park_words, (unsigned long long)S.park_cycles, (unsigned long long)S.pio_instrs);
	fprintf(f, "um2bus: stream latches write %llu, non-VIA %llu; SIO: writes %llu reads %llu clock edges %llu\n",
		(unsigned long long)S.latches_write, (unsigned long long)S.non_via_writes, (unsigned long long)S.sio_writes, (unsigned long long)S.sio_reads, (unsigned long long)S.sio_edges);
	if (g_spi_sr || g_spi_drr || g_spi_drw) fprintf(f, "um2bus: SPI0 DR writes %lu, DR reads %lu, SR reads %lu, DR-read hash %016llx\n", g_spi_drw, g_spi_drr, g_spi_sr, (unsigned long long)g_spi_h);
	fprintf(f, "um2bus: DMA runs %llu words %llu; frames %llu (forced %llu) cycles/frame min %llu avg %llu max %llu; idle pad %llu; unsupported %llu\n",
		(unsigned long long)S.dma_runs, (unsigned long long)S.dma_words, (unsigned long long)S.frames, (unsigned long long)S.forced_frames,
		(unsigned long long)S.frame_cycles_min, (unsigned long long)(S.frames ? S.frame_cycles_sum / S.frames : 0), (unsigned long long)S.frame_cycles_max,
		(unsigned long long)S.idle_pad_cycles, (unsigned long long)S.unsupported);
	if (AS.starts || AS.cpu_words) {
		fprintf(f, "um2bus: cart audio: %llu transfers started, %llu blocks completed (%llu interrupts), %llu words consumed in %llu steps, %llu aborts, %llu CPU pushes to PIO1/2;",
			(unsigned long long)AS.starts, (unsigned long long)AS.blocks, (unsigned long long)AS.irqs, (unsigned long long)AS.words, (unsigned long long)AS.steps, (unsigned long long)AS.aborts, (unsigned long long)AS.cpu_words);
		if (C.sysclk) fprintf(f, " clk_sys %.1f MHz;", C.sysclk() * 1e-6);
		for (int p = 1; p < NPIO; p++) for (int s = 0; s < 4; s++) if (AS.fps[p][s] > 0) fprintf(f, " PIO%d SM%d %.1f frames/s (%d-bit)", p, s, AS.fps[p][s], AS.bits[p][s]);
		fputc('\n', f);
		fprintf(f, "um2bus: audio thread signalled by aud_start %lu times, signal skipped %lu (wake predicted from the block end)\n", g_aud_sig, g_aud_skip);
		if (AS.blocks) fprintf(f, "um2bus: cart audio block restarts after the previous block ended: <1 ms %llu, <3 ms %llu, <10 ms %llu, <30 ms %llu, later (underrun, silence inserted) %llu; worst %.1f ms\n",
			(unsigned long long)AS.late[0], (unsigned long long)AS.late[1], (unsigned long long)AS.late[2], (unsigned long long)AS.late[3], (unsigned long long)AS.late[4], (double)AS.maxlate * 1e-6);
		if (DF.n) fprintf(f, "um2bus: ring fill (guest write index - READ_ADDR, UM2_AUDFILL): min %d mean %.0f max %d frames of %u, %llu of %llu samples above 7/8 (an overrun of the reader = the game resyncs)\n", DF.mn, DF.sum / (double)DF.n, DF.mx, 2048u, (unsigned long long)DF.bad, (unsigned long long)DF.n);
		um2aud_stats(f);
	}
	if (g_busstat) for (int i = 0; i < 12; i++) if (g_bst[i].n) fprintf(f, "um2bus: BUSSTAT %-10s n %llu, wait avg %.1f us (>100us: %llu), hold avg %.1f us, max %.0f us, holds >100us %llu\n", g_bst_name[i], (unsigned long long)g_bst[i].n, g_bst[i].wait / (double)g_bst[i].n / 1e3, (unsigned long long)g_bst[i].bigw, g_bst[i].hold / (double)g_bst[i].n / 1e3, g_bst[i].max / 1e3, (unsigned long long)g_bst[i].big);
	if (g_v2_words) fprintf(f, "um2bus: v2 engine: %llu words (%llu reads), %llu E cycles\n", g_v2_words, g_v2_reads, g_v2_cycles);
	if (g_wall1 > g_wall0) fprintf(f, "um2bus: %llu frames in %.1f s = %.1f fps (%.1f ms per frame), measured from the first frame mark\n", (unsigned long long)(S.frames - g_frames0), g_wall1 - g_wall0, (S.frames - g_frames0) / (g_wall1 - g_wall0), (g_wall1 - g_wall0) * 1e3 / (double)(S.frames - g_frames0));
	if (g_cap) fflush(g_cap);
	um2aud_finish();                            /* the .wav header (the time-limit exit does not run atexit handlers) */
#ifndef UM2BUS_NO_THREADS
	if (locked) pthread_mutex_unlock(&g_bus);
#endif
}

/* ------------------------------------------------------------------ PIO disassembler (logs) */
const char *um2bus_pio_dis(uint16_t ins, char *b, size_t n) {
	static const char *jc[] = { "", "!x,", "x--,", "!y,", "y--,", "x!=y,", "pin,", "!osre," };
	static const char *od[] = { "pins", "x", "y", "null", "pindirs", "pc", "isr", "exec" };
	static const char *md[] = { "pins", "x", "y", "pindirs", "exec", "pc", "isr", "osr" };
	static const char *ms[] = { "pins", "x", "y", "null", "?", "status", "isr", "osr" };
	static const char *sd[] = { "pins", "x", "y", "?", "pindirs", "?", "?", "?" };
	unsigned a = ins & 0xFF, dl = (ins >> 8) & 31;
	switch (ins >> 13) {
	case 0: snprintf(b, n, "jmp %s%u [%u]", jc[(a >> 5) & 7], a & 31, dl); break;
	case 1: snprintf(b, n, "wait %u %s %u [%u]", (a >> 7) & 1, ((a >> 5) & 3) == 0 ? "gpio" : ((a >> 5) & 3) == 1 ? "pin" : ((a >> 5) & 3) == 2 ? "irq" : "jmppin", a & 31, dl); break;
	case 2: snprintf(b, n, "in ... %u [%u]", a & 31, dl); break;
	case 3: snprintf(b, n, "out %s, %u [%u]", od[(a >> 5) & 7], (a & 31) ? (a & 31) : 32, dl); break;
	case 4: if (a & 0x80) snprintf(b, n, "pull %s%s [%u]", (a & 0x40) ? "ifempty " : "", (a & 0x20) ? "block" : "noblock", dl); else snprintf(b, n, "push [%u]", dl); break;
	case 5: snprintf(b, n, "mov %s, %s%s [%u]", md[(a >> 5) & 7], ((a >> 3) & 3) == 1 ? "~" : ((a >> 3) & 3) == 2 ? "::" : "", ms[a & 7], dl); break;
	case 6: snprintf(b, n, "irq %02x [%u]", a, dl); break;
	default: snprintf(b, n, "set %s, %u [%u]", sd[(a >> 5) & 7], a & 31, dl); break;
	}
	return b;
}

/* ------------------------------------------------------------------ replay (host tool) */
int um2bus_replay(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	CapRec r; unsigned long n = 0;
	g_replaying = 1;
	while (fread(&r, sizeof r, 1, f) == 1) {
		n++;
		switch (r.op) {
		case CAP_PIO_ST: um2bus_store(r.addr, r.size, r.val, 1); break;
		case CAP_PIO_PUSH: BUS_LOCK_T(9); pio_txf_push((int)(r.addr / 4), (int)(r.addr % 4), r.val); BUS_UNLOCK_T(9); break;
		case CAP_SIO_ST: um2bus_store(r.addr, r.size, r.val, 1); break;
		case CAP_SIO_GPIN: { uint32_t v; um2bus_load(r.addr, 4, &v, 1); break; }
		}
	}
	fclose(f);
	g_replaying = 0;
	fprintf(stderr, "um2bus: replayed %lu records\n", n);
	return 0;
}

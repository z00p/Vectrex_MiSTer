/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2run -- run an unmodified UVMC2 cartridge image (.um2, an RP2350 / Cortex-M33 RAM image) on the MiSTer's Cortex-A9.
 *
 * The image is loaded at 0x20000000 inside a Linux/ARMv7-A process and started at its reset vector in Thumb state.
 * What an A9 cannot execute or reach is handled here: svc calls and ARMv8-M exception entry/return, M33-only
 * instructions (SDIV/UDIV, VSEL, LDA/STL, MRS/MSR PRIMASK, ...: emulated at the first trap, then patched to a stub),
 * and the RP2350 peripherals the cartridge uses (PIO, DMA, SIO, SPI0 + an SD card model, TIMER0 alarms, NVIC, QMI/PSRAM,
 * cart audio), which are modelled in um2bus.c / um2sd.c / um2aud.c. The bus traffic that results becomes a timed
 * stream of VIA reads and writes for the Vectrex-PiTrex core (src/ptx_*.c). See DESIGN.md.
 *
 * Without --bus it is a census logger: it runs the image and reports every access it cannot execute (the original
 * purpose of this tool, kept for analysing new images).
 *
 * Trap back-ends (--mode):
 *   ptrace  (default, census only, needs no seccomp): a tracer parent runs the guest in a forked child.
 *   inproc  : in-process signal handlers. svc must then be neutralised: either --patch-svc (rewrites every 0xDFnn
 *           halfword to udf 0xDEnn) or build with -DUSE_SECCOMP (seccomp-bpf SIGSYS trap). --bus uses inproc.
 *           Also used for qemu-user testing, where ptrace() is not implemented.
 *
 * --parse does a static census only (header, vector table, SVCall handler chain, opcode scan) and works on any host.
 *
 * Build: see build.sh.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <sched.h>
#include <sys/resource.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "um2bus.h"
#include "um2sd.h"
#include "um2aud.h"
#include "um2clock.h"
#include "um2card_lock.h"
#include "ptx.h"
#include "ptx_font.h"
#ifdef __arm__
#include <sys/ptrace.h>
#include <sys/user.h>
#ifdef USE_SECCOMP
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <stddef.h>
#endif
#endif

#define IMG_BASE 0x20000000u
#define IMG_TOP  0x20082000u        /* top of RP2350 SRAM == initial SP of every image */
#define ROM_FUNCTAB 0x7C00u
#define ROM_LOOKUP  0x7D01u     /* fake rom_table_lookup entry (Thumb) */
static uint8_t *g_bootrom;      /* --bootrom: the 32 KB RP2350 boot ROM image, else the boot ROM area reads as zero (plus the header words above) */
static uint32_t g_romcode[64], g_nrom;
#define PSRAM_BASE 0x11000000u
#define PSRAM_SIZE (8u << 20)

/* ------------------------------------------------------------------ options / globals */
static struct {
    int parse, ptrace_mode, patch_svc, unsafe, quiet, no_psram, no_canned;
    uint64_t max_events, every;
    double secs;
    int top, logmax, trace;
    const char *file;
    int bus, bus_verbose;             /* --bus: real svc dispatch + device model + ptx sink */
    const char *capture;
    unsigned frame_cycles;
} opt = { 0, 1, 0, 0, 0, 0, 0, 200000, 20000, 20.0, 30, 400, 0, NULL, 0, 0, NULL, 30000 };
static void bus_finish(void);
static void rh_dump(void);
uint64_t g_trap_ns, g_trap_n;   /* time spent inside the trap handler body (not the kernel's signal path) and count, for the profile line */
static __thread int tl_core;          /* 0 = main thread (core 0), 1 = the core-1 thread */

/* static (not malloc'd): a >128 KB malloc is mmap'd, and under qemu-user that lands in the 0x40000000 MMIO window */
static uint8_t g_file[1 << 20]; static size_t g_filesz;
static uint32_t g_entry;
static uint64_t g_events, g_next_report;
static uint32_t g_ring[32][3], g_lastpc;           /* last 32 traps: sig, pc, fault address */
static int g_live, g_opt_rt, g_opt_cpu = -1, g_opt_nonice;
static unsigned g_ticks;
static char g_outbuf[1 << 18];
static double g_t0;
static int g_logged;
static char g_patched[(IMG_TOP - IMG_BASE) / 2 / 8 + 1];   /* bitmap: halfword was 0xDFnn, patched */
static uint8_t g_patch_orig_n[(IMG_TOP - IMG_BASE) / 2];

static double now(void) { return (double)um2_clock_ns() * 1e-9; }
/* CLOCK_MONOTONIC is seconds since boot: *1e6 exceeds 2^32 after 71.6 min of uptime and a double->uint32 cast then SATURATES on ARM (timer stuck at 0xFFFFFFFF). Go through uint64 and wrap. */
static double up_add(void) { static double add = -1; if (add < 0) { const char *e = getenv("UM2_UPTIME_ADD"); add = e ? atof(e) : 0; } return add; }   /* test hook: pretend a long uptime */
static uint32_t micros_ts(const struct timespec *t) { return (uint32_t)((uint64_t)((t->tv_sec + t->tv_nsec * 1e-9 + up_add()) * 1e6)); }
static uint32_t micros(void) { return (uint32_t)((uint64_t)((double)um2_clock_ns() * 1e-3 + up_add() * 1e6)); }   /* micros_ts without the timespec (no 64-bit divide on this hot path) */

static void logf_(const char *fmt, ...) {
    if (opt.quiet || g_logged >= opt.logmax) return;
    g_logged++;
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap); if (g_live) fflush(stdout);
    if (g_logged == opt.logmax) { fputs("(first-occurrence log limit reached; see summary)\n", stdout); if (g_live) fflush(stdout); }
}

/* ------------------------------------------------------------------ Thumb helpers */
static int t32(uint16_t h1) { return (h1 & 0xF800) >= 0xE800; }

/* Classify an instruction that an ARMv7-A Cortex-A9 may not execute (or that matters for the port).
 * Returns a short static string, or NULL for "ordinary". */
static const char *classify(uint16_t h1, uint16_t h2, int len, char *buf, size_t bs) {
    if (len == 2) {
        if ((h1 & 0xFF00) == 0xDF00) { snprintf(buf, bs, "svc #%u", h1 & 0xFF); return buf; }
        if ((h1 & 0xFF00) == 0xDE00) return "udf";
        if ((h1 & 0xFF00) == 0xBE00) return "bkpt";
        if ((h1 & 0xFFE8) == 0xB660) return (h1 & 0x10) ? "cpsid" : "cpsie";
        if (h1 == 0xBF20 || h1 == 0xBF30) return h1 == 0xBF20 ? "wfe" : "wfi";
        if (h1 == 0xBF40) return "sev";
        if ((h1 & 0xFF87) == 0x4700 || (h1 & 0xFF87) == 0x4780) return NULL;
        return NULL;
    }
    if ((h1 & 0xFFF0) == 0xFB90 && (h2 & 0xF0F0) == 0xF0F0) return "SDIV (no idiv on A9)";
    if ((h1 & 0xFFF0) == 0xFBB0 && (h2 & 0xF0F0) == 0xF0F0) return "UDIV (no idiv on A9)";
    if (h1 == 0xF3EF && (h2 & 0xF000) == 0x8000) return "MRS (M-profile sysreg; may silently decode as A-profile MRS)";
    if ((h1 & 0xFFF0) == 0xF380 && (h2 & 0xFF00) == 0x8800) return "MSR (M-profile sysreg; may silently decode as A-profile MSR)";
    if (h1 == 0xF3BF && (h2 & 0xFFF0) == 0x8F40) return "dsb";
    if (h1 == 0xF3BF && (h2 & 0xFFF0) == 0x8F50) return "dmb";
    if (h1 == 0xF3BF && (h2 & 0xFFF0) == 0x8F60) return "isb";
    if ((h2 & 0x0E00) == 0x0A00 || 1) {
        int vfp = ((h2 & 0x0E00) == 0x0A00);
        if (vfp) {
            if ((h1 & 0xFFB0) == 0xEEA0 || (h1 & 0xFFB0) == 0xEE90) { snprintf(buf, bs, "VFMA/VFMS/VFNMA/VFNMS (VFPv4; no vfpv4 on A9)"); return buf; }
            if ((h1 & 0xFF80) == 0xFE00 && (h2 & 0x0050) == 0x0000) return "VSEL (ARMv8 FP)";
            if ((h1 & 0xFFB0) == 0xFE80 && (h2 & 0x0010) == 0x0000) return (h2 & 0x0040) ? "VMINNM (ARMv8 FP)" : "VMAXNM (ARMv8 FP)";
            if ((h1 & 0xFFBC) == 0xFEB8 && (h2 & 0x0050) == 0x0040) return "VRINTA/N/P/M (ARMv8 FP)";
            if ((h1 & 0xFFBE) == 0xEEB6 && (h2 & 0x0050) == 0x0040) return "VRINTR/X/Z (ARMv8 FP)";
            if ((h1 & 0xFFBC) == 0xFEBC) return "VCVTA/N/P/M (ARMv8 FP)";
            if ((h1 & 0xFFBE) == 0xEEB2 && (h2 & 0x0F50) == 0x0A40) return "VCVTB/T half (FP16)";
        }
    }
    if ((h1 & 0xFFF0) == 0xE8D0 && ((h2 & 0x0FCF) == 0x0F8F || (h2 & 0x0FEF) == 0x0FEF || (h2 & 0x0FEF) == 0x0FCF)) return "LDA/LDAEX (ARMv8 acquire)";
    if ((h1 & 0xFFF0) == 0xE8C0 && (h2 & 0x0FCF) == 0x0F8F) return "STL (ARMv8 release)";
    /* coprocessor space: MCR/MRC/MCRR/MRRC/CDP/LDC/STC; cp10/11 are VFP/NEON and fine */
    if ((h1 & 0xEC00) == 0xEC00) {
        unsigned cp = (h2 >> 8) & 0xF;
        if (cp != 10 && cp != 11) { snprintf(buf, bs, "coproc p%u (%04x %04x)", cp, h1, h2); return buf; }
    }
    return NULL;
}

/* ------------------------------------------------------------------ census tables */
enum { K_ILL = 1, K_SEGV, K_BUS, K_TRAP, K_SVC, K_SVCN, K_ADDR, K_PC, K_OTHER, K_ROM, K_NKINDS };
static const char *kname[] = { "?", "ILL", "SEGV", "BUS", "TRAP", "SVC@pc", "SVC#", "ADDR", "PC", "OTHER", "ROM" };
typedef struct { uint8_t kind; uint32_t a, b; uint64_t n; uint32_t x, y, z; char note[48]; } Ev;
#define EVCAP 65536
static Ev g_ev[EVCAP];
static uint64_t g_kcount[K_NKINDS], g_kdistinct[K_NKINDS];

static Ev *ev_get(int kind, uint32_t a, uint32_t b, int *isnew) {
    uint32_t h = (a * 2654435761u) ^ (b * 40503u) ^ (kind * 977u);
    for (uint32_t i = 0; i < 64; i++) {
        Ev *e = &g_ev[(h + i) & (EVCAP - 1)];
        if (!e->kind) { e->kind = kind; e->a = a; e->b = b; if (isnew) *isnew = 1; g_kdistinct[kind]++; return e; }
        if (e->kind == kind && e->a == a && e->b == b) { if (isnew) *isnew = 0; return e; }
    }
    return NULL;
}

/* RP2350 block names (from memory of the datasheet map; verify before relying on them) */
static const char *blockname(uint32_t a) {
    static const struct { uint32_t base; const char *n; } t[] = {
        {0x40000000,"SYSINFO"},{0x40008000,"SYSCFG"},{0x40010000,"CLOCKS"},{0x40018000,"PSM"},{0x40020000,"RESETS"},
        {0x40028000,"IO_BANK0"},{0x40030000,"IO_QSPI"},{0x40038000,"PADS_BANK0"},{0x40040000,"PADS_QSPI"},
        {0x40048000,"XOSC"},{0x40050000,"PLL_SYS"},{0x40058000,"PLL_USB"},{0x40060000,"ACCESSCTRL"},{0x40068000,"BUSCTRL"},
        {0x40070000,"UART0"},{0x40078000,"UART1"},{0x40080000,"SPI0"},{0x40088000,"SPI1"},{0x40090000,"I2C0"},{0x40098000,"I2C1"},
        {0x400A0000,"ADC"},{0x400A8000,"PWM"},{0x400B0000,"TIMER0"},{0x400B8000,"TIMER1"},{0x400C0000,"HSTX_CTRL"},
        {0x400C8000,"XIP_CTRL"},{0x400D0000,"QMI"},{0x400D8000,"WATCHDOG"},{0x400E0000,"BOOTRAM"},{0x400E8000,"ROSC"},
        {0x400F0000,"TRNG"},{0x400F8000,"SHA256"},{0x40100000,"POWMAN"},{0x40108000,"TICKS"},{0x40120000,"OTP"},
        {0x50000000,"DMA"},{0x50100000,"USBCTRL"},{0x50200000,"PIO0"},{0x50300000,"PIO1"},{0x50400000,"PIO2"},
        {0x50500000,"XIP_AUX"},{0x50600000,"HSTX_FIFO"},{0xD0000000,"SIO"},{0xD0020000,"SIO_NS"},
        {0xE0000000,"PPB(ITM..)"},{0xE000E000,"PPB(SCS/NVIC/SysTick)"},{0xE0042000,"PPB"},
    };
    const char *best = NULL; uint32_t bb = 0;
    if (a >= 0x10000000 && a < 0x11000000) return "XIP flash";
    if (a >= 0x11000000 && a < 0x12000000) return "XIP PSRAM";
    if (a >= 0x13000000 && a < 0x14000000) return "XIP (nocache/maint)";
    if (a >= 0x15000000 && a < 0x16000000) return "XIP PSRAM (uncached)";
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++)
        if (a >= t[i].base && a - t[i].base < 0x8000 + ((t[i].base >= 0x50000000 && t[i].base < 0x60000000) ? 0xF8000 : 0) && t[i].base >= bb) { best = t[i].n; bb = t[i].base; }
    if (best) return best;
    if (a >= 0x40000000 && a < 0x60000000) return "periph?";
    if (a >= 0xE0000000) return "PPB?";
    return "unmapped";
}

/* ------------------------------------------------------------------ MMIO shadow / canned values */
#define SHCAP 8192
static struct { uint32_t a, v; uint8_t used; } g_sh[SHCAP];
static __thread int tl_sh_new;               /* the last sh_slot(.., 1) created the slot: first store to that word */
static uint32_t *sh_slot(uint32_t a, int create) {
    uint32_t h = (a >> 2) * 2654435761u;
    tl_sh_new = 0;
    for (uint32_t i = 0; i < SHCAP; i++) {
        uint32_t k = (h + i) & (SHCAP - 1);
        if (!g_sh[k].used) { if (!create) return NULL; g_sh[k].used = 1; g_sh[k].a = a; g_sh[k].v = 0; tl_sh_new = 1; return &g_sh[k].v; }
        if (g_sh[k].a == a) return &g_sh[k].v;
    }
    return NULL;
}
/* Unmodelled MMIO, for the end-of-run summary: noted once per word (first store that creates its shadow slot, first read from a pc of a register with nothing behind it). */
static struct { const char *blk; uint32_t first, pc; unsigned words, r, w; } g_um[64]; static int g_num; static pthread_mutex_t g_um_m = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_um_seen[1024];
static void um_note(uint32_t a, uint32_t pc, int store) {
    if (a < 0x40000000u || (a >= 0x60000000u && a < 0xD0000000u) || (a & 0xFFFF0000u) == 0x400B0000u || (a >= 0xE000E100u && a < 0xE000E500u)) return;   /* TIMER0/1 and the NVIC are modelled before the shadow */
    a &= ~3u;
    pthread_mutex_lock(&g_um_m);
    uint32_t h = (a >> 2) * 2654435761u >> 22; int dup = 0;
    for (int i = 0; i < 1024; i++) { uint32_t *s = &g_um_seen[(h + (uint32_t)i) & 1023]; if (*s == a) { dup = 1; break; } if (!*s) { *s = a; break; } }
    const char *bn = blockname(a); int j;
    for (j = 0; j < g_num; j++) if (g_um[j].blk == bn) break;
    if (j == g_num && g_num < 64) { g_um[j].blk = bn; g_um[j].first = a; g_um[j].pc = pc; g_num++; }
    if (j < g_num) { if (!dup) g_um[j].words++; if (store) g_um[j].w++; else g_um[j].r++; }
    pthread_mutex_unlock(&g_um_m);
}
static uint32_t g_bootlock;                  /* BOOTRAM BOOTLOCK0..7 held (a read claims a free lock and returns nonzero, any write releases) */
static volatile uint32_t g_clkgen;       /* bumped after every store to CLOCKS / PLL_SYS (what sysclk_hz reads): the DWT fast path caches the clock rate per generation */
static void mmio_store_base_(uint32_t addr, int size, uint32_t val);
static void mmio_store_base(uint32_t addr, int size, uint32_t val) {
    mmio_store_base_(addr, size, val);
    if ((addr & 0xFFFF8000u) == 0x40010000u || (addr & 0xFFFF8000u) == 0x40050000u) __atomic_fetch_add(&g_clkgen, 1, __ATOMIC_SEQ_CST);
}
static void mmio_store_base_(uint32_t addr, int size, uint32_t val) {
    uint32_t base = addr & ~3u, sh = (addr & 3) * 8;
    if (base >= 0x400E080Cu && base <= 0x400E0828u) { __atomic_fetch_and(&g_bootlock, ~(1u << ((base - 0x400E080Cu) >> 2)), __ATOMIC_SEQ_CST); return; }
    if (size == 4 && addr >= 0x40000000 && addr < 0x60000000 && (addr & 0x3000)) {   /* atomic aliases */
        uint32_t *p = sh_slot(addr & ~0x3000u, 1); if (!p) return;
        if (tl_sh_new) um_note(addr & ~0x3000u, 0, 1);
        switch ((addr >> 12) & 3) { case 1: *p ^= val; break; case 2: *p |= val; break; case 3: *p &= ~val; break; }
        return;
    }
    uint32_t *p = sh_slot(base, 1); if (!p) return;
    if (tl_sh_new) um_note(base, 0, 1);
    if (size == 4) *p = val;
    else { uint32_t m = (size == 2 ? 0xFFFFu : 0xFFu) << sh; *p = (*p & ~m) | ((val << sh) & m); }
}

/* ---- interrupts (--bus) ---------------------------------------------------------------------------------------------
 * Some images (UVMC2 BattleZone) are interrupt driven: a TIMER0 alarm tick (SDK alarm pool, IRQ 3) sets the flag core 0 spins on and
 * a core-1 counter, DMA/PIO/spare IRQs run SDK workers.  Model: one NVIC per core (ISER/ICER/ISPR/ICPR/IPR; priority = top 4 bits,
 * equal priority never nests), TIMER0/1 alarms driven by a helper thread, level lines re-pended after every ISR, a soft PRIMASK
 * (MRS PRIMASK + CPSID I is emulated as one step, MSR PRIMASK writes it), and asynchronous delivery by SIGURG: the handler builds
 * the ARMv8-M exception frame on the guest stack and returns into the ISR (only when the interrupted pc is inside the image; in a
 * stub or helper it retries 150 us later).  ISR return is the same EXC_RETURN fetch fault as for SVCall. */
#define NIRQ 52
typedef struct {
    volatile uint32_t en[2], pend[2];       /* ISER / ISPR bits, IRQ 0..63 */
    volatile uint32_t prim;                 /* soft PRIMASK */
    uint8_t pri[64];                        /* IPR bytes (top 4 bits kept) */
    int depth; uint8_t sprio[8]; int8_t sirq[8];   /* active exceptions: running priority and number (-1 = SVCall) */
    uint32_t sret[8];                       /* ... and the return pc in each one's frame (a patch must not cover one: patch_begin) */
    int8_t last;                            /* the IRQ taken last (round-robin among equal priorities, see nvic_pick) */
    uint32_t since, hold;                   /* burst guard: micros() when interrupt context was entered from thread mode, no delivery before `hold` */
    int held;
} Nvic;
static Nvic g_nv[2];
/* UM2_ISRSTAT=1 (diagnostics): per-ISR wall and thread-CPU time, MMIO accesses and traps inside the ISR, per core; printed with --bus-verbose at exit. */
static int g_isrstat;
static uint64_t ns_wall(void) { return um2_clock_ns(); }
static uint64_t ns_cpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static __thread uint64_t tl_bus_ns, tl_n_ml, tl_n_ms, tl_n_tl, tl_n_ts, tl_n_trap, tl_ts_ns, tl_is_w, tl_is_c, tl_is_s[8];
static struct { uint64_t n, wall, cpu, ml, ms, tl, ts, trap, ts_ns; uint32_t wh[14], ch[14]; } g_is[2];
static struct { uint64_t n, wall, cpu, bus, ml, ms, tr; uint32_t wh[14]; } g_isq[64];
static __thread int tl_is_irq; static __thread uint64_t tl_tm_t0; static uint64_t g_tm_gap_ns[2], g_tm_gaps[2], g_tm_gap_max[2];   /* thread-mode excursions (>= 20 us between two ISRs) */
static unsigned is_bin(uint64_t ns) { unsigned b = (unsigned)(ns / 50000); if (b > 9) b = ns < 2000000 ? 10 : ns < 5000000 ? 11 : ns < 20000000 ? 12 : 13; return b; }
static void isr_begin(int irq) { if (!g_isrstat) return; tl_is_irq = irq & 63; tl_is_w = ns_wall();
    if (tl_tm_t0) { uint64_t g = tl_is_w - tl_tm_t0; if (g >= 20000) { int c = tl_core & 1; g_tm_gap_ns[c] += g; g_tm_gaps[c]++; if (g > g_tm_gap_max[c]) g_tm_gap_max[c] = g; } } tl_is_c = ns_cpu(); tl_is_s[0] = tl_n_ml; tl_is_s[1] = tl_n_ms; tl_is_s[2] = tl_n_tl; tl_is_s[3] = tl_n_ts; tl_is_s[4] = tl_n_trap; tl_is_s[5] = tl_ts_ns; tl_is_s[6] = tl_bus_ns; }
static void isr_end(void) {
    if (!g_isrstat || !tl_is_w) return;
    int c = tl_core & 1; uint64_t w = ns_wall() - tl_is_w, cp = ns_cpu() - tl_is_c; tl_is_w = 0;
    tl_tm_t0 = ns_wall();
    g_is[c].n++; g_is[c].wall += w; g_is[c].cpu += cp; g_is[c].ml += tl_n_ml - tl_is_s[0]; g_is[c].ms += tl_n_ms - tl_is_s[1]; g_is[c].tl += tl_n_tl - tl_is_s[2]; g_is[c].ts += tl_n_ts - tl_is_s[3]; g_is[c].trap += tl_n_trap - tl_is_s[4]; g_is[c].ts_ns += tl_ts_ns - tl_is_s[5];
    g_is[c].wh[is_bin(w)]++; g_is[c].ch[is_bin(cp)]++;
    g_isq[tl_is_irq].bus += tl_bus_ns - tl_is_s[6]; g_isq[tl_is_irq].ml += tl_n_ml - tl_is_s[0]; g_isq[tl_is_irq].ms += tl_n_ms - tl_is_s[1]; g_isq[tl_is_irq].tr += tl_n_trap - tl_is_s[4];
    g_isq[tl_is_irq].n++; g_isq[tl_is_irq].wall += w; g_isq[tl_is_irq].cpu += cp; g_isq[tl_is_irq].wh[is_bin(w)]++;
}
static void isr_dump(void) {
    if (!g_isrstat) return;
    for (int c = 0; c < 2; c++) { if (!g_is[c].n) continue; double n = (double)g_is[c].n;
        printf("-- ISRSTAT core %d: %llu ISRs; per ISR: wall %.1f us, thread CPU %.1f us, MMIO loads %.1f (timer %.1f), stores %.1f (timer %.1f, %.1f us in tmr_access), traps %.2f\n", c, (unsigned long long)g_is[c].n,
               g_is[c].wall / n / 1e3, g_is[c].cpu / n / 1e3, g_is[c].ml / n, g_is[c].tl / n, g_is[c].ms / n, g_is[c].ts / n, g_is[c].ts_ns / n / 1e3, g_is[c].trap / n);
        printf("   wall histogram (50 us bins x9, then >=450us, >=2ms, >=5ms, >=20ms):"); for (int i = 0; i < 14; i++) printf(" %u", g_is[c].wh[i]);
        printf("\n   cpu  histogram (same bins):"); for (int i = 0; i < 14; i++) printf(" %u", g_is[c].ch[i]); printf("\n"); }
    for (int c = 0; c < 2; c++) if (g_tm_gaps[c]) printf("   core %d thread mode: %llu excursions (>= 20 us), %.1f ms in total, longest %.2f ms\n", c, (unsigned long long)g_tm_gaps[c], g_tm_gap_ns[c] / 1e6, g_tm_gap_max[c] / 1e6);
    for (int q = 0; q < 64; q++) if (g_isq[q].n) { printf("   IRQ %2d: %llu ISRs, wall %.1f us, cpu %.1f us, in um2bus %.1f us, MMIO ld %.1f st %.1f traps %.2f; wall hist", q, (unsigned long long)g_isq[q].n, g_isq[q].wall / (double)g_isq[q].n / 1e3, g_isq[q].cpu / (double)g_isq[q].n / 1e3, g_isq[q].bus / (double)g_isq[q].n / 1e3, g_isq[q].ml / (double)g_isq[q].n, g_isq[q].ms / (double)g_isq[q].n, g_isq[q].tr / (double)g_isq[q].n); for (int i = 0; i < 14; i++) printf(" %u", g_isq[q].wh[i]); printf("\n"); }
}
static pthread_t g_th_core0, g_th_core1; static volatile int g_have_core1;
void um2_rt_boost(int delta);
static volatile int g_irq_on;              /* some IRQ was enabled: the trap path polls only then */
static int g_irq_thunk = 1;                 /* ISRs return through the native um2_irqret thunk instead of an EXC_RETURN fetch fault (UM2_IRQSLOWRET=1: old way) */
void um2_irqret(void);                      /* ARM-state thunk, see below */
typedef struct { uint32_t target[4], armed, intr, inte, intf; } Tmr;
static Tmr g_tm[2];
static pthread_mutex_t g_irq_m = PTHREAD_MUTEX_INITIALIZER;
/* Alarm / interrupt machinery, 28 Oct 2026 (DESIGN.md, "Cart audio"): ALARM stores whose target has already passed are fired inline (no irq-thread round trip), the irq thread
 * only runs for alarms in the future, sleeps on a futex word (lock-free wake-ups, also from the SIGURG handler when a delivery was refused), and the burst guard is off by default.
 * The irq thread sleeps on a futex word instead of a condition variable (28 Oct 2026): irq_wake() is lock-free and async-signal-safe (the SIGURG handler calls it when a delivery
 * was refused), and a wake-up that arrives while the thread is between two sleeps is not lost (the word changed since the thread read it). */
static uint32_t g_irq_seq, g_irq_waiting;
static void irq_wake(void) {
    __atomic_add_fetch(&g_irq_seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_irq_waiting, __ATOMIC_SEQ_CST)) syscall(SYS_futex, &g_irq_seq, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}
static uint32_t g_sleep_until, g_tm_gen; static int g_irq_sleeping;   /* irq thread: sleeps until g_sleep_until (valid while g_irq_sleeping); g_tm_gen: bumped when an alarm is armed (under g_irq_m) */
static int g_alarm_fast = 1;                /* UM2_NOALARMFAST=1: every timer store wakes the irq thread and a past ALARM target is fired by it (the 27 Oct behaviour) */
/* A SIGURG that could not be delivered (the guest thread was in helper code, or in an IT block) must be retried: the irq thread does that every 150 us while it is awake, but it may be
 * asleep until the next alarm.  Wake it only when it sleeps long, otherwise its own retry loop runs (a wake per refusal would make the kick-refuse-wake cycle spin). */
static void irq_wake_retry(void);
static unsigned long g_al_inline, g_al_future, g_al_signal, g_irq_wakes, g_irq_fires;
static unsigned long g_irqf_pc, g_irqf_prim, g_irqf_other, g_irqs_stub, g_ie_why[4], g_irq_bursts;
static unsigned g_marks, g_cen_m0;           /* frame marks so far / at the census reset */
static unsigned long g_irq_cnt[NIRQ], g_irq_kicks, g_irq_pc[2], g_irq_async[2], g_irq_chain, g_irq_trap;   /* g_irq_pc: taken per core, g_irq_async: taken from the SIGURG handler */
static int nvic_pick(Nvic *n);
static int irq_levels(void);
static uint64_t irq_lines(void) {
    uint64_t m = 0;
    for (int t = 0; t < 2; t++) m |= (uint64_t)((g_tm[t].intr | g_tm[t].intf) & g_tm[t].inte & 0xF) << (4 * t);
    m |= um2bus_irq_lines();
    return m;
}
static int irq_kick(void) {                  /* signal every core that has an enabled pending IRQ; returns how many */
    int k = 0;
    for (int c = 0; c < 2; c++) {
        Nvic *n = &g_nv[c];
        if (!n->prim && nvic_pick(n) >= 0 && (c == 0 || g_have_core1)) { pthread_kill(c ? g_th_core1 : g_th_core0, SIGURG); k++; g_irq_kicks++; }   /* only what could preempt now */
    }
    return k;
}
static int irq_levels(void) {               /* level lines -> pending on both cores (a line nobody cleared pends again), then kick */
    uint64_t m = irq_lines();
    for (int c = 0; c < 2; c++) {
        if ((uint32_t)m) __atomic_fetch_or(&g_nv[c].pend[0], (uint32_t)m, __ATOMIC_SEQ_CST);
        if ((uint32_t)(m >> 32)) __atomic_fetch_or(&g_nv[c].pend[1], (uint32_t)(m >> 32), __ATOMIC_SEQ_CST);
    }
    return irq_kick();
}
static uint32_t g_irq_burst = 0, g_irq_gap = 1000;   /* burst guard, see irq_held(); off by default since 28 Oct 2026 */
static uint32_t g_irq_min;                  /* UM2_IRQMIN=<us>: the IRQ THREAD fires a due alarm at most this often (0 = every alarm, as the hardware does).  NOTE (28 Oct 2026, A9): this does not coalesce BattleZone's ticks.
                                              * The SDK alarm pool re-arms a late repeating alarm and forces the interrupt itself (INTF), and the level lines are re-pended at every ISR exit, so the thread is not
                                              * on that path; one ISR runs exactly one 6502 slice anyway, so a coalesced tick would be a lost tick.  IRQMIN=500: 6700-6900 ISRs/s, as without.  It only turns the inline fast path off. */
static int tmr_access(int store, uint32_t addr, uint32_t *val) {
    int t = (addr & 0x8000) ? 1 : 0; uint32_t off = addr & 0xFFF; unsigned alias = (addr >> 12) & 3;
    Tmr *m = &g_tm[t]; int chg = 0, h = 0;
    if (!store && off != 0x20 && off != 0x3C && off != 0x40 && off != 0x44 && off != 0x48) return 0;    /* clock reads (TIMELR/TIMEHR/TIMERAW*) have no alarm state: no lock */
    pthread_mutex_lock(&g_irq_m);
    if (!store) {
        h = 1;
        switch (off) { case 0x20: *val = m->armed; break; case 0x3C: *val = m->intr; break; case 0x40: *val = m->inte; break;
                       case 0x44: *val = m->intf; break; case 0x48: *val = (m->intr | m->intf) & m->inte; break; default: h = 0; }
        pthread_mutex_unlock(&g_irq_m); return h;
    }
    uint32_t v = *val; uint64_t tsn = g_isrstat ? ns_wall() : 0; tl_n_ts++; int sig = 0, lines = 0;
    switch (off) {
    case 0x10: case 0x14: case 0x18: case 0x1C: { unsigned n = (off - 0x10) >> 2;
        static int alog = -1; if (alog < 0) alog = getenv("UM2_ALARMLOG") != NULL;
        if (alog) { static unsigned shown; static uint32_t last; uint32_t nw = micros(); static unsigned cnt; if ((cnt++ % 100) == 0 && shown++ < 4000) fprintf(stderr, "alarm%u t%d target-now=%d target-prevtarget=%u core%d @%u\n", n, t, (int32_t)(v - nw), v - last, tl_core & 1, nw); last = v; }
        m->target[n] = v; m->armed |= 1u << n; chg = 1;
        if (!g_alarm_fast) { sig = 1; break; }
        /* Alarm fast path (28 Oct 2026).  A target that has already passed (the SDK alarm pool re-arms a late repeating alarm for prev + 166 us, 0.4 s in the past) is fired right here, as
         * the irq thread would a moment later: no wake-up, no two context switches on the guest's CPU.  A future target only wakes the irq thread when it is earlier than the time
         * that thread already sleeps until. */
        if ((int32_t)(v - micros()) <= 0 && !(g_irq_min)) { m->armed &= ~(1u << n); m->intr |= 1u << n; lines = 1; g_al_inline++; }
        else { g_tm_gen++; g_al_future++; if (!g_irq_sleeping || (int32_t)(v - g_sleep_until) < 0) sig = 1; }
        break; }
    case 0x20: m->armed &= ~v; chg = 1; sig = !g_alarm_fast; break;
    case 0x3C: m->intr &= ~v; chg = 1; sig = !g_alarm_fast; break;
    case 0x40: m->inte = (alias == 0 ? v : alias == 1 ? (m->inte ^ v) : alias == 2 ? (m->inte | v) : (m->inte & ~v)) & 0xF; chg = 1; lines = 1; sig = !g_alarm_fast; break;
    case 0x44: m->intf = (alias == 0 ? v : alias == 1 ? (m->intf ^ v) : alias == 2 ? (m->intf | v) : (m->intf & ~v)) & 0xF; chg = 1; lines = 1; sig = !g_alarm_fast; break;
    default: break;
    }
    if (sig) g_al_signal++;
    pthread_mutex_unlock(&g_irq_m);
    if (sig) irq_wake();                      /* after the unlock: the woken irq thread (higher priority, same CPU) would run at once and block on the mutex we still hold */
    if (chg && g_irq_on && (lines || !g_alarm_fast)) irq_levels();
    if (tsn) tl_ts_ns += ns_wall() - tsn;
    return 0;                                 /* stores also reach the shadow (harmless) */
}
static void irq_wake_retry(void) { static int off = -1; if (off < 0) off = getenv("UM2_NOIRQRETRY") != NULL; if (!off && g_irq_sleeping && (int32_t)(g_sleep_until - micros()) > 300) irq_wake(); }       /* UM2_NOIRQRETRY=1 (test): a refused delivery is not retried early */
static void *irq_thread(void *arg) {
    (void)arg; uint32_t last_fire = 0;
    pthread_setname_np(pthread_self(), "u2irq");
    um2_rt_boost(2);                         /* like the other helpers: an alarm must not wait behind the guest threads */
    pthread_mutex_lock(&g_irq_m);
    for (;;) {
        uint32_t seq = __atomic_load_n(&g_irq_seq, __ATOMIC_SEQ_CST);         /* before the scan: a wake-up after this point makes the futex wait below return at once */
        struct timespec ts; um2_mono(&ts);                                     /* the one clock read of this round: the sleep below ends `wait` after it (a few us early at most) */
        uint32_t nowu = micros_ts(&ts), gen = g_tm_gen; int64_t wait = 20000; int fired = 0;
        for (int t = 0; t < 2; t++) for (int n = 0; n < 4; n++) {
            if (!((g_tm[t].armed >> n) & 1)) continue;
            int32_t d = (int32_t)(g_tm[t].target[n] - nowu);
            if (d <= 0 && g_irq_min && (uint32_t)(nowu - last_fire) < g_irq_min) d = (int32_t)(g_irq_min - (nowu - last_fire));       /* coalescing: not yet (the guest's alarm pool catches up with what it missed) */
            if (d <= 0) { g_tm[t].armed &= ~(1u << n); g_tm[t].intr |= 1u << n; fired = 1; last_fire = nowu; g_irq_fires++; }
            else if (d < wait) wait = d;
        }
        int kicked = 0;
        if (fired) { pthread_mutex_unlock(&g_irq_m); kicked = irq_levels(); pthread_mutex_lock(&g_irq_m); }
        { int stuck = 0; for (int c = 0; c < 2; c++) { Nvic *n = &g_nv[c]; if (!n->prim && nvic_pick(n) >= 0) { if (n->held && (int32_t)(nowu - n->hold) < 0) { int32_t h = (int32_t)(n->hold - nowu); if (h < wait) wait = h; } else stuck = 1; } }
          if (stuck) { if (!kicked) irq_kick(); if (wait > 150) wait = 150; } }      /* just kicked: no second signal for the same tick (it found nothing pending in ~60 percent of the cases), look again in 150 us */
        if (g_tm_gen != gen) continue;          /* an alarm was armed while the lock was open: rescan before sleeping */
        g_sleep_until = nowu + (uint32_t)wait; g_irq_sleeping = 1;
        long ns = ts.tv_nsec + (long)(wait * 1000) % 1000000000L; long sec = ts.tv_sec + (long)(wait * 1000) / 1000000000L; if (ns >= 1000000000L) { ns -= 1000000000L; sec++; }
        struct { int32_t s, n; } kts = { (int32_t)sec, (int32_t)ns };         /* the kernel's 32-bit ARM timespec: CLOCK_MONOTONIC (seconds since boot) is far from 2038 */
        __atomic_store_n(&g_irq_waiting, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&g_irq_m);
        syscall(SYS_futex, &g_irq_seq, FUTEX_WAIT_BITSET_PRIVATE, seq, &kts, NULL, FUTEX_BITSET_MATCH_ANY);
        __atomic_store_n(&g_irq_waiting, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&g_irq_m);
        g_irq_sleeping = 0; g_irq_wakes++;
    }
    return NULL;
}
static void irq_init(void) {
    g_alarm_fast = getenv("UM2_NOALARMFAST") == NULL;
    g_irq_thunk = getenv("UM2_IRQSLOWRET") == NULL; g_isrstat = getenv("UM2_ISRSTAT") != NULL;
    { const char *e = getenv("UM2_IRQMIN"); g_irq_min = e ? (uint32_t)atoi(e) : 0; }
    { const char *e = getenv("UM2_IRQBURST"); if (e) g_irq_burst = (uint32_t)atoi(e); e = getenv("UM2_IRQGAP"); if (e) g_irq_gap = (uint32_t)atoi(e); }
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 16);
    if (!pthread_create(&t, &at, irq_thread, NULL)) pthread_detach(t);
}
static int nvic_access(int store, uint32_t addr, int size, uint32_t *val) {   /* NVIC at 0xE000E100..0xE000E4FF, banked per core */
    uint32_t off = addr - 0xE000E000u;
    if (off < 0x100 || off >= 0x500) return 0;
    Nvic *n = &g_nv[tl_core & 1];
    uint32_t v = store ? *val : 0;
    if (off >= 0x400) {
        unsigned i = off - 0x400;
        if (store) { for (int b = 0; b < size && i + b < 64; b++) n->pri[i + b] = (uint8_t)(v >> (8 * b)) & 0xF0; }
        else { uint32_t r = 0; for (int b = 0; b < size && i + b < 64; b++) r |= (uint32_t)n->pri[i + b] << (8 * b); *val = r; }
        return 1;
    }
    unsigned grp = off & ~0x7Fu, w = (off & 0x7F) >> 2;
    if (w > 1 || size != 4) { if (!store) *val = 0; return 1; }
    switch (grp) {
    case 0x100: if (store) { __atomic_fetch_or(&n->en[w], v, __ATOMIC_SEQ_CST); g_irq_on = 1; } else *val = n->en[w]; break;
    case 0x180: if (store) __atomic_fetch_and(&n->en[w], ~v, __ATOMIC_SEQ_CST); else *val = n->en[w]; break;
    case 0x200: if (store) __atomic_fetch_or(&n->pend[w], v, __ATOMIC_SEQ_CST); else *val = n->pend[w]; break;
    case 0x280: if (store) __atomic_fetch_and(&n->pend[w], ~v, __ATOMIC_SEQ_CST); else *val = n->pend[w]; break;
    case 0x300: if (!store) { uint32_t a = 0; for (int d = 0; d < n->depth; d++) if (n->sirq[d] >= 0 && (n->sirq[d] >> 5) == (int)w) a |= 1u << (n->sirq[d] & 31); *val = a; } break;
    default: return 0;
    }
    if (store && (grp == 0x100 || grp == 0x200)) irq_levels();
    return 1;
}
static uint32_t ipsr_now(void) { Nvic *n = &g_nv[tl_core & 1]; if (!n->depth) return 0; int i = n->sirq[n->depth - 1]; return i < 0 ? 11u : 16u + (uint32_t)i; }

/* A guest that polls the timer from the same instruction over and over is waiting (frame pacing, delays).  On this single-CPU
 * MiSTer a real-time spin at priority 50 would starve Main completely, so after a few dozen polls sleep a little per poll.
 * 28 Oct 2026: only a SPIN counts.  The BattleZone alarm ISR reads TIMELR at one pc once per tick (4-6 kHz), and between two of its reads it
 * stores to the alarm / interrupt registers, so it napped ~once per tick: a nanosleep(60 us) is >= 110 us on the A9 (timer slack), i.e. a
 * third of the tick period spent asleep in the interrupt path.  tl_act counts this thread's other MMIO accesses; a poll run that has any is not a wait.
 * UM2_NAPALL=1 = the old rule (every run of 24 reads at one pc). */
static __thread uint32_t tl_tpc, tl_tn, tl_act, tl_tact; static unsigned long g_naps[2];
static volatile int g_c1_own_cpu;   /* 1 once core 1 is on its own CPU (--cpu1): only then may core 0 skip the donate tick (and, with UM2_NONAP0, the timer-poll nap) */
static int g_cpu1 = -1;             /* --cpu1 N: pin the core-1 guest thread to CPU N (core 0 stays where --cpu put the process) */
static void timer_poll_nap(uint32_t pc) {
    static int off = -1, all = 0, free0 = 0; if (off < 0) { off = getenv("UM2_NONAP") != NULL; all = getenv("UM2_NAPALL") != NULL; free0 = getenv("UM2_NONAP0") != NULL; } if (off) return;
    /* UM2_NONAP0=1 (opt-in, untested on the A9): core 0 has CPU 0 to itself once core 1 sits on its own CPU (the same condition on_donate uses), so its polls spin instead of
     * sleeping; Main is on CPU 1.  A nap is >= 110 us whatever is asked, so every poll that ends a wait overshoots by up to that.  The helpers share CPU 0 but run above it. */
    if (free0 && g_cpu1 >= 0 && g_c1_own_cpu && !tl_core) return;
    if (pc == tl_tpc && (all || tl_act == tl_tact)) { if (++tl_tn > 24) { struct timespec ts = { 0, 60000 }; g_naps[tl_core & 1]++; nanosleep(&ts, NULL); } }
    else { tl_tpc = pc; tl_tn = 0; }
    tl_tact = tl_act;
}

/* clk_sys as the image configured it, from the shadow of its clock setup (XOSC 12 MHz -> PLL_SYS -> CLK_SYS_CTRL/DIV): 12 MHz (XOSC) until the image selects the PLL, 150 MHz if the aux source is not one we know.
 * Sets the I2S frame rate of the cart audio (PIO clkdiv is relative to it) and the DWT cycle counter below. */
static uint32_t shv(uint32_t a) { uint32_t *p = sh_slot(a, 0); return p ? *p : 0; }
static double sysclk_hz(void) {
    uint32_t ctrl = shv(0x4001003c), div = shv(0x40010040) >> 16, cs = shv(0x40050000), fb = shv(0x40050008) & 0xFFF, pr = shv(0x4005000c);
    if (div == 0) div = 1;
    if (!(ctrl & 1)) return 12e6 / div;                                             /* clk_ref (XOSC) */
    unsigned aux = (ctrl >> 5) & 7, p1 = (pr >> 16) & 7, p2 = (pr >> 12) & 7, rd = cs & 63;
    if (aux == 0 && fb && p1 && p2) return 12e6 / (rd ? rd : 1) * fb / (p1 * p2) / div;   /* PLL_SYS */
    return aux == 3 ? 12e6 / div : 150e6;
}
/* DWT_CYCCNT (per core).  The SDK's clock calibration counts N polled periods of the E clock (GPIO31 reads) between two reads of this counter, and the
 * cart-audio code then derives the I2S clock divider from the result.  Host time between the reads is meaningless there (a trap costs microseconds on
 * the A9, nanoseconds on a PC), so while the thread polls E edges the counter advances by sysclk/1.5 MHz cycles per polled period instead; without edges it
 * follows host time at sysclk.  UM2_NODWTFIX=1 (or UM2_NOAUDIO=1): the old 150 MHz host-time counter. */
static int g_spifast;          /* set at start unless UM2_NOSPIFAST=1 or UM2_ISRSTAT is on: the stubs take SPI0 DR / SR straight to um2bus_spi_load / um2bus_spi_dr_store (4 Nov 2026) */
static int g_spilean;          /* with g_spifast: stubs of SPI0 sites go through um2_hthunk_spild / _spist, no VFP save for DR / SR (10 Nov 2026; UM2_NOSPILEAN=1: um2_hthunk as before) */
static int g_srfast;           /* with g_spilean: SR loads skip the thunk entirely (um2_hthunk_spisr; off by default, UM2_SRFAST=1: on -- no gain measured beyond the noise) */
static __thread uint32_t tl_patch_addr;   /* the address of the access that asked for the patch now being built */
static int g_dwtfast = 1;        /* UM2_NODWTFAST=1: DWT_CYCCNT reads from the stubs take the generic MMIO path again (shadow lookups, census, loop breaker) and recompute the clock rate each time */
static __thread uint32_t tl_dwt, tl_dwt_us; static __thread uint64_t tl_dwt_e; static __thread int tl_dwt_init;
static uint32_t dwt_now(void) {
    static int off = -1; if (off < 0) off = getenv("UM2_NODWTFIX") != NULL || getenv("UM2_NOAUDIO") != NULL;
    uint32_t us = micros();
    if (off) return us * 150u;
    double hz; uint64_t e = um2bus_edges();
    if (g_dwtfast) { static __thread double chz; static __thread uint32_t cgen = ~0u; uint32_t gn = g_clkgen; if (gn != cgen) { chz = sysclk_hz(); cgen = gn; } hz = chz; }      /* per-thread cache, renewed when a clock register was written (UM2_NODWTFAST=1: recomputed on every read) */
    else hz = sysclk_hz();
    if (!tl_dwt_init) { tl_dwt_init = 1; tl_dwt_us = us; tl_dwt_e = e; }
    if (e != tl_dwt_e) tl_dwt += (uint32_t)((double)(e - tl_dwt_e) * (hz / 1.5e6) + 0.5);
    else tl_dwt += (uint32_t)((double)(uint32_t)(us - tl_dwt_us) * (hz / 1e6));
    tl_dwt_us = us; tl_dwt_e = e;
    { static int lg = -1; static unsigned shown; if (lg < 0) { const char *q = getenv("UM2_DWTLOG"); lg = q ? atoi(q) : 0; }     /* diagnostics: the first N counter reads of core 0 with the step since the previous one */
      if (lg && !tl_core && shown < (unsigned)lg) { static uint32_t last; fprintf(stderr, "dwt read %u: %u (+%u cycles = %.1f us at %.0f MHz)\n", shown++, tl_dwt, tl_dwt - last, (tl_dwt - last) / (hz / 1e6), hz / 1e6); last = tl_dwt; } }
    return tl_dwt;
}

static unsigned g_tm_reads[32], g_tm_broken[32];
static const uint32_t g_lb_cyc[15] = { 0xFFFFFFFFu, 0, 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x1000, 0x10000, 0x80000000u, 0xFFFF0000u };
static __thread uint32_t tl_lb_pc, tl_lb_a, tl_lb_act, tl_lb_run;
static uint32_t mmio_load_base(uint32_t addr, int size, uint32_t pc) {
    uint32_t base = addr & ~3u, v = 0, *p = sh_slot(base, 0);
    int known = 2, isnew = 0;                         /* known: 2 canned (a model), 1 only what the image wrote, 0 nothing to go by */
    if (p) v = *p;
    if (!opt.no_canned && addr < 0x8000) {            /* bootrom: fake header + function table pointers (guess at RP2350 layout) */
        if (g_bootrom) memcpy(&v, g_bootrom + base, 4);
        if ((addr & ~3u) == 0x10) v = 0x01754D;      /* 'M' 'u' 0x01 magic */
        else if ((addr & ~3u) == 0x14) v = ROM_FUNCTAB | ((uint32_t)ROM_LOOKUP << 16);   /* u16 @0x14 func table, u16 @0x16 lookup fn */
    } else if (!opt.no_canned) {
        uint32_t blk = addr & ~0x7FFFu, off = addr & 0x7FFF;
        if (base >= 0x400E080Cu && base <= 0x400E0828u) { uint32_t b = 1u << ((base - 0x400E080Cu) >> 2); v = (__atomic_fetch_or(&g_bootlock, b, __ATOMIC_SEQ_CST) & b) ? 0 : b; }   /* BOOTLOCKn */
        else if (base == 0x400E0808u) v = g_bootlock;                                  /* BOOTLOCK_STAT */
        else if (base == 0x400100A4u) v = 0x10;                                        /* FC0_STATUS: DONE (the frequency counter answers at once) */
        else if (base == 0x400100A8u) { uint32_t s = shv(0x400100A0) & 0xFF; double k = s == 5 || s == 8 ? 12000 : s == 2 || s == 0x0B ? 48000 : sysclk_hz() / 1000; v = (uint32_t)k << 5; }   /* FC0_RESULT (kHz << 5): xosc/clk_ref 12 MHz, pll_usb/clk_usb 48 MHz, else clk_sys (source numbers from the datasheet, unverified) */
        else if (addr == 0x40020008) v = 0xFFFFFFFFu;                                  /* RESETS.RESET_DONE: all done */
        else if (blk == 0x40050000 || blk == 0x40058000) v |= 0x80000000u;             /* PLL CS.LOCK */
        else if (blk == 0x40048000 || blk == 0x400E8000) v |= 0x80000000u;             /* XOSC/ROSC STATUS.STABLE (bit 31) */
        else if (blk == 0x40010000 && off <= 0x74 && off % 12 == 8) v = off == 0x38 ? 1u << (shv(0x40010030) & 3) : off == 0x44 ? 1u << (shv(0x4001003C) & 1) : 1;   /* CLK_x_SELECTED: one-hot SRC of the glitchless muxes (clk_ref, clk_sys; the SDK waits for == 1), else hardwired 1 */
        else if ((blk == 0x400B0000 || blk == 0x400B8000) && (off == 0x0C || off == 0x28)) { v = micros(); if (opt.bus) timer_poll_nap(pc); }  /* TIMELR/TIMERAWL */
        else if (addr == 0xE0001004) v = dwt_now();                                    /* DWT_CYCCNT */
        else if (addr == 0xE000E010) v |= 0x10000;                                     /* SYST_CSR.COUNTFLAG */
        else if (addr == 0xE000E018) v = (0xFFFFFF - (micros() * 150u)) & 0xFFFFFF;    /* SYST_CVR counts down */
        else if (addr == 0xD0000050) v = 3;                                            /* SIO FIFO_ST: VLD|RDY */
        else if (addr == 0xD0000058) { uint32_t *q = sh_slot(0xD0000054, 0); v = q ? *q : 0; }   /* FIFO_RD echoes FIFO_WR (core1 launch) */
        else if (addr == 0xD0000000) v = 0;                                            /* CPUID */
        else if (addr == 0xE000ED08 && !p) v = IMG_BASE;                               /* SCB.VTOR: the image's own vector table */
        else if (addr == 0xE000ED00) v = 0x410FD214;                                   /* SCB.CPUID: Cortex-M33 r0p4 */
        else known = p != NULL;
    } else known = p != NULL;
    /* Loop breaker: a pc that re-reads a register we know nothing about, with no other MMIO access in between, probably waits on a status bit; after 16 such reads it gets a
     * cycling list of values.  A register the image wrote gets the fakes OR-ed into what it wrote, and only after 256 such reads.  Never for a canned register (DWT, CLOCKS
     * SELECTED), nor for reads that merely repeat across calls (B11: the old count-per-pc rule faked DWT_CYCCNT in the clock calibration of every image, and config
     * read-modify-writes).  UM2_LBOLD=1: the old rule (any 16 reads at one pc). */
    static int tbrk = -1, lbold; if (tbrk < 0) { tbrk = getenv("UM2_TIMERBREAK") != NULL; lbold = getenv("UM2_LBOLD") != NULL; }
    int tmr = (addr & ~0x7FFFu) == 0x400B0000u || (addr & ~0x7FFFu) == 0x400B8000u;
    int usb = (addr & ~0x3FFFu) == 0x50110000u;   /* USBCTRL regs (+ aliases): nothing is attached, a USB host polling them must read idle */
    Ev *e = (addr >= 0x40000000 && !(addr >= 0x60000000 && addr < 0xD0000000) && !(tmr && !tbrk && opt.bus_verbose < 2)) ? ev_get(K_OTHER, pc, addr, &isnew) : NULL;   /* timer reads: no census entry unless asked (never a loop-breaker case) */
    if (e) {
        e->n++;
        if (isnew && known < 2) um_note(addr, pc, 0);
        if (pc == tl_lb_pc && addr == tl_lb_a && tl_act == tl_lb_act + 1) tl_lb_run++; else tl_lb_run = 1;
        tl_lb_pc = pc; tl_lb_a = addr; tl_lb_act = tl_act;
        uint64_t n = lbold ? e->n : tl_lb_run;
        if (tmr) { g_tm_reads[(addr & 0x7FFF) >> 2 & 31]++; if (n > 16) g_tm_broken[(addr & 0x7FFF) >> 2 & 31]++; }
        if (n > (known == 1 && !lbold ? 256u : 16u) && (lbold ? !(tmr && !tbrk) : (known < 2 || (tmr && tbrk))) && !usb) {   /* the timer is a clock, never a status bit */
            unsigned k = (unsigned)((n / 16) % 15);
            if (!(e->z & 1)) { e->y = v; e->z = 1; snprintf(e->note, sizeof e->note, "loop-breaker @%08x addr %08x", pc, addr); }   /* y: the value it would have read, x: which fakes went out (lb_dump) */
            e->x |= 1u << k; v = known == 1 && !lbold ? v | g_lb_cyc[k] : g_lb_cyc[k];
        }
    }
    if (size == 1) return (v >> ((addr & 3) * 8)) & 0xFF;
    if (size == 2) return (v >> ((addr & 3) * 8)) & 0xFFFF;
    return v;
}

static void mmio_store(uint32_t addr, int size, uint32_t val) {
    tl_act++; tl_n_ms++;
    if (opt.bus) {
        if (addr >= 0xE000E100u && addr < 0xE000E500u && nvic_access(1, addr, size, &val)) return;
        if (size == 4 && (addr & 0xFFFF4000u) == 0x400B0000u) tmr_access(1, addr, &val);       /* TIMER0 / TIMER1 (+ aliases) */
    }
    if (opt.bus) { uint64_t b0 = g_isrstat ? ns_wall() : 0; int r = um2bus_store(addr, size, val, tl_core); if (b0) tl_bus_ns += ns_wall() - b0; if (r) return; }
    mmio_store_base(addr, size, val);
}
static uint32_t mmio_dwt_read(void) { tl_n_ml++; tl_act++; return dwt_now(); }      /* what mmio_load does for DWT_CYCCNT (no NVIC, timer or bus-model address; canned value), without the shadow lookups, the census and the loop-breaker bookkeeping */
static uint32_t mmio_load(uint32_t addr, int size, uint32_t pc) {
    uint32_t v; tl_n_ml++;
    if ((addr & 0xFFFF4000u) != 0x400B0000u) tl_act++; else tl_n_tl++;        /* anything but the timer pages (incl. aliases) is activity */
    if (opt.bus) {
        if (addr >= 0xE000E100u && addr < 0xE000E500u && nvic_access(0, addr, size, &v)) return v;
        if (size == 4 && (addr & 0xFFFF4000u) == 0x400B0000u && tmr_access(0, addr, &v)) return v;
    }
    uint64_t b0 = (opt.bus && g_isrstat) ? ns_wall() : 0; int bl = opt.bus && um2bus_load(addr, size, &v, tl_core); if (b0) tl_bus_ns += ns_wall() - b0;
    if (bl) {
        if (size == 1) return (v >> ((addr & 3) * 8)) & 0xFF;
        if (size == 2) return (v >> ((addr & 3) * 8)) & 0xFFFF;
        return v;
    }
    return mmio_load_base(addr, size, pc);
}

/* ------------------------------------------------------------------ CPU context shared by both back-ends */
typedef struct { uint32_t r[17]; } Cpu;      /* r0..r15, cpsr */
enum { ACT_CONT = 0, ACT_STOP = 1 };
static int (*g_rd16)(uint32_t);               /* guest halfword fetch, -1 on failure */

static void it_advance(uint32_t *cpsr) {
    uint32_t it = ((*cpsr >> 8) & 0xFC) | ((*cpsr >> 25) & 3);
    if (!it) return;
    if ((it & 7) == 0) it = 0; else it = (it & 0xE0) | ((it << 1) & 0x1F);
    *cpsr &= ~((0x3Fu << 10) | (3u << 25));
    *cpsr |= (((it >> 2) & 0x3F) << 10) | ((it & 3) << 25);
}
static void skip(Cpu *c, int len) { c->r[15] += len; it_advance(&c->r[16]); }

static int hot_patch(const Cpu *c, uint32_t site, uint16_t h1, uint16_t h2, int len, int is_load);
static int rom_patch(const Cpu *c, uint32_t site, uint16_t h1);
static uint32_t g_nhot;
static int g_nospi;      /* UM2_NOSPIPATCH=1: SPI0 stays on the trap path (A/B timing, regression) */
static int g_nonewblk;   /* UM2_NONEWBLK=1: IO_BANK0, PADS_BANK0 and DWT stay on the trap path (A/B timing, regression) */
static int g_noreach;    /* UM2_NOREACH=1: the hot-patch reach of 1 Nov 2026 off (IT-block sites, relocated branches, hoisted ALU halfword, 32-bit second op, widened next-instruction set): the 31 Oct behaviour, for A/B counts */
static int g_novrint;    /* UM2_NOVRINT=1: VRINTA/N/P/M.F32 are skipped as before (A/B) */
static int g_novmaxnm;   /* UM2_NOVMAXNM=1: VMAXNM/VMINNM.F32 are skipped as before (A/B) */
static int g_noite;      /* UM2_NOITE=1: two-instruction IT blocks (ITT / ITE of two 16-bit ldr/str) stay on the trap path, as before 2 Nov 2026 (A/B) */
typedef struct { int ok, load, size, sgn, rt, rt2, rmask, wbreg; uint32_t wbval; } Mi;

static int popc(unsigned x) { return __builtin_popcount(x); }

static int decode_mem(uint16_t h1, uint16_t h2, int len, const Cpu *c, uint32_t addr, Mi *m) {
    memset(m, 0, sizeof *m); m->rt2 = -1; m->wbreg = -1;
    if (len == 2) {
        unsigned op = h1 >> 12;
        if (op == 5) {                                   /* register offset */
            static const int sz[8] = {4,2,1,1,4,2,1,2}, ld[8] = {0,0,0,1,1,1,1,1}, sg[8] = {0,0,0,1,0,0,0,1};
            unsigned o = (h1 >> 9) & 7; m->size = sz[o]; m->load = ld[o]; m->sgn = sg[o]; m->rt = h1 & 7; m->ok = 1; return 1;
        }
        if (op == 6 || op == 7 || op == 8) { m->size = op == 6 ? 4 : op == 7 ? 1 : 2; m->load = (h1 >> 11) & 1; m->rt = h1 & 7; m->ok = 1; return 1; }
        if (op == 9) { m->size = 4; m->load = (h1 >> 11) & 1; m->rt = (h1 >> 8) & 7; m->ok = 1; return 1; }
        if (op == 0xC) {                                 /* LDMIA/STMIA Rn!, {list} */
            unsigned rn = (h1 >> 8) & 7, l = (h1 >> 11) & 1, list = h1 & 0xFF;
            m->size = 4; m->load = l; m->rmask = list; m->ok = 1;
            if (!(l && (list >> rn & 1))) { m->wbreg = rn; m->wbval = c->r[rn] + 4 * popc(list); }
            return 1;
        }
        return 0;
    }
    if ((h1 & 0xFE00) == 0xF800) {                        /* LDR/STR family, 32-bit */
        unsigned size = (h1 >> 5) & 3, l = (h1 >> 4) & 1, s = (h1 >> 8) & 1, rn = h1 & 0xF;
        if (size == 3 || rn == 15) return 0;
        if (!l && s) return 0;
        m->size = 1 << size; m->load = l; m->sgn = s; m->rt = h2 >> 12; m->ok = 1;
        if (!(h1 & 0x80) && (h2 & 0x0800)) {              /* imm8 forms: P U W */
            unsigned P = (h2 >> 10) & 1, U = (h2 >> 9) & 1, W = (h2 >> 8) & 1, imm = h2 & 0xFF;
            if (((h2 >> 8) & 0xF) != 0xE && (W || !P)) {
                m->wbreg = rn;
                m->wbval = P ? addr : c->r[rn] + (U ? imm : -imm);
            }
        } else if (!(h1 & 0x80) && (h2 & 0x0FC0) != 0) return 0;  /* not the register-offset form either */
        return 1;
    }
    if ((h1 & 0xFE40) == 0xE840 && ((h1 & 0x0100) || (h1 & 0x20))) {   /* LDRD/STRD imm8 */
        unsigned P = (h1 >> 8) & 1, U = (h1 >> 7) & 1, W = (h1 >> 5) & 1, rn = h1 & 0xF, imm = (h2 & 0xFF) * 4;
        if (rn == 15) return 0;
        m->size = 4; m->load = (h1 >> 4) & 1; m->rt = h2 >> 12; m->rt2 = (h2 >> 8) & 0xF; m->ok = 1;
        if (W) { m->wbreg = rn; m->wbval = P ? addr : c->r[rn] + (U ? imm : -imm); }
        return 1;
    }
    if ((h1 & 0xFE40) == 0xE800 && (((h1 >> 7) & 3) == 1 || ((h1 >> 7) & 3) == 2)) {  /* LDM/STM .W IA (E88x/E89x) / DB (E90x/E91x) */
        unsigned ia = ((h1 >> 7) & 3) == 1, W = (h1 >> 5) & 1, l = (h1 >> 4) & 1, rn = h1 & 0xF, list = h2;
        m->size = 4; m->load = l; m->rmask = list; m->ok = 1;
        if (W && !(l && (list >> rn & 1))) { m->wbreg = rn; m->wbval = ia ? c->r[rn] + 4 * popc(list) : c->r[rn] - 4 * popc(list); }
        return 1;
    }
    return 0;
}

/* UM2_SITES=1 (diagnostics, 1 Nov 2026): a census of every trapping memory-access site (pc) with the address range, the load/store size and the reason hot_patch gave up,
 * printed at exit by hs_dump (traps per frame from the frame marks since the census window started: UM2_CENRESET=N).  Costs one spinlock round per trap, off by default. */
/* Why hot_patch said no: tl_why is only read by the census; the codes are the HW_* names. */
enum { HW_NONE = 0, HW_IT, HW_OFF, HW_ENC16, HW_NONEXT, HW_POOL, HW_NEXTBAD, HW_NOPRE, HW_ENC32, HW_REG, HW_BUSY, HW_REFUSED, HW_ADDR, HW_SIZE, HW_MULTI, HW_NKIND };
static const char *const g_why_txt[HW_NKIND] = { "-", "in an IT block", "hot-patch off/no stub area", "16-bit form not ldr/str imm5", "next halfword unreadable", "pool word out of range",
    "next insn cannot be relocated and no const-load before", "no const-load pair before the site", "32-bit form not ldr/str imm12 or reg", "sp/pc operand", "could not park the other core (retry)",
    "stub area full / overlaps another patch", "address not in a patchable block", "not a 32-bit access", "multi-register or writeback form" };
static __thread int tl_why;
typedef struct { uint32_t pc, amin, amax; uint64_t n; uint8_t ld, size, why, tries; } Hs;
#define HSCAP 2048
static Hs g_hs[HSCAP]; static int g_hs_on = -1; static volatile int g_hs_lk;
static void hs_note(uint32_t pc, uint32_t addr, int load, int size, int why, int tried) {
    if (g_hs_on < 0) g_hs_on = getenv("UM2_SITES") != NULL;
    if (!g_hs_on) return;
    while (__atomic_exchange_n(&g_hs_lk, 1, __ATOMIC_ACQUIRE)) ;
    uint32_t h = (pc >> 1) * 2654435761u >> 21;
    for (int i = 0; i < HSCAP; i++) {
        Hs *s = &g_hs[(h + (uint32_t)i) & (HSCAP - 1)];
        if (!s->n) { s->pc = pc; s->amin = s->amax = addr; s->ld = (uint8_t)load; s->size = (uint8_t)size; }
        if (s->pc != pc) continue;
        s->n++; if (addr < s->amin) s->amin = addr; if (addr > s->amax) s->amax = addr;
        if (why) s->why = (uint8_t)why;
        s->tries += (uint8_t)(tried && s->tries < 255);
        break;
    }
    __atomic_store_n(&g_hs_lk, 0, __ATOMIC_RELEASE);
}
static unsigned long g_excret;    /* EXC_RETURN fetch faults (the second signal of every non-native SVC), not in g_events */
static unsigned long g_svc_nat[256];    /* SVCs entered natively (svc_patch stubs), by call number */
/* UM2_SITES=1 also counts the calls of the hot-patch stubs (no signal, but each is a helper call through um2_hthunk with a VFP save: the cost of a stub site is not zero).
 * Same table layout as the trap census, keyed by the site; printed as "STUBS" lines by hs_dump. */
static Hs g_sc[512]; static volatile int g_sc_lk;
static void sc_note(uint32_t pc, uint32_t addr, int load) {
    while (__atomic_exchange_n(&g_sc_lk, 1, __ATOMIC_ACQUIRE)) ;
    uint32_t h = (pc >> 1) * 2654435761u >> 23;
    for (int i = 0; i < 512; i++) {
        Hs *s = &g_sc[(h + (uint32_t)i) & 511];
        if (!s->n) { s->pc = pc; s->amin = s->amax = addr; s->ld = (uint8_t)load; s->size = 4; }
        if (s->pc != pc) continue;
        s->n++; if (addr < s->amin) s->amin = addr; if (addr > s->amax) s->amax = addr;
        break;
    }
    __atomic_store_n(&g_sc_lk, 0, __ATOMIC_RELEASE);
}
static void hs_reset(void) { memset(g_hs, 0, sizeof g_hs); memset(g_sc, 0, sizeof g_sc); }

static const char *regn(int r) { static const char *n[] = {"r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","sp","lr","pc"}; return n[r & 15]; }

static int cpu_fault_mem(int sig, Cpu *c, uint32_t addr, uint16_t h1, uint16_t h2, int len) {
    uint32_t pc = c->r[15];
    Mi m;
    if (!decode_mem(h1, h2, len, c, addr, &m)) {
        printf("STOP: undecodable memory fault pc=%08x insn=%04x %04x addr=%08x (%s)\n", pc, h1, len == 4 ? h2 : 0, addr, blockname(addr));
        return ACT_STOP;
    }
    int nreg = m.rmask ? popc(m.rmask) : (m.rt2 >= 0 ? 2 : 1);
    int regs[16], k = 0;
    if (m.rmask) { for (int i = 0; i < 16; i++) if (m.rmask >> i & 1) regs[k++] = i; }
    else { regs[k++] = m.rt; if (m.rt2 >= 0) regs[k++] = m.rt2; }
    /* census */
    int isnew; Ev *e = ev_get(sig == SIGBUS ? K_BUS : K_SEGV, pc, 0, &isnew);
    if (e) {
        e->n++;
        if (addr < 0x8000 && m.load && m.size == 1 && len == 2 && (h1 & 0xFE00) == 0x5C00 && (e->n == 6 || (e->n > 6 && e->n < 520 && ((e->n - 6) & 7) == 0))
            && rom_patch(c, pc, h1)) return ACT_CONT;      /* boot ROM byte table (Centipede 0.3a): run the site again, now a b.w to the stub */
        int why = 0, tried = 0;
        int elig = (addr >= 0xD0000000u && addr < 0xD0000200u && !(addr >= 0xD0000100u)) || (addr >= 0x50000000u && addr < 0x50010000u) || ((addr & 0xFFFFC000u) == 0x40080000u && !g_nospi)
            || (addr >= 0x50200000u && addr < 0x50500000u)     /* PIO0..2: the UVM2 v2 bus engine polls FSTAT and pushes TXF0 word by word */
            || (addr >= 0x400B0000u && addr < 0x400C0000u)     /* TIMER0/1: alarm ISR, clock polls */
            || (!g_nonewblk && ((addr & 0xFFFF8000u) == 0x40028000u || (addr & 0xFFFF8000u) == 0x40038000u   /* IO_BANK0 / PADS_BANK0: the pad-function loops that switch the bus between PIO and SIO every frame */
                                || (addr & 0xFFFFF000u) == 0xE0001000u));       /* DWT (CYCCNT): the cycle-count delay loops (audio pulse trains, bus timing) */
               /* SIO, DMA, SPI0 (SD card: ~4 traps per byte otherwise; UM2_NOSPIPATCH=1 = old behaviour) */
        if (opt.bus && (e->n == 6 || (e->n > 6 && e->n < 520 && ((e->n - 6) & 7) == 0)) && m.size == 4 && nreg == 1 && m.wbreg < 0 && elig) {
            tried = 1;
            tl_patch_addr = addr;
            if (hot_patch(c, pc, h1, h2, len, m.load)) { hs_note(pc, addr, m.load, m.size, 0, 1); return ACT_CONT; }   /* patched: run the site again (resuming at pc+2 would land inside the b.w); the constant-load variant returns 0 and lets this access finish through the emulation below */
            why = tl_why;
        } else if (!elig) why = HW_ADDR; else if (m.size != 4) why = HW_SIZE; else if (nreg != 1 || m.wbreg >= 0) why = HW_MULTI;
        hs_note(pc, addr, m.load, m.size, why, tried);
        if (isnew) { e->x = addr; e->z = (m.load ? 1 : 2) | (m.size << 4); snprintf(e->note, sizeof e->note, "%s", blockname(addr));
            logf_("[%s] pc=%08x insn=%04x %04x %s%d%s addr=%08x (%s) %s\n", sig == SIGBUS ? "BUS" : "SEGV", pc, h1, len == 4 ? h2 : 0,
                  m.load ? "load" : "store", m.size * 8, m.sgn ? "s" : "", addr, blockname(addr), m.load ? regn(regs[0]) : ""); }
        e->y = addr;
    }
    for (int i = 0; i < nreg; i++) {
        uint32_t a = addr + 4 * i;
        Ev *ea = (!opt.bus || opt.bus_verbose) ? ev_get(K_ADDR, a & ~3u, 0, NULL) : NULL;   /* per-word census: only the summary uses it */
        if (ea) { ea->n++; if (m.load) ea->x++; else ea->y++; }
        if (m.load) {
            uint32_t v = mmio_load(a, m.size, pc);
            if (m.sgn) v = m.size == 1 ? (uint32_t)(int8_t)v : (uint32_t)(int16_t)v;
            if (regs[i] == 15) { printf("STOP: load into pc at %08x\n", pc); return ACT_STOP; }
            c->r[regs[i]] = v;
            if (ea) ea->z = v;
        } else {
            uint32_t v = c->r[regs[i]];
            if (ea) ea->z = v;
            mmio_store(a, m.size, m.size == 4 ? v : m.size == 2 ? v & 0xFFFF : v & 0xFF);
        }
    }
    if (m.wbreg >= 0) {
        int clobbered = 0;
        if (m.load) for (int i = 0; i < nreg; i++) if (regs[i] == m.wbreg) clobbered = 1;
        if (!clobbered) c->r[m.wbreg] = m.wbval;
    }
    skip(c, len);
    return ACT_CONT;
}

static void print_summary(int final);
static void lb_dump(FILE *f);
static void um_dump(FILE *f);
static uint32_t g_lastregs[17]; static int g_haveregs;
static void (*g_wr16)(uint32_t, uint16_t);    /* guest halfword store (used to rewrite ARMv8 LDA/STL in place) */
static uint32_t g_sys[256];                   /* shadow of the M-profile special registers (MRS/MSR SYSm) */

/* VFPv4/FPv5 single-precision fused multiply-accumulate -> the chained VFPv3 instruction with the same operands:
 *   VFMA  EEA0|D<<6|Vn, Vd 0A.0   -> VMLA   EE00|..   (Sd =  Sd + Sn*Sm)       VFMS  (op bit 6 set) -> VMLS   (Sd = Sd - Sn*Sm)
 *   VFNMA EE90|D<<6|Vn, Vd 0A.0   -> VNMLA  EE10|..   (Sd = -Sd - Sn*Sm)       VFNMS (op bit 6 clear) -> VNMLS (Sd = -Sd + Sn*Sm)
 * h2 = Vd:4 | 101 | sz=0 | N | op | M | 0 | Vm:4 (bit 4 clear = data-processing, sz clear = single). */
static int fma_rewrite(uint16_t h1, uint16_t h2, uint16_t *n1) {
    if ((h2 & 0x0F10) != 0x0A00) return 0;
    if ((h1 & 0xFFB0) == 0xEEA0) { *n1 = h1 & ~0x00A0; return 1; }
    if ((h1 & 0xFFB0) == 0xEE90) { *n1 = h1 & ~0x0080; return 1; }
    return 0;
}

/* ARMv8-M load-acquire / store-release have no ARMv7-A encoding.  Rewrite them in place to the plain (or exclusive)
 * ARMv7 equivalent of identical length; the caller then re-executes the instruction.  Returns 1 if rewritten. */
static int v8_rewrite(uint16_t h1, uint16_t h2, uint16_t *n1, uint16_t *n2) {
    unsigned rn = h1 & 15, rt = h2 >> 12, lo = h2 & 0x0FFF;
    if ((h1 & 0xFFF0) == 0xE8D0) {
        switch (lo) {
        case 0xFAF: *n1 = 0xF8D0 | rn; *n2 = rt << 12; return 1;                 /* LDA   -> LDR.W   */
        case 0xF8F: *n1 = 0xF890 | rn; *n2 = rt << 12; return 1;                 /* LDAB  -> LDRB.W  */
        case 0xF9F: *n1 = 0xF8B0 | rn; *n2 = rt << 12; return 1;                 /* LDAH  -> LDRH.W  */
        case 0xFEF: *n1 = 0xE850 | rn; *n2 = (rt << 12) | 0x0F00; return 1;      /* LDAEX -> LDREX   */
        case 0xFCF: case 0xFDF: *n1 = h1; *n2 = h2 & ~0x80; return 1;            /* LDAEXB/H -> LDREXB/H */
        }
    } else if ((h1 & 0xFFF0) == 0xE8C0) {
        switch (lo & 0xFF0) {
        case 0xFA0: if (lo == 0xFAF) { *n1 = 0xF8C0 | rn; *n2 = rt << 12; return 1; } break;   /* STL  -> STR.W  */
        case 0xF80: if (lo == 0xF8F) { *n1 = 0xF880 | rn; *n2 = rt << 12; return 1; } break;   /* STLB -> STRB.W */
        case 0xF90: if (lo == 0xF9F) { *n1 = 0xF8A0 | rn; *n2 = rt << 12; return 1; } break;   /* STLH -> STRH.W */
        case 0xFE0: *n1 = 0xE840 | rn; *n2 = (rt << 12) | ((lo & 0xF) << 8); return 1;        /* STLEX -> STREX */
        case 0xFC0: case 0xFD0: *n1 = h1; *n2 = h2 & ~0x80; return 1;                         /* STLEXB/H -> STREXB/H */
        }
    }
    return 0;
}

/* ---- M-profile instructions that an A9 does NOT reliably fault on ----
 * TT/TTT/TTA/TTAT  E84n Fxxx  : decodes as STREX Rd,Rt=PC,[Rn,#imm]   -> store to [Rn] (data abort, or a silent write!)
 * MRS Rd,SYSm      F3EF 8Rxx  : decodes as MRS Rd,CPSR (SYSm SBZ)      -> silently returns CPSR
 * MSR SYSm,Rn      F38n 88xx  : decodes as MSR CPSR_f,Rn (SYSm SBZ)    -> silently overwrites NZCV from Rn[31:28]
 * Everything else M33-only (LDA/STL, SDIV/UDIV, VFPv4/v5, coprocessors, SG) is UNDEFINED on a Cortex-A9 and raises SIGILL.
 * So at load time every halfword pair matching the three patterns is replaced by UDF.W (always SIGILL) and remembered here. */
static struct { uint32_t addr; uint16_t h1, h2; } g_pat[4096];
static int g_npat, g_noprepatch;
static int pat_find(uint32_t a) {
    int lo = 0, hi = g_npat - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pat[m].addr == a) return m; if (g_pat[m].addr < a) lo = m + 1; else hi = m - 1; }
    return -1;
}
static int sysm_ok(unsigned s) { return (s >= 1 && s <= 11) || (s >= 16 && s <= 20) || (s >= 0x88 && s <= 0x8B) || (s >= 0x90 && s <= 0x94) || s == 0x98 || s == 0x99; }
static int is_tt(uint16_t h1, uint16_t h2)  { return (h1 & 0xFFF0) == 0xE840 && (h2 & 0xF03F) == 0xF000; }
static int is_mrs(uint16_t h1, uint16_t h2) { return h1 == 0xF3EF && (h2 & 0xF000) == 0x8000; }
static int is_msr(uint16_t h1, uint16_t h2) { return (h1 & 0xFFF0) == 0xF380 && (h2 & 0xF300) == 0x8000 && (h2 & 0x0C00); }
static void prepatch(uint8_t *img, uint32_t words) {
    uint16_t *h = (uint16_t *)img; size_t n = (size_t)words * 2; int nt = 0, nr = 0, nw = 0;
    for (size_t i = 16; i + 1 < n; i++) {
        uint16_t a = h[i], b = h[i + 1]; int kind = 0;
        if (is_tt(a, b)) kind = 1; else if (is_mrs(a, b) && sysm_ok(b & 0xFF)) kind = 2; else if (is_msr(a, b) && sysm_ok(b & 0xFF)) kind = 3;
        if (!kind) continue;
        if (g_npat >= (int)(sizeof g_pat / sizeof *g_pat)) { fprintf(stderr, "prepatch table full\n"); break; }
        g_pat[g_npat].addr = IMG_BASE + (uint32_t)i * 2; g_pat[g_npat].h1 = a; g_pat[g_npat].h2 = b; g_npat++;
        h[i] = 0xF7F2; h[i + 1] = 0xA0F0;             /* UDF.W #0x20F0 (NOT f7f0 a000: that is the Linux Thumb-2 breakpoint -> SIGTRAP) */
        nt += kind == 1; nr += kind == 2; nw += kind == 3; i++;
    }
    fprintf(stderr, "prepatched %d M-profile sites to UDF.W (TT %d, MRS %d, MSR %d)\n", g_npat, nt, nr, nw);
}
static int prim_patch(const Cpu *c, uint32_t site, int mrs, int reg, int span);
static int m33_shim(Cpu *c, uint32_t pc, uint16_t h1, uint16_t h2, int patched) {
    int isnew; const char *what; char note[48]; int sysm_prim = (is_mrs(h1, h2) || is_msr(h1, h2)) && (h2 & 0xFF) == 16;
    if (is_tt(h1, h2)) { c->r[(h2 >> 8) & 15] = 0; what = "TT (TrustZone test target)"; snprintf(note, sizeof note, "TT [emulated, Rd=0]"); }
    else if (is_mrs(h1, h2) || is_msr(h1, h2)) {
        int mrs = is_mrs(h1, h2); unsigned sysm = h2 & 0xFF, r = mrs ? (h2 >> 8) & 15 : h1 & 15;
        if (mrs) c->r[r] = (sysm == 8 || sysm == 9) ? c->r[13] : sysm == 16 ? g_nv[tl_core & 1].prim : sysm == 5 ? ipsr_now() : sysm <= 3 ? ((c->r[16] & 0xF8000000u) | (sysm == 3 ? ipsr_now() : 0)) : g_sys[sysm];
        else {
            g_sys[sysm] = c->r[r];
            if (sysm == 16) g_nv[tl_core & 1].prim = c->r[r] & 1;
            if (sysm == 8 || sysm == 9) c->r[13] = c->r[r];
            if (sysm <= 3) c->r[16] = (c->r[16] & 0x07FFFFFFu) | (c->r[r] & 0xF8000000u);
        }
        what = mrs ? "MRS" : "MSR"; snprintf(note, sizeof note, "%s SYSm=%u [emulated]", what, sysm);
    } else return 0;
    Ev *e = ev_get(K_ILL, pc, 0, &isnew);
    if (e) { e->n++; if (isnew) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, "%s", note);
        logf_("[ILL] pc=%08x insn=%04x %04x %s%s\n", pc, h1, h2, note, patched ? " (prepatched)" : " (caught at fault)"); } }
    if (opt.bus && sysm_prim && e && (e->n <= 3 || (e->n < 520 && (e->n & 7) == 0))) {      /* PRIMASK pair: from now on a stub, no trap (one try per trap, a few early, then every 8th) */
        int mrs_ = is_mrs(h1, h2), rr = mrs_ ? (int)((h2 >> 8) & 15) : (int)(h1 & 15);
        if (!mrs_) prim_patch(c, pc, 0, rr, 4);
        else if (g_rd16(pc + 4) == 0xB672) prim_patch(c, pc, 1, rr, 6);
    }
    skip(c, 4);
    if (opt.bus && is_mrs(h1, h2) && (h2 & 0xFF) == 16) {         /* MRS rN, PRIMASK + CPSID I (save_and_disable_interrupts): one step */
        int nx = g_rd16(c->r[15]);
        if (nx == 0xB672) { g_nv[tl_core & 1].prim = 1; skip(c, 2); }
    }
    return 1;
}

/* ---- ARMv8-M exception entry/return for SVCall (--bus): the image's own SVC handler really runs.
 * Entry stacks r0-r3, r12, lr, return pc, xPSR (8-byte aligned) on the current stack and enters the handler with
 * lr = EXC_RETURN (0xFFFFFFF9); the handler's `pop {.., pc}` then fetches 0xFFFFFFF8 on this A9, which faults
 * and lands in exc_return().  Only thread-mode / main-stack / no-FP frames exist in these images. */
static uint32_t g_vtor;
static uint32_t vtor_now(void) {
    uint32_t *p = sh_slot(0xE000ED08, 0);
    return (p && *p) ? *p : IMG_BASE;
}
static int exc_enter2(Cpu *c, uint32_t ret_pc, unsigned vec, int irq) {
    uint32_t vt = vtor_now(); int h = g_rd16(vt + vec * 4), h2 = g_rd16(vt + vec * 4 + 2);
    if (h < 0 || h2 < 0) return 0;
    uint32_t handler = (uint32_t)h | ((uint32_t)h2 << 16);
    uint32_t sp = c->r[13] - 32, pad = 0;
    if (sp & 4) { sp -= 4; pad = 1; }
    uint32_t *f = (uint32_t *)(uintptr_t)sp;
    int nat = irq && g_irq_thunk;           /* native return: the thunk gets the Thumb return address (ldr pc), no IT state (the caller refused those) */
    f[0] = c->r[0]; f[1] = c->r[1]; f[2] = c->r[2]; f[3] = c->r[3]; f[4] = c->r[12]; f[5] = c->r[14]; f[6] = ret_pc | (nat ? 1u : 0u);
    /* APSR flags | T | stack-align flag; an IRQ frame also keeps the IT state (the interrupted code may sit inside an IT block) and carries marker bit 20 */
    f[7] = (c->r[16] & 0xF8000000u) | (1u << 24) | (pad << 9) | (irq ? ((c->r[16] & ((0x3Fu << 10) | (3u << 25))) | (1u << 20)) : 0);
    c->r[13] = sp; c->r[14] = nat ? (uint32_t)(uintptr_t)um2_irqret : 0xFFFFFFF9u; c->r[15] = handler & ~1u;
    c->r[16] = (c->r[16] & ~((0x3Fu << 10) | (3u << 25))) | 0x20;
    (void)g_vtor;
    return 1;
}
static int exc_enter(Cpu *c, uint32_t ret_pc, unsigned vec) {            /* SVCall: runs at priority 0, nothing preempts it */
    if (!exc_enter2(c, ret_pc, vec, 0)) return 0;
    Nvic *n = &g_nv[tl_core & 1];
    if (n->depth < 7) { n->sprio[n->depth] = 0; n->sirq[n->depth] = -1; n->sret[n->depth] = ret_pc; n->depth++; }
    return 1;
}
static __thread ucontext_t *tl_uc;                                       /* the signal context being handled (VFP state lives in it) */
#define VFP_MAGIC 0x56465001u
static uint32_t *uc_vfp(ucontext_t *uc) {                                /* struct user_vfp (32 x u64 + fpscr) inside uc_regspace */
    unsigned char *p = (unsigned char *)uc->uc_regspace, *e = p + sizeof uc->uc_regspace;
    while (p + 8 <= e) { uint32_t m = *(uint32_t *)p, sz = *(uint32_t *)(p + 4); if (m == VFP_MAGIC) return (uint32_t *)(p + 8); if (!m || sz < 8) break; p += sz; }
    return NULL;
}
static __thread uint32_t tl_fps[8][65]; static __thread uint8_t tl_fpv[8];      /* interrupted VFP state per exception depth, and whether it was captured */
/* Equal priorities: the hardware takes the lowest number first.  That starves a line when a lower-numbered one pends again at once, which is what the
 * TIMER0 alarm does as soon as its ISR (the 6502 slice) takes longer than the 166 us tick (always, on this emulator): the DMA completion interrupt of the cart
 * audio (IRQ 10) then never ran.  So among equal priorities the IRQ after the one taken last goes first (round robin).  UM2_NOIRQRR=1 (or UM2_NOAUDIO=1): lowest number first. */
static int nvic_pick(Nvic *n) {
    static int rr = -1; if (rr < 0) rr = getenv("UM2_NOIRQRR") == NULL && getenv("UM2_NOAUDIO") == NULL;
    uint64_t d = (((uint64_t)n->pend[1] << 32) | n->pend[0]) & (((uint64_t)n->en[1] << 32) | n->en[0]);
    int run = n->depth ? n->sprio[n->depth - 1] : 256, best = -1, bp = 256, bd = 64;
    while (d) { int i = __builtin_ctzll(d); d &= d - 1; if (i >= NIRQ) continue; int p = n->pri[i]; if (p >= run) continue;
        int dist = rr ? ((i - n->last - 1) & 63) : 0;
        if (p < bp) { bp = p; best = i; bd = dist; } else if (p == bp && dist < bd) { best = i; bd = dist; } }
    return best;
}
/* Burst guard.  On the A9 the TIMER0 alarm ISR (BattleZone's 6502 slice) takes longer than its 166 us period, so every ISR exit tail-chains into the next
 * alarm and thread mode on that core never runs again (28 Oct hardware: 471k interrupts, all tail-chained; core 0's main code, which starts the cart
 * audio, never got there).  On the RP2350 the ISR is short and thread mode keeps most of the time.  So after UM2_IRQBURST us (default 6000) of
 * uninterrupted interrupt context the core returns to thread mode and takes no interrupt for UM2_IRQGAP us (default 1000).  UM2_IRQBURST=0 = off. */

static inline int irq_held(Nvic *n) {
    if (!n->held) return 0;
    if ((int32_t)(micros() - n->hold) < 0) return 1;
    n->held = 0; return 0;
}
static int irq_enter(Cpu *c, ucontext_t *uc) {        /* take the best pending IRQ on this core, if any; c = state to resume (pc inside the image) */
    Nvic *n = &g_nv[tl_core & 1];
    if (n->prim || n->depth >= 7) { g_ie_why[0]++; return 0; }
    if (g_irq_thunk && (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) { g_ie_why[1]++; irq_wake_retry(); return 0; }       /* inside an IT block: the native return cannot restore ITSTATE; the irq thread retries */
    int irq = nvic_pick(n); if (irq < 0) { g_ie_why[2]++; return 0; }
    if (irq_held(n)) { g_ie_why[3]++; return 0; }
    uint32_t ret = c->r[15];
    if (!exc_enter2(c, ret, 16 + (unsigned)irq, 1)) return 0;
    __atomic_fetch_and(&n->pend[irq >> 5], ~(1u << (irq & 31)), __ATOMIC_SEQ_CST);
    uint32_t *v = uc ? uc_vfp(uc) : NULL;
    if (v) memcpy(tl_fps[n->depth], v, sizeof tl_fps[0]);
    else { uint32_t *d = tl_fps[n->depth]; uint32_t fp;       /* no VFP block in the signal frame: the hardware registers still hold the interrupted values */
           __asm__ volatile("vstmia %0, {d0-d15}" :: "r"(d) : "memory"); __asm__ volatile("vmrs %0, fpscr" : "=r"(fp)); d[64] = fp; }
    tl_fpv[n->depth] = 1;
    if (!n->depth && g_irq_burst) n->since = micros();
    if (!n->depth) isr_begin(irq);
    n->sret[n->depth] = ret;
    n->sprio[n->depth] = n->pri[irq]; n->sirq[n->depth] = (int8_t)irq; n->depth++; n->last = (int8_t)irq;
    g_irq_cnt[irq]++; g_irq_pc[tl_core & 1]++;
    return 1;
}
static void irq_poll(Cpu *c) {                         /* end of every trap: deliver what is pending (c is the resume state) */
    static int async_only = -1; if (async_only < 0) async_only = getenv("UM2_IRQASYNC") != NULL;     /* test hook: only the SIGURG path delivers */
    if (!g_irq_on || async_only) return;
    Nvic *n = &g_nv[tl_core & 1];
    if (n->prim) return;
    if (c->r[15] < IMG_BASE || c->r[15] >= IMG_TOP || !(c->r[16] & 0x20)) return;
    if (irq_enter(c, tl_uc)) g_irq_trap++;
}
/* Native ISR return.  lr = um2_irqret when an IRQ is entered, so the handler's `bx lr` / `pop {.., pc}` lands here (ARM state, guest sp = the exception
 * frame; r4-r11 are the interrupted code's again).  um2_irq_exit() pops the active-exception record and, tail-chaining like the NVIC, returns the next pending
 * ISR's address (then the frame stays and the thunk re-enters with lr = um2_irqret).  Otherwise the thunk restores the VFP state captured at entry,
 * the flags and r0-r3/r12/lr from the frame and jumps to the Thumb return address.  Saves one SIGSEGV round trip per interrupt (about 50 us on the A9). */
uint32_t um2_irq_exit(uint32_t sp) {
    (void)sp;
    Nvic *n = &g_nv[tl_core & 1];
    if (n->depth) n->depth--;
    if (!n->depth) isr_end();
    if (g_irq_on) { uint64_t m = irq_lines();                       /* an unserviced level line pends again */
        if ((uint32_t)m) __atomic_fetch_or(&n->pend[0], (uint32_t)m, __ATOMIC_SEQ_CST);
        if ((uint32_t)(m >> 32)) __atomic_fetch_or(&n->pend[1], (uint32_t)(m >> 32), __ATOMIC_SEQ_CST); }
    if (n->prim || n->depth >= 7) return 0;
    int irq = nvic_pick(n); if (irq < 0) return 0;
    if (!n->depth && g_irq_burst) { uint32_t t = micros();             /* tail-chain from the outermost ISR: thread mode starved for too long? */
        if (t - n->since > g_irq_burst) { n->hold = t + g_irq_gap; n->held = 1; g_irq_bursts++; irq_wake(); return 0; } }       /* the irq thread must come back when the hold ends (it may be asleep for 20 ms) */
    uint32_t vt = vtor_now(); int h = g_rd16(vt + (16 + (unsigned)irq) * 4), h2 = g_rd16(vt + (16 + (unsigned)irq) * 4 + 2);
    if (h < 0 || h2 < 0) return 0;
    __atomic_fetch_and(&n->pend[irq >> 5], ~(1u << (irq & 31)), __ATOMIC_SEQ_CST);
    if (!n->depth) isr_begin(irq);
    if (!tl_fpv[n->depth & 7]) { uint32_t *dd = tl_fps[n->depth & 7]; uint32_t fp;      /* chained after a frame that carries no VFP state (a native SVC return): the ISR may clobber d0-d15, so keep what is in them now for the final return */
        __asm__ volatile("vstmia %0, {d0-d15}" :: "r"(dd) : "memory"); __asm__ volatile("vmrs %0, fpscr" : "=r"(fp)); dd[64] = fp; tl_fpv[n->depth & 7] = 1; }
    n->sprio[n->depth] = n->pri[irq]; n->sirq[n->depth] = (int8_t)irq; n->depth++; n->last = (int8_t)irq;
    g_irq_cnt[irq]++; g_irq_pc[tl_core & 1]++; g_irq_chain++;
    return (uint32_t)h | ((uint32_t)h2 << 16) | 1u;
}
uint32_t *um2_irq_fp(void) { Nvic *n = &g_nv[tl_core & 1]; return tl_fpv[n->depth] ? tl_fps[n->depth] : NULL; }
#ifdef __arm__
__asm__(".text\n.arm\n.type um2_irqret, %function\n.global um2_irqret\num2_irqret:\n"
        "  mov r0, sp\n  bl um2_irq_exit\n  cmp r0, #0\n  adrne lr, um2_irqret\n  bxne r0\n"
        "  bl um2_irq_fp\n  cmp r0, #0\n  vldmiane r0, {d0-d15}\n  ldrne r1, [r0, #256]\n  vmsrne fpscr, r1\n"
        "  ldr r1, [sp, #24]\n  orr r1, r1, #1\n  str r1, [sp, #24]\n"        /* the return address gets its Thumb bit here: an SVC frame keeps the architectural bit 0 clear for the handler, which reads the call number at pc - 2 */
        "  ldr r0, [sp, #28]\n  tst r0, #0x200\n  bne 1f\n"
        "  msr APSR_nzcvq, r0\n  ldmia sp!, {r0-r3, r12, lr}\n  ldr pc, [sp], #8\n"
        "1:\n  msr APSR_nzcvq, r0\n  ldmia sp!, {r0-r3, r12, lr}\n  ldr pc, [sp], #12\n.thumb\n");
#else
void um2_irqret(void) {}
#endif
static int exc_return(Cpu *c) {
    uint32_t sp = c->r[13];
    uint32_t *f = (uint32_t *)(uintptr_t)sp;
    c->r[0] = f[0]; c->r[1] = f[1]; c->r[2] = f[2]; c->r[3] = f[3]; c->r[12] = f[4]; c->r[14] = f[5];
    c->r[15] = f[6] & ~1u;
    c->r[16] = (c->r[16] & 0x07FFFFFFu & ~((0x3Fu << 10) | (3u << 25))) | (f[7] & 0xF8000000u) | 0x20 | ((f[7] & (1u << 20)) ? (f[7] & ((0x3Fu << 10) | (3u << 25))) : 0);
    c->r[13] = sp + 32 + ((f[7] >> 9) & 1 ? 4 : 0);
    { Nvic *n = &g_nv[tl_core & 1]; if (n->depth) n->depth--;
      if ((f[7] & (1u << 20)) && tl_uc) { uint32_t *v = uc_vfp(tl_uc); if (v) memcpy(v, tl_fps[n->depth], sizeof tl_fps[0]); } }
    return 1;
}

/* ---- SDIV/UDIV acceleration (--bus): the first time an executed SDIV/UDIV traps, replace it with `b.w stub`; the stub (Thumb-2, in a
 * side mapping within branch range) saves r0-r3/r12/lr/flags, calls an ARM-side divide and returns to the next instruction.  Only code that
 * really runs is patched (no scan of the image for look-alikes in data).  Sites inside IT blocks, with sp/lr/pc operands, or unaligned
 * (a non-atomic 2x16-bit patch could be executed half-written by the other core) are left to the trap emulation. */
#define STUB_BASE 0x20100000u
#define STUB_SIZE (512u << 10)
static uint32_t g_stub_off, g_stub_ready, g_nstubs, g_npatched_div;
static int g_novfp;      /* UM2_NOVFP=1: stubs use the lean thunk (A/B timing only) */
uint32_t um2_sdiv(uint32_t a, uint32_t b) { int32_t x = (int32_t)a, y = (int32_t)b; if (!y) return 0; if (x == INT32_MIN && y == -1) return a; return (uint32_t)(x / y); }
uint32_t um2_udiv(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static void stub_setup(void) {
    void *m = mmap((void *)STUB_BASE, STUB_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_stub_ready = (m == (void *)STUB_BASE);
    g_novfp = getenv("UM2_NOVFP") != NULL;
    g_nospi = getenv("UM2_NOSPIPATCH") != NULL;
    g_nonewblk = getenv("UM2_NONEWBLK") != NULL;
    g_dwtfast = getenv("UM2_NODWTFAST") == NULL;
    g_spifast = opt.bus && !g_isrstat && getenv("UM2_NOSPIFAST") == NULL;
    g_spilean = g_spifast && getenv("UM2_NOSPILEAN") == NULL && getenv("UM2_SITES") == NULL && um2sd_lean_ok() && um2bus_spi_lean_ok();   /* UM2_SITES counts in the helper; -v -v -v logs card commands */
    g_srfast = g_spilean && getenv("UM2_SRFAST") != NULL;
#ifdef UM2_VFPTEST
    g_spilean = 0;
#endif
    g_noreach = getenv("UM2_NOREACH") != NULL;
    g_novrint = getenv("UM2_NOVRINT") != NULL;
    g_novmaxnm = getenv("UM2_NOVMAXNM") != NULL;
    g_noite = getenv("UM2_NOITE") != NULL;
    if (!g_stub_ready) perror("um2run: stub area (division stays on the trap path)");
}
static uint32_t enc_bw(uint32_t from, uint32_t to) {      /* Thumb-2 B.W T4: returns h1 | h2 << 16 */
    int32_t off = (int32_t)(to - (from + 4));
    uint32_t S = ((uint32_t)off >> 24) & 1, I1 = ((uint32_t)off >> 23) & 1, I2 = ((uint32_t)off >> 22) & 1;
    uint32_t J1 = (I1 ^ 1) ^ S, J2 = (I2 ^ 1) ^ S;
    uint32_t h1 = 0xF000u | (S << 10) | (((uint32_t)off >> 12) & 0x3FF);
    uint32_t h2 = 0x9000u | (J1 << 13) | (J2 << 11) | (((uint32_t)off >> 1) & 0x7FF);
    return h1 | (h2 << 16);
}
/* ---- patching protocol (27 Oct 2026, review item 2).  A patch overwrites guest code that the other core may be executing, or may be about to resume
 * at: a trap handler that decoded the old instruction skips to site+len, which after the patch is the middle of the new b.w.  So every patch (div,
 * hot site, LDA/STL stub) runs with the other guest core parked: SIGUSR2 -> on_quiesce records the interrupted pc and waits to be released.  A core that
 * is inside a trap handler has all signals blocked, so it parks only after its sigreturn, with the resume pc in hand: if that pc lies inside the span
 * (site, site+span) the attempt is abandoned (the old code is intact) and retried on a later trap.  One patch at a time (trylock: a core waiting for
 * the lock in a handler could never park); spans never overlap; the start/cover bitmaps make "is this a patched site" O(1) for every trap. */
static uint8_t g_pstart[(IMG_TOP - IMG_BASE) / 16 + 1], g_pcover[(IMG_TOP - IMG_BASE) / 16 + 1];   /* 1 bit per halfword: first halfword of a patch / any halfword of a patch */
static inline int bm_get(const uint8_t *bm, uint32_t a) { if (a < IMG_BASE || a >= IMG_TOP) return 0; uint32_t i = (a - IMG_BASE) >> 1; return (__atomic_load_n(&bm[i >> 3], __ATOMIC_ACQUIRE) >> (i & 7)) & 1; }
static inline void bm_set(uint8_t *bm, uint32_t a) { uint32_t i = (a - IMG_BASE) >> 1; __atomic_fetch_or(&bm[i >> 3], (uint8_t)(1u << (i & 7)), __ATOMIC_SEQ_CST); }
static int is_patched(uint32_t pc) { return bm_get(g_pstart, pc); }
static pthread_mutex_t g_patch_m = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t g_q_req, g_q_go, g_q_ack, g_q_pc; static uint32_t g_q_seq, g_q_cur;
static void on_quiesce(int sig, siginfo_t *si, void *vuc) {
    (void)sig; (void)si;
    uint32_t r = __atomic_load_n(&g_q_req, __ATOMIC_ACQUIRE);
    g_q_pc = ((ucontext_t *)vuc)->uc_mcontext.arm_pc;
    __atomic_store_n(&g_q_ack, r, __ATOMIC_RELEASE);
    for (unsigned n = 0; __atomic_load_n(&g_q_go, __ATOMIC_ACQUIRE) != r; n++) {      /* sleep after a short spin: the patcher may be a lower RT priority on this very CPU */
        if (n < 32) __asm__ volatile("yield"); else { struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL); }
    }
}
/* park the other guest core: 1 = parked (or there is none), 0 = no ack within ~10 ms or it resumes inside (lo, hi) */
static int quiesce_other(uint32_t lo, uint32_t hi) {
    pthread_t o; g_q_cur = 0;
    if (tl_core) o = g_th_core0; else if (__atomic_load_n(&g_have_core1, __ATOMIC_ACQUIRE)) o = g_th_core1; else return 1;
    uint32_t s = ++g_q_seq;
    __atomic_store_n(&g_q_req, s, __ATOMIC_RELEASE);
    if (pthread_kill(o, SIGUSR2)) return 1;
    for (unsigned n = 0; __atomic_load_n(&g_q_ack, __ATOMIC_ACQUIRE) != s; n++) {
        if (n > 132) { __atomic_store_n(&g_q_go, s, __ATOMIC_RELEASE); return 0; }       /* late arrival finds go == s and returns at once */
        if (n < 32) sched_yield(); else { struct timespec ts = { 0, 50000 }; nanosleep(&ts, NULL); }
    }
    uint32_t pc = g_q_pc;
    if (pc > lo && pc < hi) { __atomic_store_n(&g_q_go, s, __ATOMIC_RELEASE); return 0; }
    g_q_cur = s; return 1;
}
static unsigned long g_patch_frame;          /* patch attempts postponed because an exception frame returns inside the span */
/* 1 = go (lock held, other core parked, *base = stub address), -1 = site already patched, 0 = busy / could not park (retry on a later trap), -2 = refused for good */
static int patch_begin(uint32_t site, uint32_t span, uint32_t bytes, uint32_t *base) {
    if (pthread_mutex_trylock(&g_patch_m)) return 0;
    if (is_patched(site)) { pthread_mutex_unlock(&g_patch_m); return -1; }
    int bad = g_stub_off + bytes > STUB_SIZE;
    for (uint32_t a = site; a < site + span; a += 2) if (bm_get(g_pcover, a)) bad = 1;      /* would clobber another patch (a branch into the middle of one) */
    if (bad) { pthread_mutex_unlock(&g_patch_m); return -2; }
    if (!quiesce_other(site, site + span)) { pthread_mutex_unlock(&g_patch_m); return 0; }
    for (int k = 0; k < 2; k++) for (int d = 0; d < g_nv[k].depth && d < 8; d++) {         /* review finding 1: an open exception (either core, the other one is parked) returns inside the span */
        uint32_t r = g_nv[k].sret[d] & ~1u;
        if (r > site && r < site + span) { g_patch_frame++; if (g_q_cur) { __atomic_store_n(&g_q_go, g_q_cur, __ATOMIC_RELEASE); g_q_cur = 0; } pthread_mutex_unlock(&g_patch_m); return 0; }
    }
    *base = STUB_BASE + g_stub_off; g_stub_off += bytes; g_nstubs++;
    return 1;
}
static void patch_end(uint32_t site, uint32_t span, uint32_t base) {     /* the stub at base is complete and cache-clean; the other core is still parked */
    uint32_t sbw = enc_bw(site, base);
    for (uint32_t a = site; a < site + span; a += 2) bm_set(g_pcover, a);
    bm_set(g_pstart, site);
    if (site & 3) {
        __atomic_store_n((volatile uint16_t *)(uintptr_t)(site + 2), (uint16_t)(sbw >> 16), __ATOMIC_RELEASE);
        __atomic_store_n((volatile uint16_t *)(uintptr_t)site, (uint16_t)(sbw & 0xFFFF), __ATOMIC_RELEASE);
    } else __atomic_store_n((volatile uint32_t *)(uintptr_t)site, sbw, __ATOMIC_RELEASE);
    __builtin___clear_cache((char *)(uintptr_t)site, (char *)(uintptr_t)(site + 4));
    if (g_q_cur) { __atomic_store_n(&g_q_go, g_q_cur, __ATOMIC_RELEASE); g_q_cur = 0; }
    pthread_mutex_unlock(&g_patch_m);
}
static int div_patch(const Cpu *c, uint32_t site, int sgn, int rd, int rn, int rm) {
    if (!opt.bus || !g_stub_ready) return 0;
    const char *why = (c->r[16] & ((0x3Fu << 10) | (3u << 25))) ? "IT block" : (rd == 13 || rn == 13 || rm == 13 || rd == 15 || rn == 15 || rm == 15) ? "sp/pc operand" : NULL;
    if (why) { static int shown; if (opt.bus_verbose && shown++ < 40) fprintf(stderr, "div_patch: %08x not patched (%s)%c", site, why, 10); return 0; }
    uint16_t k[48]; int n = 0;
#define E(x) k[n++] = (uint16_t)(x)
    E(0xE92D); E(0x500F);                               /* push {r0-r3, r12, lr}     */
    E(0xF3EF); E(0x8300);                               /* mrs r3, apsr              */
    E(0xF84D); E(0x3D04);                               /* str r3, [sp, #-4]!        */
    int src[2] = { rm, rn }, dst[2] = { 1, 0 };
    for (int i = 0; i < 2; i++) {
        int sreg = src[i], d = dst[i];
        if (sreg <= 3 || sreg == 12 || sreg == 14) { E(0xF8DD); E((d << 12) | (sreg == 14 ? 24 : sreg == 12 ? 20 : 4 + 4 * sreg)); }   /* ldr.w d, [sp, #slot] */
        else E(0x4600 | ((d & 8) << 4) | (sreg << 3) | (d & 7));                                        /* mov d, sreg */
    }
    int lit_at = -1;
    E(0xF8DF); lit_at = n; E(0xC000);                   /* ldr.w r12, [pc, #lit]     */
E(0x466B); E(0xF023); E(0x0307); E(0xF1A3); E(0x0308); E(0x46EE); E(0xF8C3); E(0xE000); E(0x469D);   /* 8-byte align sp for the C helper, old sp kept at [sp] */
    E(0x47E0);                                          /* blx r12                   */
    E(0xF8DD); E(0xD000);                               /* ldr.w sp, [sp]  (undo the alignment) */
    if (rd <= 3 || rd == 12 || rd == 14) { E(0xF8CD); E((0 << 12) | (rd == 14 ? 24 : rd == 12 ? 20 : 4 + 4 * rd)); }   /* str.w r0, [sp, #slot]  */
    else E(0x4600 | ((rd & 8) << 4) | (0 << 3) | (rd & 7));                                  /* mov rd, r0 */
    E(0xF85D); E(0x3B04);                               /* ldr r3, [sp], #4          */
    E(0xF383); E(0x8800);                               /* msr apsr_nzcvq, r3        */
    E(0xE8BD); E(0x500F);                               /* pop {r0-r3, r12, lr}      */
    int b_at = n; n += 2;                               /* b.w site+4                */
    if (n & 1) E(0xBF00);                               /* keep the literal word-aligned */
    int lit_word = n; n += 4;
    uint32_t bytes = (uint32_t)n * 2, base;
    if (patch_begin(site, 4, bytes, &base) <= 0) return 0;      /* the caller has emulated the division already and skips the instruction either way */
    uint32_t lit_addr = base + (uint32_t)lit_word * 2;
    uint32_t ldr_pc = base + (uint32_t)(lit_at - 1) * 2 + 4;
    k[lit_at] |= (uint16_t)(lit_addr - (ldr_pc & ~3u));
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + 4);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    uint32_t fn = (uint32_t)(uintptr_t)(sgn ? um2_sdiv : um2_udiv);
    memcpy(k + lit_word, &fn, 4);
    memcpy((void *)(uintptr_t)base, k, bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, 4, base);
    return 1;
}
/* Byte loads from the boot ROM area (0x00000000-0x00007FFF; readable on the RP2350). Centipede 0.3a reads its 17-bit noise table
 * through a pointer it never sets, i.e. from the first 16 KB of the boot ROM, with `ldrb rt,[rn,rm]`: about 40,000 traps a second
 * (11 Nov 2026). The area cannot be mapped where it is (ARM Linux refuses anything below 0x2000), so a read-only copy with what
 * mmio_load_base returns there (zero, plus the canned header words) sits at ROMCOPY_BASE, and a hot site of that form becomes
 * `b.w stub`: the stub adds ROMCOPY_BASE to addresses below 0x8000 and loads; any other address is loaded as it is. The instruction
 * after the site moves into the stub (only plain data processing, no pc, no memory access). No flags touched; one low register is
 * saved on the stack. Only sites that trap on the boot ROM area again and again are patched. UM2_NOROMPATCH=1: trap every time. */
#define ROMCOPY_BASE 0x80000000u
static int g_romcopy;
static int target_hits(uint32_t site, uint32_t want);
static void romcopy_setup(void) {
    if (getenv("UM2_NOROMPATCH")) return;
    void *q = mmap((void *)ROMCOPY_BASE, 0x8000, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q != (void *)ROMCOPY_BASE) { perror("um2run: boot ROM copy (its reads trap as before)"); return; }
    uint32_t *w = (uint32_t *)q;
    if (g_bootrom) memcpy(q, g_bootrom, 0x8000);
    w[0x10 / 4] = 0x01754Du; w[0x14 / 4] = ROM_FUNCTAB | ((uint32_t)ROM_LOOKUP << 16);       /* as mmio_load_base */
    mprotect(q, 0x8000, PROT_READ);
    g_romcopy = 1;
}
static int movable_insn(int a, int b) {     /* may run in the stub as it is: plain data processing that neither reads nor writes pc nor touches memory */
    if (a < 0) return 0;
    if (a < 0xE800) return a < 0x4400;                                                   /* 16-bit: shifts, add/sub, mov/cmp imm8, ALU register */
    if (b < 0 || (b & 0x8000)) return 0;
    int rd = (b >> 8) & 15, s = (a >> 4) & 1;
    if ((a & 0xFA00) == 0xF000) return rd != 15 || s;                                    /* data processing, modified immediate */
    if ((a & 0xFE00) == 0xEA00) return (b & 15) != 15 && (rd != 15 || s);               /* data processing, shifted register */
    return 0;
}
static int rom_patch(const Cpu *c, uint32_t site, uint16_t h1) {
    if (!opt.bus || !g_stub_ready || !g_romcopy || (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) return 0;
    int rt = h1 & 7, rn = (h1 >> 3) & 7, rm = (h1 >> 6) & 7, s = 0;
    while (s == rt || s == rn || s == rm) s++;
    int n1 = g_rd16(site + 2), n2 = n1 >= 0xE800 ? g_rd16(site + 4) : -1, nlen = n1 >= 0xE800 ? 2 : 1;
    if (!movable_insn(n1, n2) || target_hits(site, site + 2)) return 0;
    uint32_t span = 2 + 2 * (uint32_t)nlen;
    uint16_t k[24]; int n = 0;
#define E(x) k[n++] = (uint16_t)(x)
    E(0xB400 | (1 << s));                                                   /* push {s}                   */
    E(0xEB00 | rn); E((s << 8) | rm);                                       /* add.w s, rn, rm            */
    E(0xEA4F); E((3 << 12) | (rt << 8) | (3 << 6) | (1 << 4) | s);          /* lsr.w rt, s, #15           */
    E(0xFAB0 | rt); E(0xF080 | (rt << 8) | rt);                             /* clz rt, rt (32: boot ROM)  */
    E(0xEA4F); E((1 << 12) | (rt << 8) | (1 << 6) | (1 << 4) | rt);         /* lsr.w rt, rt, #5           */
    E(0xEB00 | s); E((7 << 12) | (s << 8) | (3 << 6) | rt);                 /* add.w s, s, rt, lsl #31    */
    E(0xF890 | s); E(rt << 12);                                             /* ldrb.w rt, [s]             */
    E(0xBC00 | (1 << s));                                                   /* pop {s}                    */
    E(n1); if (nlen == 2) E(n2);                                            /* the next instruction       */
    int b_at = n; n += 2;                                                   /* b.w site + span            */
    E(0xBF00);
    uint32_t bytes = (uint32_t)n * 2, base;
    int pb = patch_begin(site, span, bytes, &base);
    if (pb <= 0) return pb == -1;                                           /* -1: the other core patched it: run the site again */
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + span);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    memcpy((void *)(uintptr_t)base, k, bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, span, base);
    if (opt.bus_verbose) fprintf(stderr, "rom_patch: %08x ldrb r%d,[r%d,r%d] -> stub %08x%c", site, rt, rn, rm, base, 10);
    return 1;
}
/* VSEL.F32 (ARMv8 FP, in the M33's FPv5; UNDEFINED on the A9).  BattleZone's cart-audio synthesis executes ~60000 of them per second (a clamp in the
 * per-sample loop), each a ~50 us trap that, before 28 Oct 2026, was also skipped (the destination kept its old value).  The first trap emulates the
 * instruction through the saved VFP state of the signal frame and replaces it with `b.w stub`; the stub is the equivalent conditional pair
 * `ite cc ; vmov.f32 sd, sn ; vmov.f32 sd, sm ; b.w site+4` (no flags touched, no registers saved).  UM2_NOVSELPATCH=1: trap every time. */
static int vsel_patch(const Cpu *c, uint32_t site, unsigned cc, unsigned sd, unsigned sn, unsigned sm) {
    static int off = -1; if (off < 0) off = getenv("UM2_NOVSELPATCH") != NULL;
    if (off || !opt.bus || !g_stub_ready || (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) return 0;
    static const unsigned cond[4] = { 0x0, 0x6, 0xA, 0xC };               /* EQ, VS, GE, GT */
    unsigned fc = cond[cc & 3];
    uint16_t k[8]; int n = 0;
#define E(x) k[n++] = (uint16_t)(x)
    E(0xBF00 | (fc << 4) | ((fc & 1) ? 0x4 : 0xC));                       /* ite cc */
    E(0xEEB0 | ((sd & 1) << 6)); E(((sd >> 1) << 12) | 0x0A40 | ((sn & 1) << 5) | (sn >> 1));     /* vmovcc.f32 sd, sn */
    E(0xEEB0 | ((sd & 1) << 6)); E(((sd >> 1) << 12) | 0x0A40 | ((sm & 1) << 5) | (sm >> 1));     /* vmovncc.f32 sd, sm */
    int b_at = n; n += 2;
    E(0xBF00);
    uint32_t bytes = (uint32_t)n * 2, base;
    if (patch_begin(site, 4, bytes, &base) <= 0) return 0;
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + 4);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    memcpy((void *)(uintptr_t)base, k, bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, 4, base);
    return 1;
}

/* Helper thunk for the stubs (27 Oct 2026, review item 1).  The stubs call C helpers (um2_mload/um2_mstore) on the guest's own thread and
 * stack; C code may use d0-d7 (doubles in now(), NEON memcpy/memset in libc) and FPSCR, which are the guest's s0-s15 and FP flags, and may run
 * deeper than the little stack the image left.  The thunk (ARM state, r3 = target, r0-r2 = args, returns r0) saves d0-d7 + FPSCR and runs the
 * target on this thread's own host stack (um2_tl_hsp, set per core before the guest starts; 0 = stay on the current stack, 8-aligned).  r4-r6
 * keep FPSCR / sp / TLS across the call (callee-saved in the C helper); the guest stack only takes 16 bytes.  FPSCR is written back only if it
 * changed (VMSR serialises).  um2_hthunk_lean (UM2_NOVFP=1) skips the VFP part: only for A/B timing on the hardware. */
__thread uint32_t um2_tl_hsp;
void um2_hthunk(void), um2_hthunk_lean(void);
__asm__(".pushsection .text.um2_hthunk,\"ax\",%progbits\n\t.arm\n\t.arch armv7-a\n\t.fpu vfpv3-d16\n\t.balign 4\n"
        ".global um2_hthunk\n.type um2_hthunk, %function\n"
        "um2_hthunk:\n\tpush {r4-r6, lr}\n\tmov r6, sp\n\tmrc p15, 0, r4, c13, c0, 3\n\tldr r5, 1f\n\tldr r5, [r4, r5]\n\tcmp r5, #0\n\tmovne sp, r5\n\tbic sp, sp, #7\n"
        "\tvmrs r4, fpscr\n\tvpush {d0-d7}\n\tblx r3\n\tvpop {d0-d7}\n\tvmrs r2, fpscr\n\tcmp r2, r4\n\tvmsrne fpscr, r4\n\tmov sp, r6\n\tpop {r4-r6, lr}\n\tbx lr\n"
        "1:\t.word um2_tl_hsp(tpoff)\n.size um2_hthunk, .-um2_hthunk\n"
        ".global um2_hthunk_lean\n.type um2_hthunk_lean, %function\n"
        "um2_hthunk_lean:\n\tpush {r4-r6, lr}\n\tmov r6, sp\n\tmrc p15, 0, r4, c13, c0, 3\n\tldr r5, 1b\n\tldr r5, [r4, r5]\n\tcmp r5, #0\n\tmovne sp, r5\n\tbic sp, sp, #7\n"
        "\tblx r3\n\tmov sp, r6\n\tpop {r4-r6, lr}\n\tbx lr\n"
        ".size um2_hthunk_lean, .-um2_hthunk_lean\n"
        /* SD card byte loop (10 Nov 2026): stubs of SPI0 sites call these instead of um2_hthunk.  SPI0 DR / SR (the address in r0, checked here at run
         * time) goes to um2_spild_lean / um2_spist_lean through the lean thunk: that path (um2bus_spi_*, um2sd) uses no VFP register and no libc
         * but pthread_mutex and pread/pwrite (checked in the disassembly).  Any other address goes to um2_hthunk with r3 = um2_mload / um2_mstore,
         * as before.  r2 and r12 are free here (the stub saved r0-r3, r12, lr and the flags). */
        ".global um2_hthunk_spild\n.type um2_hthunk_spild, %function\n"
        "um2_hthunk_spild:\n\tldr r12, 3f\n\tand r12, r0, r12\n\tldr r2, 4f\n\tcmp r12, r2\n\tbne um2_hthunk\n\tldr r3, 5f\n\tb um2_hthunk_lean\n"
        ".size um2_hthunk_spild, .-um2_hthunk_spild\n"
        ".global um2_hthunk_spist\n.type um2_hthunk_spist, %function\n"
        "um2_hthunk_spist:\n\tldr r12, 4f\n\tcmp r0, r12\n\tbne um2_hthunk\n\tmov r0, r1\n\tldr r3, 6f\n\tb um2_hthunk_lean\n"
        "3:\t.word 0xFFFFCFFB\n4:\t.word 0x40080008\n5:\t.word um2_spild_lean\n6:\t.word um2_spist_lean\n"
        ".size um2_hthunk_spist, .-um2_hthunk_spist\n"
        /* um2_hthunk_spisr: as um2_hthunk_spild, but SPI0 SR (any alias) goes straight to um2_srfast, a leaf that uses no stack and no VFP
         * register (checked in the disassembly), without the thunk's stack switch: half the card's accesses are SR polls. */
        ".global um2_hthunk_spisr\n.type um2_hthunk_spisr, %function\n"
        "um2_hthunk_spisr:\n\tldr r12, 7f\n\tand r12, r0, r12\n\tldr r2, 8f\n\tcmp r12, r2\n\tldreq r12, 9f\n\tbxeq r12\n\tb um2_hthunk_spild\n"
        "7:\t.word 0xFFFFCFFF\n8:\t.word 0x4008000C\n9:\t.word um2_srfast\n"
        ".size um2_hthunk_spisr, .-um2_hthunk_spisr\n.popsection\n");
void um2_hthunk_spild(void), um2_hthunk_spist(void), um2_hthunk_spisr(void);
/* ---- hot GPIO_IN load acceleration (--bus): `ldr rt, [rn, #imm]` (16-bit) that faults on SIO GPIO_IN again and again (the SDK's E-clock polling loops)
 * is rewritten together with the following 16-bit ALU/compare instruction into `b.w stub`.  The stub saves r0-r3/r12/lr/flags, calls
 * um2_mload() (the same model the trap path uses), stores the result into rt, restores, runs the relocated instruction and returns.
 * Refused when the second halfword is a branch target (looked for in a +-64 byte window), when the next instruction is not a plain
 * PC-independent 16-bit data instruction, or inside an IT block. */
static uint8_t g_hstk[2][1 << 16] __attribute__((aligned(64)));      /* host stacks of the two guest threads (static: see g_ld_stack) */
static void hstack_set(int core) { um2_tl_hsp = (uint32_t)(uintptr_t)g_hstk[core] + sizeof g_hstk[core] - 16; }
/* test build only (-DUM2_VFPTEST): the helpers clobber d0-d7 + FPSCR and note whether they run on the guest stack; tools/um2run test guest checks that the stubs hide both */
#ifdef UM2_VFPTEST
static unsigned g_vt_calls, g_vt_guest;
static void vt_probe(void) {
    uint32_t sp = (uint32_t)(uintptr_t)__builtin_frame_address(0); g_vt_calls++; if (sp >= IMG_BASE && sp < IMG_TOP) g_vt_guest++;
    __asm__ volatile("vmov.f64 d0, #2.0\n\tvmov.f64 d1, #3.0\n\tvmov.f64 d2, #4.0\n\tvmov.f64 d3, #5.0\n\tvmov.f64 d4, #6.0\n\tvmov.f64 d5, #7.0\n\tvmov.f64 d6, #8.0\n\tvmov.f64 d7, #9.0\n\tvmsr fpscr, %0" :: "r"(0x90000001u) : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc");
}
#define VT_PROBE() vt_probe()
#else
#define VT_PROBE() ((void)0)
#endif
uint32_t um2_mload(uint32_t addr, uint32_t site) {
    VT_PROBE(); if (__builtin_expect(g_hs_on > 0, 0)) sc_note(site, addr, 1);
    if (addr < 0x40000000u) return *(volatile uint32_t *)(uintptr_t)addr;
    if (addr == 0xE0001004u && g_dwtfast && !opt.no_canned) return mmio_dwt_read();
    if ((addr & 0xFFFFCFFBu) == 0x40080008u && g_spifast) { tl_n_ml++; tl_act++; return um2bus_spi_load(addr); }      /* SPI0 DR / SR: what mmio_load does for them, without the walk */
    return mmio_load(addr, 4, site);
}
/* Review finding 2: every direct branch target in the image (B/Bcc/CBZ/CBNZ, B.W/Bcc.W/BL), from one linear sweep at load.  The sweep follows instruction lengths and
 * steps over the literal-pool words the PC-relative loads it has passed refer to (a pool word such as 0x2005E578 otherwise reads as `b.n` back into the code); other data
 * can still put it out of step for a few halfwords, so target_hits() keeps the old local scan of every halfword as well.  TBB/TBH tables are not decoded. */
static uint8_t g_btgt[(IMG_TOP - IMG_BASE) / 16 + 1]; static unsigned g_nbtgt;
static uint8_t g_lpool[(IMG_TOP - IMG_BASE) / 32 + 1];             /* 1 bit per word: referenced by a literal load */
static uint16_t rd16s(const uint8_t *p);
static void lpool_mark(uint32_t a, int nw) { for (int i = 0; i < nw; i++, a += 4) if (a >= IMG_BASE && a < IMG_TOP) { uint32_t w = (a - IMG_BASE) >> 2; g_lpool[w >> 3] |= (uint8_t)(1u << (w & 7)); } }
static void btgt_scan(const uint8_t *img, uint32_t words) {
    uint32_t len = words * 4;
    for (uint32_t o = 0; o + 2 <= len;) {
        if (!(o & 3) && (g_lpool[o >> 5] >> ((o >> 2) & 7) & 1)) { o += 4; continue; }
        uint16_t a = rd16s(img + o); uint32_t pc = IMG_BASE + o, t = 0; int br = 0, lb = (int)((pc + 4) & ~3u);
        if ((a & 0xF800) == 0x4800) lpool_mark((uint32_t)lb + (a & 0xFFu) * 4, 1);                        /* ldr rt,[pc,#imm8] */
        if (t32(a)) {
            if (o + 4 > len) break;
            uint16_t b = rd16s(img + o + 2);
            if ((a & 0xFE1F) == 0xF81F) lpool_mark(((uint32_t)lb + ((a & 0x80) ? (uint32_t)(b & 0xFFF) : (uint32_t)-(int32_t)(b & 0xFFF))) & ~3u, 1);   /* ldr{,b,h,sb,sh}.w literal */
            else if ((a & 0xFE7F) == 0xE85F) lpool_mark((uint32_t)lb + ((a & 0x80) ? (uint32_t)(b & 0xFF) * 4 : (uint32_t)-(int32_t)((b & 0xFF) * 4)), 2);   /* ldrd literal */
            else if ((a & 0xFF3F) == 0xED1F && ((b >> 9) & 7) == 5) lpool_mark((uint32_t)lb + ((a & 0x80) ? (uint32_t)(b & 0xFF) * 4 : (uint32_t)-(int32_t)((b & 0xFF) * 4)), (b & 0x100) ? 2 : 1);   /* vldr literal */
            if ((a & 0xF800) == 0xF000 && (b & 0x8000)) {
                uint32_t S = (a >> 10) & 1, J1 = (b >> 13) & 1, J2 = (b >> 11) & 1;
                if ((b & 0x5000) == 0x0000 && ((a >> 6) & 15) < 14) { int32_t off = (int32_t)((S << 20) | (J2 << 19) | (J1 << 18) | ((a & 0x3F) << 12) | ((b & 0x7FF) << 1)); if (S) off |= (int32_t)0xFFE00000; t = pc + 4 + (uint32_t)off; br = 1; }   /* Bcc.W (T3) */
                else if (b & 0x1000) { uint32_t i1 = !(J1 ^ S), i2 = !(J2 ^ S); int32_t off = (int32_t)((S << 24) | (i1 << 23) | (i2 << 22) | ((a & 0x3FF) << 12) | ((b & 0x7FF) << 1)); if (S) off |= (int32_t)0xFE000000; t = pc + 4 + (uint32_t)off; br = 1; }   /* B.W (T4) / BL */
            }
            o += 4;
        } else {
            if ((a & 0xF000) == 0xD000 && (a & 0x0F00) < 0x0E00) { t = pc + 4 + (uint32_t)((int32_t)(int8_t)(a & 0xFF) * 2); br = 1; }
            else if ((a & 0xF800) == 0xE000 && (a & 0x7FF)) { t = pc + 4 + (uint32_t)((int32_t)((a & 0x7FF) << 21) >> 20); br = 1; }
            else if ((a & 0xF500) == 0xB100) { t = pc + 4 + (uint32_t)((((a >> 9) & 1) << 6) | (((a >> 3) & 31) << 1)); br = 1; }
            o += 2;
        }
        if (br && t >= IMG_BASE && t < IMG_BASE + len && !(t & 1)) { if (!bm_get(g_btgt, t)) g_nbtgt++; bm_set(g_btgt, t); }
    }
}
static int target_hits(uint32_t site, uint32_t want) {
    if (bm_get(g_btgt, want)) return 1;
    for (uint32_t a = site - 64; a < site + 64; a += 2) {
        int hh = g_rd16(a); if (hh < 0 || (!g_noreach && bm_get(g_pcover, a))) continue;   /* our own b.w halves (a patch is never an original branch: g_btgt still holds the branches the patched code had) */
        uint16_t x = (uint16_t)hh; int32_t off = 0; int br = 0;
        if ((x & 0xF000) == 0xD000 && (x & 0x0F00) < 0x0E00) { off = (int32_t)(int8_t)(x & 0xFF) * 2; br = 1; }
        else if ((x & 0xF800) == 0xE000) { off = (int32_t)((x & 0x7FF) << 21) >> 20; br = (x & 0x7FF) != 0; }     /* 0xE000 = `b .+4` is nothing a compiler emits: it is the second half of `ldr.w lr,[ip]` (BattleZone's DMA ISR: a false "branch target" that kept its READ_ADDR store on the trap path, 1 per ms) */
        else if ((x & 0xF500) == 0xB100) { off = (int32_t)((((x >> 9) & 1) << 6) | (((x >> 3) & 31) << 1)); br = 1; }
        if (br && a + 4 + (uint32_t)off == want) return 1;
    }
    return 0;
}
/* 32-bit Thumb-2 data-processing instruction without PC/SP operands: executable from the stub unchanged */
static int safe32(uint16_t a, uint16_t b) {
    int rd = (b >> 8) & 15;
    if ((a & 0xFA00) == 0xF000 && !(b & 0x8000)) { int rn = a & 15; return rn != 15 && rn != 13 && rd != 15 && rd != 13; }   /* modified / plain immediate */
    if ((a & 0xFE00) == 0xEA00) { int rn = a & 15, rm = b & 15; return rn != 15 && rn != 13 && rm != 15 && rm != 13 && rd != 15 && rd != 13; }   /* shifted register */
    return 0;
}
static int slot_of(int r) { return r <= 3 ? 4 + 4 * r : r == 12 ? 20 : r == 14 ? 24 : -1; }
void um2_mstore(uint32_t addr, uint32_t val, uint32_t site) {
    VT_PROBE(); if (__builtin_expect(g_hs_on > 0, 0)) sc_note(site, addr, 0);
    if (addr < 0x40000000u) *(volatile uint32_t *)(uintptr_t)addr = val;
    else if (addr == 0x40080008u && g_spifast) { tl_act++; tl_n_ms++; um2bus_spi_dr_store(val); }    /* SPI0 DR: as um2_mload */
    else mmio_store(addr, 4, val);
}
/* what um2_mload / um2_mstore do for SPI0 DR / SR, from um2_hthunk_spild / _spist (only installed when g_spifast holds and nothing per site is counted) */
uint32_t um2_spild_lean(uint32_t addr) { tl_n_ml++; tl_act++; return um2bus_spi_load(addr); }
void um2_spist_lean(uint32_t val) { tl_act++; tl_n_ms++; um2bus_spi_dr_store(val); }
uint32_t um2_srfast(void) { tl_n_ml++; tl_act++; return um2bus_spi_sr(); }   /* from um2_hthunk_spisr, on the guest's stack: must stay a frameless leaf */
/* Generalised: 32-bit loads/stores of SIO and DMA registers too.  A site is `ldr/str rt,[rn,#imm]` in the 16-bit imm5 form or the 32-bit
 * imm12 form; a 16-bit site takes the following halfword with it, which is relocated into the stub if it is a plain PC-independent ALU
 * instruction, or emulated through the helper as well if it is another 16-bit imm5 ldr/str (the helpers pass non-MMIO addresses straight
 * through).  The stub returns to the instruction after the pair. */
typedef struct { int load, rt, rn; uint32_t imm; int cst; uint32_t cval; } HOp;   /* cst: `ldr rt,[pc,#imm]` of a constant pool word */   /* cst: `ldr rt,[pc,#imm]` of a constant pool word */
#define NOPE(w) (tl_why = (w), 0)
/* Thumb-2 B<cond>.W (T3, +-1 MB: used inside one stub only) */
static uint32_t enc_bcc(uint32_t from, uint32_t to, unsigned cond) {
    int32_t off = (int32_t)(to - (from + 4));
    uint32_t S = ((uint32_t)off >> 20) & 1, J2 = ((uint32_t)off >> 19) & 1, J1 = ((uint32_t)off >> 18) & 1;
    uint32_t h1 = 0xF000u | (S << 10) | ((cond & 15) << 6) | (((uint32_t)off >> 12) & 0x3F);
    uint32_t h2 = 0x8000u | (J1 << 13) | (J2 << 11) | (((uint32_t)off >> 1) & 0x7FF);
    return h1 | (h2 << 16);
}
/* 16-bit instructions that may run from a stub, after the stub's epilogue has restored every register and the flags, instead of at their own address: no pc operand,
 * no memory access that could hit MMIO (that would fault with pc in the stub), no branch except `bx rm` / `pop {.., pc}` (control leaves the stub for the same place). */

static int safe16(uint16_t x) {
    if (g_noreach) return x <= 0x43FF || (x >= 0xB200 && x <= 0xB2FF);
    if (x <= 0x43FF || (x >= 0xB200 && x <= 0xB2FF)) return 1;                                    /* shifts, add/sub, mov/cmp/add/sub imm8, ALU, sxt/uxt */
    if (x >= 0x4400 && x <= 0x46FF) { unsigned rm = (x >> 3) & 15, rd = (x & 7) | ((x >> 4) & 8); return rm != 15 && rd != 15; }   /* add/cmp/mov with high registers, not pc */
    if ((x & 0xFF87) == 0x4700) return ((x >> 3) & 15) != 15;                                     /* bx rm */
    if (x >= 0xB000 && x <= 0xB0FF) return 1;                                                     /* add/sub sp, #imm */
    if (x >= 0xA800 && x <= 0xAFFF) return 1;                                                     /* add rd, sp, #imm */
    if (x >= 0x9000 && x <= 0x9FFF) return 1;                                                     /* str/ldr rt, [sp, #imm]: the stack is plain RAM */
    if ((x & 0xFE00) == 0xB400 || (x & 0xFE00) == 0xBC00) return 1;                          /* push {.., lr}, pop {.., pc} (stack only) */
    return 0;
}
/* the same set without the control-transfer forms and without anything that moves sp: what may be hoisted in front of a site (executed before its stub body) */
static int safe16_pre(uint16_t x) { return x <= 0x43FF || (x >= 0xB200 && x <= 0xB2FF); }
static int hot_patch(const Cpu *c, uint32_t site, uint16_t h1, uint16_t h2, int len, int is_load) {
    tl_why = 0;
    { static int init; static const char *only; if (!init) { only = getenv("UM2_HOTONLY"); init = 1; } if (only && strtoul(only, 0, 16) != site) return NOPE(HW_OFF); }
    if (!opt.bus || !g_stub_ready) return NOPE(HW_OFF);
    HOp op[2]; int nop = 1; int nx = -1, nx2 = -1, span = 4;
    int rmp1 = 0, rmsh = 0;                               /* op[0] is `ldr/str.w rt,[rn,rm,lsl #rmsh]` (rm + 1, 0 = immediate form) */
    int pre = 0;                                          /* patched from site - 2: 1 = the pair is `ldr rt,[pc,#imm]` (constant) + the site instruction, 2 = a plain ALU halfword before the site is hoisted
                                                           * into the stub, 3 = the site is the only instruction of an IT block and the `it` goes with it (the stub starts with the inverse conditional branch) */
    unsigned itcc = 0; uint16_t preh = 0;                 /* pre 3: the IT condition; pre 2: the hoisted halfword */
    int itp = 0, itb = 0;                                 /* pre 3 with two instructions: itp 1 = ITT (both on the condition), 2 = ITE (one or the other); itb: the site is the second instruction (the IT is at site - 4) */
    int nbr = -1; uint32_t nbr_t = 0; uint16_t nbr_cb = 0;      /* the next halfword is a B<cond>.n (0..13), B.n (14) or CBZ/CBNZ (15, nbr_cb = the stub's own CBZ/CBNZ): relocated as branches in the stub */
    uint32_t itst = ((c->r[16] >> 8) & 0xFC) | ((c->r[16] >> 25) & 3);
    if (len == 2) {
        if ((h1 & 0xF800) != (is_load ? 0x6800 : 0x6000)) return NOPE(HW_ENC16);
        op[0] = (HOp){ is_load, h1 & 7, (h1 >> 3) & 7, ((h1 >> 6) & 31) * 4, 0, 0 };
    } else {
        if ((h1 & 0xFFF0) == (is_load ? 0xF8D0 : 0xF8C0)) op[0] = (HOp){ is_load, (h2 >> 12) & 15, h1 & 15, h2 & 0xFFF, 0, 0 };
        else if ((h1 & 0xFFF0) == (is_load ? 0xF850 : 0xF840) && (h2 & 0x0FC0) == 0)       /* ldr/str.w rt,[rn,rm,lsl #0..3] */
            { op[0] = (HOp){ is_load, (h2 >> 12) & 15, h1 & 15, 0, 0, 0 }; rmp1 = (int)(h2 & 15) + 1; rmsh = (int)((h2 >> 4) & 3); }
        else return NOPE(HW_ENC32);
    }
    if (itst && g_noreach) return NOPE(HW_IT);
    if (itst) {                                           /* inside an IT block: `it<cc>` + this one instruction (mask 1000b), or a two-instruction ITT / ITE block of two 16-bit word ldr/str, patched from the `it` */
        int ih = g_rd16(site - 2), iq = g_rd16(site - 4);
        if (!g_noite && len == 2 && ih >= 0) {            /* UM2_NOITE=1: the 1 Nov behaviour (two-instruction blocks stay on the trap path) */
            int pa = g_rd16(site + 2);                    /* site is the first instruction of the block: the IT is at site - 2, the second instruction follows */
            if ((ih & 0xFF00) == 0xBF00 && (ih & 7) == 4 && (uint32_t)(ih & 0xFF) == itst && pa >= 0 && (pa & 0xF000) == 0x6000 && iq >= 0 && iq < 0xE800 && ((ih >> 4) & 15) < 14
                && !target_hits(site, site) && !target_hits(site, site + 2) && !bm_get(g_pcover, site - 2)) {
                op[1] = (HOp){ (pa & 0x0800) != 0, pa & 7, (pa >> 3) & 7, (uint32_t)((pa >> 6) & 31) * 4, 0, 0 };
                itp = ((ih >> 3) & 1) == ((ih >> 4) & 1) ? 1 : 2; itcc = (unsigned)(ih >> 4) & 15; pre = 3; span = 6; nop = 2;
            } else if (iq >= 0 && (iq & 0xFF00) == 0xBF00 && (iq & 7) == 4 && itst == (((uint32_t)iq & 0xE0) | (((uint32_t)iq << 1) & 0x1F)) && (ih & 0xF000) == 0x6000 && ((iq >> 4) & 15) < 14
                       && g_rd16(site - 6) >= 0 && g_rd16(site - 6) < 0xE800 && !target_hits(site, site) && !target_hits(site, site - 2) && !bm_get(g_pcover, site - 4)) {
                op[1] = op[0]; op[0] = (HOp){ (ih & 0x0800) != 0, ih & 7, (ih >> 3) & 7, (uint32_t)((ih >> 6) & 31) * 4, 0, 0 };   /* site is the second instruction: the IT is at site - 4 */
                itp = ((iq >> 3) & 1) == ((iq >> 4) & 1) ? 1 : 2; itcc = (unsigned)(iq >> 4) & 15; pre = 3; span = 6; nop = 2; itb = 1;
            }
        }
        if (!itp) {
            if (ih < 0 || iq < 0 || (ih & 0xFF08) != 0xBF08 || (ih & 7) != 0 || (uint32_t)(ih & 0xFF) != itst || iq >= 0xE800 || ((ih >> 4) & 15) >= 14 || target_hits(site, site) || bm_get(g_pcover, site - 2)) return NOPE(HW_IT);
            itcc = (unsigned)(ih >> 4) & 15; pre = 3; span = len == 2 ? 4 : 6;
        }
    } else if (len == 2) {
        nx = g_rd16(site + 2); if (nx < 0) return NOPE(HW_NONEXT);
        int ok = !target_hits(site, site + 2);
        if (ok) {
            if ((nx & 0xE000) == 0x6000 && (nx & 0x1000) == 0) {        /* 0x6000..0x6FFF: str/ldr rt,[rn,#imm5*4] */
                op[1] = (HOp){ (nx & 0x0800) != 0, nx & 7, (nx >> 3) & 7, (uint32_t)((nx >> 6) & 31) * 4, 0, 0 }; nop = 2; nx = -1;
            } else if ((nx & 0xF800) == 0x4800) {                         /* ldr rt,[pc,#imm8*4]: the pool word is constant, materialise it */
                uint32_t la = ((site + 2 + 4) & ~3u) + (uint32_t)(nx & 0xFF) * 4;
                if (la < IMG_BASE || la + 4 > IMG_TOP) return NOPE(HW_POOL);
                op[1] = (HOp){ 1, (nx >> 8) & 7, 0, 0, 1, *(volatile uint32_t *)(uintptr_t)la }; nop = 2; nx = -1;
            } else if (!g_noreach && ((nx & 0xFFF0) == 0xF8D0 || (nx & 0xFFF0) == 0xF8C0) && (nx2 = g_rd16(site + 4)) >= 0
                       && (nx & 15) != 13 && (nx & 15) != 15 && (nx2 >> 12) != 13 && (nx2 >> 12) != 15 && !target_hits(site, site + 4)) {
                /* ldr.w / str.w rt,[rn,#imm12] next (a bus word after a clock read, ...): a second helper call, no relocation */
                op[1] = (HOp){ (nx & 0x10) != 0, (nx2 >> 12) & 15, nx & 15, (uint32_t)nx2 & 0xFFF, 0, 0 }; nop = 2; nx = -1; nx2 = -1; span = 6;
            } else if ((nx & 0xF800) >= 0xE800) {                         /* 32-bit next instruction: relocate it too when it is plain data processing */
                nx2 = g_rd16(site + 4); if (nx2 < 0 || !safe32((uint16_t)nx, (uint16_t)nx2) || target_hits(site, site + 4)) ok = 0; else span = 6;
            } else if (!g_noreach && (nx & 0xF500) == 0xB100) {                         /* CBZ / CBNZ rn: stub ends with CB(N)Z over a B.W to the next instruction, then B.W to the target (forward only) */
                uint32_t t = site + 2 + 4 + (uint32_t)((((nx >> 9) & 1) << 6) | (((nx >> 3) & 31) << 1));
                nbr = 15; nbr_t = t; nbr_cb = (uint16_t)(0xB108 | (nx & 0x0800) | (nx & 7)); nx = -1;
            } else if (!g_noreach && (((nx & 0xF000) == 0xD000 && (nx & 0x0F00) < 0x0E00) || (nx & 0xF800) == 0xE000)) {      /* B<cond>.n / B.n: stub ends with B<!cond>.n over a B.W to the target, then B.W to the next instruction */
                int cc = (nx & 0xF000) == 0xD000 ? (nx >> 8) & 15 : 14;
                uint32_t t = site + 2 + 4 + (uint32_t)(cc == 14 ? ((int32_t)((nx & 0x7FF) << 21) >> 20) : (int32_t)(int8_t)(nx & 0xFF) * 2);
                if (t > site && t < site + 4) ok = 0;                     /* a branch back into the patch (to the pair's second half) */
                else { nbr = cc; nbr_t = t; nx = -1; }
            } else if (!safe16((uint16_t)nx)) ok = 0;
        }
        if (!ok) { static int np = -1; if (np < 0) np = getenv("UM2_NOPREPATCH") != NULL; if (np) return NOPE(HW_NEXTBAD); }       /* A/B switch: the constant-load + site variant off */
        if (!ok) {                                        /* the next halfword cannot go along (it is a loop head, a patched site, ...): take the instruction BEFORE the site instead:
                                                           * a constant load, or a plain ALU halfword (hoisted ahead of the stub body).
                                                           * Only when the halfword before that cannot be the first half of a 32-bit instruction (so site - 2 is an instruction start). */
            nop = 1; nx = -1; nx2 = -1; span = 4; nbr = -1;
            int pp = g_rd16(site - 2), pq = g_rd16(site - 4);
            if (pp < 0 || pq < 0 || pq >= 0xE800 || target_hits(site, site) || bm_get(g_pcover, site - 2)) return NOPE(HW_NOPRE);
            /* site - 2 might sit in an IT block (or a 32-bit instruction ends in what looks like one): leave it where it is.  An IT covers up to 4 instructions of up to 4 bytes, so it can be 16 bytes back */
            for (int q = 4; q <= 16; q += 2) {
                int w = g_rd16(site - q); if (w < 0) return NOPE(HW_NOPRE);
                int nit = w & 15 ? 4 - __builtin_ctz(w & 15) : 0;                              /* instructions in the block if this is an IT; 0: a hint such as nop */
                if (!bm_get(g_pcover, site - q) && (w & 0xFF00) == 0xBF00 && nit >= (q - 1) / 4 + 1) return NOPE(HW_NOPRE);
            }
            if ((pp & 0xF800) == 0x4800) {
                uint32_t la = ((site - 2 + 4) & ~3u) + (uint32_t)(pp & 0xFF) * 4;
                if (la < IMG_BASE || la + 4 > IMG_TOP) return NOPE(HW_POOL);
                op[1] = op[0]; op[0] = (HOp){ 1, (pp >> 8) & 7, 0, 0, 1, *(volatile uint32_t *)(uintptr_t)la }; nop = 2; pre = 1;
            } else if (!g_noreach && safe16_pre((uint16_t)pp)) {
                preh = (uint16_t)pp; pre = 2;
            } else return NOPE(HW_NOPRE);
        }
    }
    for (int i = 0; i < nop; i++) if (!op[i].cst && (op[i].rn == 13 || op[i].rn == 15 || op[i].rt == 13 || op[i].rt == 15)) return NOPE(HW_REG);
    if (rmp1 == 14 || rmp1 == 16) return NOPE(HW_REG);                           /* rm = sp / pc */
    uint32_t psite = pre ? site - (itb ? 4u : 2u) : site;               /* first patched address */
    uint16_t k[104]; int n = 0;
    struct { int at; int which; uint32_t val; } lits[8]; int nl = 0;       /* ldr.w literal fixups: index of the offset halfword */
#define E(x) k[n++] = (uint16_t)(x)
    int skip_at = -1, mid_at = -1, else_n = -1;
    if (pre == 3) { skip_at = n; n += 2; }              /* b<!cc>.w end: the IT's condition is false, nothing happens (flags are still the caller's); ITE: b<!cc>.w to the second part */
    int nparts = itp == 2 ? 2 : 1;                      /* ITE: two complete sequences, one instruction each, the first ends with a b.w to the common end */
    for (int part = 0; part < nparts; part++) {
    if (part == 1) { mid_at = n; n += 2; else_n = n; }
    if (pre == 2) E(preh);                              /* the hoisted ALU halfword, on the live registers and flags */
    E(0xE92D); E(0x500F);                               /* push {r0-r3, r12, lr}     */
    E(0xF3EF); E(0x8300);                               /* mrs r3, apsr              */
    E(0xF84D); E(0x3D04);                               /* str r3, [sp, #-4]!        */
    for (int i = nparts == 2 ? part : 0; i < (nparts == 2 ? part + 1 : nop); i++) {
        HOp *o = &op[i];
        if (o->cst) {                                   /* rt = constant */
            int dst = slot_of(o->rt) >= 0 ? 0 : o->rt;
            E(0xF8DF); lits[nl].at = n; lits[nl].which = 0; lits[nl].val = o->cval; nl++; E(dst << 12);
            if (slot_of(o->rt) >= 0) { E(0xF8CD); E(slot_of(o->rt)); }
            continue;
        }
        if (slot_of(o->rn) >= 0) { E(0xF8DD); E(slot_of(o->rn)); } else E(0x4600 | (o->rn << 3));      /* r0 = rn */
        if (o->imm) { E(0xF200 | (((o->imm >> 11) & 1) << 10)); E((((o->imm >> 8) & 7) << 12) | (o->imm & 0xFF)); }   /* addw r0, r0, #imm */
        if (i == 0 && rmp1) {                           /* r1 = rm; add.w r0, r0, r1, lsl #sh */
            int rm = rmp1 - 1;
            if (slot_of(rm) >= 0) { E(0xF8DD); E(0x1000 | slot_of(rm)); } else E(0x4600 | (rm << 3) | 1);
            E(0xEB00); E((uint16_t)(((rmsh & 3) << 6) | 1));
        }
        if (!o->load) { if (slot_of(o->rt) >= 0) { E(0xF8DD); E(0x1000 | slot_of(o->rt)); } else E(0x4600 | (o->rt << 3) | 1); }   /* r1 = rt */
        { void (*th)(void) = g_novfp ? um2_hthunk_lean : (g_spilean && (tl_patch_addr & 0xFFFFC000u) == 0x40080000u) ? (o->load ? (g_srfast ? um2_hthunk_spisr : um2_hthunk_spild) : um2_hthunk_spist) : um2_hthunk;
        E(0xF8DF); lits[nl].at = n; lits[nl].which = 0; lits[nl].val = (uint32_t)(uintptr_t)th; nl++; E(0xC000); }   /* ldr r12, =thunk */
        E(0xF8DF); lits[nl].at = n; lits[nl].which = 0; lits[nl].val = (uint32_t)(uintptr_t)(o->load ? (void *)um2_mload : (void *)um2_mstore); nl++; E(0x3000);   /* ldr r3, =fn */
        E(0xF8DF); lits[nl].at = n; lits[nl].which = 0; lits[nl].val = site + (pre ? 0u : 2u * (uint32_t)i); nl++; E(o->load ? 0x1000 : 0x2000);          /* ldr r1|r2, =site */
        E(0x47E0);                                      /* blx r12: thunk = save d0-d7/FPSCR, host stack, call r3 */
        if (o->load) { if (slot_of(o->rt) >= 0) { E(0xF8CD); E(slot_of(o->rt)); } else E(0x4600 | (o->rt & 7) | ((o->rt & 8) << 4)); }   /* rt = r0 */
    }
    E(0xF85D); E(0x3B04);                               /* ldr r3, [sp], #4          */
    E(0xF383); E(0x8800);                               /* msr apsr_nzcvq, r3        */
    E(0xE8BD); E(0x500F);                               /* pop {r0-r3, r12, lr}      */
    }
    if (nx >= 0) E((uint16_t)nx);                       /* the relocated instruction */
    if (nx2 >= 0) E((uint16_t)nx2);                     /* ... its second halfword */
    int nb_at = -1;
    if (nbr >= 0 && nbr < 14) { E(0xD000 | ((unsigned)(nbr ^ 1) << 8) | 1); nb_at = n; n += 2; }   /* b<!cc>.n over the next B.W: falls through to the b.w to the next instruction */
    else if (nbr == 15) { E(nbr_cb); nb_at = n; n += 2; }                               /* cb(n)z rn, .+6 (taken: over the next B.W) ; b.w fall-through ; b.w target */
    int b_at = n; n += 2;
    if (n & 1) E(0xBF00);
    int lit_base = n; n += 2 * nl;
    uint32_t bytes = (uint32_t)n * 2, base;
    { int pb = patch_begin(psite, (uint32_t)span, bytes, &base); if (pb <= 0) { tl_why = pb == 0 ? HW_BUSY : pb == -2 ? HW_REFUSED : HW_NONE; return (pb < 0 && pb != -2 && !pre) ? 1 : 0; } }      /* -1: another core got there first (run the site again); 0 / -2: not now / never */
    for (int i = 0; i < nl; i++) {
        uint32_t laddr = base + (uint32_t)(lit_base + 2 * i) * 2;
        uint32_t ipc = base + (uint32_t)(lits[i].at - 1) * 2 + 4;
        k[lits[i].at] |= (uint16_t)(laddr - (ipc & ~3u));
        memcpy(k + lit_base + 2 * i, &lits[i].val, 4);
    }
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, (nbr == 14 || nbr == 15) ? nbr_t : psite + (uint32_t)span);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    if (nb_at >= 0) { bw = enc_bw(base + (uint32_t)nb_at * 2, nbr == 15 ? psite + (uint32_t)span : nbr_t); k[nb_at] = (uint16_t)(bw & 0xFFFF); k[nb_at + 1] = (uint16_t)(bw >> 16); }
    if (skip_at >= 0) { bw = enc_bcc(base + (uint32_t)skip_at * 2, base + (uint32_t)(else_n >= 0 ? else_n : b_at) * 2, itcc ^ 1u); k[skip_at] = (uint16_t)(bw & 0xFFFF); k[skip_at + 1] = (uint16_t)(bw >> 16); }
    if (mid_at >= 0) { bw = enc_bw(base + (uint32_t)mid_at * 2, base + (uint32_t)b_at * 2); k[mid_at] = (uint16_t)(bw & 0xFFFF); k[mid_at + 1] = (uint16_t)(bw >> 16); }
    memcpy((void *)(uintptr_t)base, k, (size_t)bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(psite, (uint32_t)span, base);
    g_nhot++;
    { static int dsd = -1; if (dsd < 0) dsd = getenv("UM2_DUMPSTUB") != NULL; if (dsd) { fprintf(stderr, "stubdump %08x:", base); for (int q = 0; q < n; q++) fprintf(stderr, " %04x", k[q]); fputc(10, stderr); } }
    if (opt.bus_verbose) fprintf(stderr, "hot_patch: %08x %s r%d,[r%d,#%u]%s%s%s -> stub %08x%c", site, op[0].load ? "ldr" : "str", op[0].rt, op[0].rn, op[0].imm,
        len == 2 ? "+next16" : ".w", pre == 1 ? "+PRE(site-2)" : pre == 2 ? "+PRE(alu)" : pre == 3 ? (itp == 1 ? "+ITT" : itp == 2 ? "+ITE" : "+IT") : nop == 2 ? "+op2" : "", nbr >= 0 ? "+branch" : "", base, 10);
    return pre ? 0 : 1;      /* pre: the site is the SECOND halfword of the new b.w, so it is not re-run: this access goes on through the normal emulation and resumes at site + 2 = the end of the patch */
}

/* LDA/STL family (ARMv8-M acquire/release).  The in-place rewrite to a plain LDR/STR/LDREX/STREX loses the ordering, which matters between the two guest
 * cores on the weakly ordered A9, so the site becomes `b.w stub`: load-acquire = the plain load, then `dmb ish`; store-release = `dmb ish`, then the plain
 * store.  Same registers, no stack, flags untouched.  Returns 1 (patched, or another core did it), 2 (try again: could not park the other core), 0 (no stub:
 * IT block, no stub area, overlap: the caller falls back to the in-place rewrite). */
static uint32_t g_nv8;
static int v8_stub(const Cpu *c, uint32_t site, uint16_t h1, uint16_t n1, uint16_t n2) {
    if (!opt.bus || !g_stub_ready || (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) return 0;
    int load = (h1 & 0xFFF0) == 0xE8D0;
    uint16_t k[8]; int n = 0;
    if (!load) { k[n++] = 0xF3BF; k[n++] = 0x8F5B; }               /* dmb ish */
    k[n++] = n1; k[n++] = n2;
    if (load) { k[n++] = 0xF3BF; k[n++] = 0x8F5B; }
    int b_at = n; n += 2;
    uint32_t bytes = (uint32_t)n * 2, base;
    int pb = patch_begin(site, 4, bytes, &base);
    if (pb == -1) return 1;
    if (pb == 0) return 2;
    if (pb < 0) return 0;
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + 4);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    memcpy((void *)(uintptr_t)base, k, bytes);
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, 4, base);
    g_nv8++;
    if (opt.bus_verbose) fprintf(stderr, "v8_stub: %08x %04x.. -> %04x %04x + dmb, stub %08x%c", site, h1, n1, n2, base, 10);
    return 1;
}

/* PRIMASK sites (--bus), 28 Oct 2026.  `mrs rd, PRIMASK ; cpsid i` (save_and_disable_interrupts, the SDK's spin locks / mutexes) and `msr PRIMASK, rn` each
 * cost a SIGILL trap (~50 us on the A9, ~20 per BattleZone frame).  The first trap at a site now turns it into `b.w stub`: the stub saves what a call needs,
 * runs um2_prim_save / um2_prim_restore through the VFP-safe thunk (the per-core soft PRIMASK, the same variable the trap path and the interrupt delivery use)
 * and returns to the instruction after the pair.  Unmasking with an interrupt pending wakes the irq thread, which kicks the core (the trap path delivered at
 * the end of the trap; this is a few tens of microseconds later).  UM2_NOPRIMPATCH=1 = trap path only. */
uint32_t um2_prim_save(void) { Nvic *n = &g_nv[tl_core & 1]; uint32_t o = n->prim; n->prim = 1; return o; }
uint32_t um2_prim_restore(uint32_t v) {
    Nvic *n = &g_nv[tl_core & 1]; n->prim = v & 1;
    if (!(v & 1) && g_irq_on && nvic_pick(n) >= 0) irq_wake();
    return 0;
}
static uint32_t g_nprim;
static int prim_patch(const Cpu *c, uint32_t site, int mrs, int reg, int span) {
    static int off = -1; if (off < 0) off = getenv("UM2_NOPRIMPATCH") != NULL; if (off) return 0;
    if (!opt.bus || !g_stub_ready || (c->r[16] & ((0x3Fu << 10) | (3u << 25))) || reg == 13 || reg == 15) return 0;
    if (span == 6 && target_hits(site, site + 4)) return 0;                   /* the cpsid is a branch target: stay on the trap path */
    uint16_t k[48]; int n = 0;
    struct { int at; uint32_t val; } lits[3]; int nl = 0;
#define E(x) k[n++] = (uint16_t)(x)
    E(0xE92D); E(0x500F);                               /* push {r0-r3, r12, lr}     */
    E(0xF3EF); E(0x8300);                               /* mrs r3, apsr              */
    E(0xF84D); E(0x3D04);                               /* str r3, [sp, #-4]!        */
    if (!mrs) { if (slot_of(reg) >= 0) { E(0xF8DD); E(slot_of(reg)); } else E(0x4600 | (reg << 3)); }      /* r0 = rn (the value written) */
    E(0xF8DF); lits[nl].at = n; lits[nl].val = (uint32_t)(uintptr_t)(g_novfp ? um2_hthunk_lean : um2_hthunk); nl++; E(0xC000);   /* ldr r12, =thunk */
    E(0xF8DF); lits[nl].at = n; lits[nl].val = (uint32_t)(uintptr_t)(mrs ? (void *)um2_prim_save : (void *)um2_prim_restore); nl++; E(0x3000);   /* ldr r3, =fn */
    E(0x47E0);                                          /* blx r12 */
    if (mrs) { if (slot_of(reg) >= 0) { E(0xF8CD); E(slot_of(reg)); } else E(0x4600 | (reg & 7) | ((reg & 8) << 4)); }   /* rd = r0 (the old PRIMASK) */
    E(0xF85D); E(0x3B04);                               /* ldr r3, [sp], #4          */
    E(0xF383); E(0x8800);                               /* msr apsr_nzcvq, r3        */
    E(0xE8BD); E(0x500F);                               /* pop {r0-r3, r12, lr}      */
    int b_at = n; n += 2;
    if (n & 1) E(0xBF00);
    int lit_base = n; n += 2 * nl;
    uint32_t bytes = (uint32_t)n * 2, base;
    { int pb = patch_begin(site, (uint32_t)span, bytes, &base); if (pb <= 0) return pb < 0 && pb != -2; }
    for (int i = 0; i < nl; i++) {
        uint32_t laddr = base + (uint32_t)(lit_base + 2 * i) * 2;
        uint32_t ipc = base + (uint32_t)(lits[i].at - 1) * 2 + 4;
        k[lits[i].at] |= (uint16_t)(laddr - (ipc & ~3u));
        memcpy(k + lit_base + 2 * i, &lits[i].val, 4);
    }
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + (uint32_t)span);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    memcpy((void *)(uintptr_t)base, k, (size_t)bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, (uint32_t)span, base);
    g_nprim++;
    if (opt.bus_verbose) fprintf(stderr, "prim_patch: %08x %s r%d span %d -> stub %08x%c", site, mrs ? "mrs+cpsid" : "msr", reg, span, base, 10);
    return 1;
}

/* Native SVC entry (1 Nov 2026).  The UVMC2 runtime's system calls are veneers `svc #n ; bx lr`.  On the A9 every one costs two signal round trips (about 50 us each): the udf that
 * stands for the svc, and the fetch fault when the image's SVCall handler returns to EXC_RETURN.  Enduro makes 40 a frame, Defender 15.  After a few traps the veneer becomes
 * `b.w stub`: the stub does the ARMv8-M exception entry in Thumb (the C helper um2_svc_enter writes the frame and the NVIC bookkeeping, exactly what exc_enter does), jumps to the
 * image's own SVCall handler with lr = um2_irqret (the native ISR return thunk), and the exception returns to a copy of the veneer's second instruction inside the stub, so a
 * handler that changes the stacked lr behaves as before.  Preceded in the stub by the udf halfword the handler reads at return pc - 2 (it takes the call number from there).
 * Not patched: IT blocks, a second instruction that cannot be relocated (a pc-relative load), a branch target on it.  UM2_NOSVCPATCH=1 = every svc on the trap path. */
uint32_t um2_svc_enter(uint32_t *blk, uint32_t ret_pc, uint32_t site) {
    uint32_t flags = blk[0], r[6]; memcpy(r, blk + 1, sizeof r);                     /* r0 r1 r2 r3 r12 lr, as the stub pushed them; the frame about to be written overlaps this block */
    uint32_t old_sp = (uint32_t)(uintptr_t)blk + 28, base = (old_sp - 32) & ~7u, pad = ((old_sp - 32) & 4) ? 1u : 0u;
    uint32_t *f = (uint32_t *)(uintptr_t)base;
    memcpy(f, r, sizeof r); f[6] = ret_pc; f[7] = (flags & 0xF8000000u) | (1u << 24) | (pad << 9);
    Nvic *n = &g_nv[tl_core & 1]; int d = n->depth;
    if (d < 7) { n->sprio[d] = 0; n->sirq[d] = -1; n->sret[d] = ret_pc; n->depth++; }
    tl_fpv[d & 7] = 0;                                                                /* an SVC frame carries no VFP state (as on the trap path): the return thunk must not restore a stale one */
    g_svc_nat[((site - IMG_BASE) >> 1) < sizeof g_patch_orig_n ? g_patch_orig_n[(site - IMG_BASE) >> 1] : 0]++;
    uint32_t vt = vtor_now(); int h = g_rd16(vt + 44), h2 = g_rd16(vt + 46);
    return (uint32_t)h | ((uint32_t)h2 << 16);
}
static uint32_t g_nsvcp;
static int svc_patch(const Cpu *c, uint32_t site, unsigned num) {
    static int off = -1; if (off < 0) off = getenv("UM2_NOSVCPATCH") != NULL; if (off) return 0;
    if (!opt.bus || !g_stub_ready || !g_irq_thunk || (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) return 0;
    int nx = g_rd16(site + 2);
    static int nolit = -1; if (nolit < 0) nolit = getenv("UM2_NOSVCLIT") != NULL;       /* UM2_NOSVCLIT=1: a pc-relative second instruction keeps the veneer on the trap path (as before) */
    int cst = 0; uint32_t cval = 0;                     /* the second instruction is `ldr rt,[pc,#imm]` with the pool word read now (image constants, as the hot-patch pairs do): the stub loads the constant */
    if (nx >= 0 && !nolit && (nx & 0xF800) == 0x4800 && !target_hits(site, site + 2)) {
        uint32_t la = ((site + 2 + 4) & ~3u) + (uint32_t)(nx & 0xFF) * 4;
        if (la < IMG_BASE || la + 4 > IMG_TOP) return 0;
        cst = 1; cval = *(volatile uint32_t *)(uintptr_t)la;
    } else if (nx < 0 || !safe16((uint16_t)nx) || target_hits(site, site + 2)) return 0;
    int xfer = !cst && ((nx & 0xFF87) == 0x4700 || ((nx & 0xFE00) == 0xBC00 && (nx & 0x100)));      /* bx rm / pop {.., pc}: the relocated instruction leaves the stub itself */
    uint16_t k[48]; int n = 0;
    struct { int at; uint32_t val; } lits[6]; int nl = 0;
    int x_lit = -1;
#define E(x) k[n++] = (uint16_t)(x)
#define LIT(rt, v) do { E(0xF8DF); lits[nl].at = n; lits[nl].val = (v); nl++; E((rt) << 12); } while (0)
    E(0xE92D); E(0x500F);                               /* push {r0-r3, r12, lr}     */
    E(0xF3EF); E(0x8300);                               /* mrs r3, apsr              */
    E(0xF84D); E(0x3D04);                               /* str r3, [sp, #-4]!        */
    E(0x4668);                                          /* mov r0, sp   (the block)  */
    E(0xF1AD); E(0x0104);                               /* sub.w r1, sp, #4          */
    E(0xF021); E(0x0107);                               /* bic.w r1, r1, #7          */
    E(0x468D);                                          /* mov sp, r1   (the frame base; from here an interrupt frame goes below it) */
    x_lit = nl; LIT(1, 0);                              /* ldr r1, =return pc (the relocated instruction, below) */
    LIT(2, site);                                       /* ldr r2, =site             */
    LIT(12, (uint32_t)(uintptr_t)(g_novfp ? um2_hthunk_lean : um2_hthunk));
    LIT(3, (uint32_t)(uintptr_t)um2_svc_enter);
    E(0x47E0);                                          /* blx r12: thunk -> um2_svc_enter(r0, r1, r2) = the SVCall handler */
    E(0x4684);                                          /* mov r12, r0               */
    LIT(14, (uint32_t)(uintptr_t)um2_irqret);           /* ldr lr, =um2_irqret       */
    E(0x4760);                                          /* bx r12                    */
    E(0xDE00 | (num & 0xFF));                           /* the udf the handler reads at return pc - 2 */
    int x_at = n;                                       /* the relocated second instruction: the exception returns here */
    if (cst) LIT((nx >> 8) & 7, cval); else E((uint16_t)nx);
    int b_at = -1; if (!xfer) { b_at = n; n += 2; }     /* b.w site + 4 */
    if (n & 1) E(0xBF00);
    int lit_base = n; n += 2 * nl;
    uint32_t bytes = (uint32_t)n * 2, base;
    { int pb = patch_begin(site, 4, bytes, &base); if (pb <= 0) return pb < 0 && pb != -2; }      /* -1: another core got there first (the site is a b.w now) */
    lits[x_lit].val = base + (uint32_t)x_at * 2;
    for (int i = 0; i < nl; i++) {
        uint32_t laddr = base + (uint32_t)(lit_base + 2 * i) * 2;
        uint32_t ipc = base + (uint32_t)(lits[i].at - 1) * 2 + 4;
        k[lits[i].at] |= (uint16_t)(laddr - (ipc & ~3u));
        memcpy(k + lit_base + 2 * i, &lits[i].val, 4);
    }
    if (b_at >= 0) { uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + 4); k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16); }
    memcpy((void *)(uintptr_t)base, k, (size_t)bytes);
#undef LIT
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, 4, base);
    g_nsvcp++;
    if (opt.bus_verbose) fprintf(stderr, "svc_patch: %08x svc #%u + %04x -> stub %08x%c", site, num, nx, base, 10);
    return 1;
}
/* VRINTA/VRINTN/VRINTP/VRINTM.F32 (ARMv8 FP: round to an integral value; UNDEFINED on the A9).  VecDoom's renderer executes ~270 of them a frame (floor / ceil of screen
 * coordinates, 20 sites): each was a ~50 us trap AND was skipped, so the destination register kept its old value (wrong geometry, not only slow).  The first trap now
 * emulates the instruction on the saved VFP state of the signal frame, and the site becomes `b.w stub`: vmov r0, sm ; um2_vrint(r0, mode) through the VFP-safe thunk ;
 * vmov sd, r0.  rm: 0 = A (to nearest, ties away), 1 = N (ties to even), 2 = P (+inf, ceil), 3 = M (-inf, floor).  Exact in float arithmetic: |x| < 2^23 only has a
 * fraction, anything larger (and inf / nan) is returned unchanged.  UM2_NOVRINT=1: the old skip (A/B). */
uint32_t um2_vrint(uint32_t bits, uint32_t rm) {
    float x, t, r; memcpy(&x, &bits, 4);
    if (!(__builtin_fabsf(x) < 8388608.0f) || x == 0.0f) return bits;             /* big, inf, nan, +-0 */
    t = (float)(int32_t)x;                                                         /* trunc */
    switch (rm & 3) {
    case 2: r = (t < x) ? t + 1.0f : t; break;                                     /* ceil */
    case 3: r = (t > x) ? t - 1.0f : t; break;                                     /* floor */
    default: {
        float d = __builtin_fabsf(x - t), away = x < 0 ? -1.0f : 1.0f;              /* distance to the truncated value */
        if (d > 0.5f) r = t + away; else if (d < 0.5f) r = t;
        else r = (rm & 3) == 0 ? t + away : (((int32_t)t & 1) ? t + away : t);      /* tie: away from zero (A) / to even (N) */
    } }
    if (r == 0.0f) r = __builtin_copysignf(0.0f, x);                               /* -0.3 -> -0.0, as the instruction gives */
    uint32_t o; memcpy(&o, &r, 4); return o;
}
static uint32_t g_nvrint;
static int vrint_patch(const Cpu *c, uint32_t site, unsigned rm, unsigned sd, unsigned sm) {
    static int off = -1; if (off < 0) off = getenv("UM2_NOVRINTPATCH") != NULL;
    if (off || !opt.bus || !g_stub_ready || (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) return 0;
    uint16_t k[40]; int n = 0;
    struct { int at; uint32_t val; } lits[2]; int nl = 0;
#define E(x) k[n++] = (uint16_t)(x)
    E(0xE92D); E(0x500F);                               /* push {r0-r3, r12, lr}     */
    E(0xF3EF); E(0x8300);                               /* mrs r3, apsr              */
    E(0xF84D); E(0x3D04);                               /* str r3, [sp, #-4]!        */
    E(0xEE10 | (sm >> 1)); E(0x0A10 | ((sm & 1) << 7)); /* vmov r0, sm               */
    E(0xF240); E(0x0100 | (rm & 3));                    /* movw r1, #rm              */
    E(0xF8DF); lits[nl].at = n; lits[nl].val = (uint32_t)(uintptr_t)(g_novfp ? um2_hthunk_lean : um2_hthunk); nl++; E(0xC000);   /* ldr r12, =thunk */
    E(0xF8DF); lits[nl].at = n; lits[nl].val = (uint32_t)(uintptr_t)um2_vrint; nl++; E(0x3000);                                   /* ldr r3, =um2_vrint */
    E(0x47E0);                                          /* blx r12                   */
    E(0xEE00 | (sd >> 1)); E(0x0A10 | ((sd & 1) << 7)); /* vmov sd, r0               */
    E(0xF85D); E(0x3B04);                               /* ldr r3, [sp], #4          */
    E(0xF383); E(0x8800);                               /* msr apsr_nzcvq, r3        */
    E(0xE8BD); E(0x500F);                               /* pop {r0-r3, r12, lr}      */
    int b_at = n; n += 2;
    if (n & 1) E(0xBF00);
    int lit_base = n; n += 2 * nl;
    uint32_t bytes = (uint32_t)n * 2, base;
    { int pb = patch_begin(site, 4, bytes, &base); if (pb <= 0) return pb < 0 && pb != -2; }
    for (int i = 0; i < nl; i++) {
        uint32_t laddr = base + (uint32_t)(lit_base + 2 * i) * 2;
        uint32_t ipc = base + (uint32_t)(lits[i].at - 1) * 2 + 4;
        k[lits[i].at] |= (uint16_t)(laddr - (ipc & ~3u));
        memcpy(k + lit_base + 2 * i, &lits[i].val, 4);
    }
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + 4);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    memcpy((void *)(uintptr_t)base, k, (size_t)bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, 4, base);
    g_nvrint++;
    if (opt.bus_verbose) fprintf(stderr, "vrint_patch: %08x vrint%c s%u, s%u -> stub %08x%c", site, "anpm"[rm & 3], sd, sm, base, 10);
    return 1;
}
/* VMAXNM/VMINNM.F32 (ARMv8 FP, IEEE 754-2008 maxNum / minNum; UNDEFINED on the A9).  Vector Tank clamps with a vmaxnm + vminnm pair; until 5 Nov 2026 both were
 * skipped (the destination kept its old value) and each cost a trap: ~2 a frame on the title, then ~26000 a second once the game proper starts, with no frame
 * ever finished.  Same scheme as VRINT: the first trap emulates on the signal frame's VFP state, then the site becomes `b.w stub`: vmov r0, sn ; vmov r1, sm ;
 * um2_vmaxnm(r0, r1, op) through the VFP-safe thunk ; vmov sd, r0.  op: 0 = max, 1 = min.  A quiet NaN against a number gives the number; a signalling NaN gives
 * that NaN quieted (first operand first); +0 is above -0.  UM2_NOVMAXNM=1: the old skip (A/B); UM2_NOVMAXNMPATCH=1: emulate on every trap. */
uint32_t um2_vmaxnm(uint32_t a, uint32_t b, uint32_t op) {
    int an = (a & 0x7FFFFFFFu) > 0x7F800000u, bn = (b & 0x7FFFFFFFu) > 0x7F800000u;
    if (an || bn) {
        if (an && !(a & 0x00400000u)) return a | 0x00400000u;
        if (bn && !(b & 0x00400000u)) return b | 0x00400000u;
        return an ? (bn ? a : b) : a;
    }
    float x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    if (x == y) return op ? (a | b) : (a & b);                                     /* equal: only +0 / -0 differ in bits */
    return (op ? x < y : x > y) ? a : b;
}
static uint32_t g_nvmaxnm;
static int vmaxnm_patch(const Cpu *c, uint32_t site, unsigned op, unsigned sd, unsigned sn, unsigned sm) {
    static int off = -1; if (off < 0) off = getenv("UM2_NOVMAXNMPATCH") != NULL;
    if (off || !opt.bus || !g_stub_ready || (c->r[16] & ((0x3Fu << 10) | (3u << 25)))) return 0;
    uint16_t k[40]; int n = 0;
    struct { int at; uint32_t val; } lits[2]; int nl = 0;
#define E(x) k[n++] = (uint16_t)(x)
    E(0xE92D); E(0x500F);                               /* push {r0-r3, r12, lr}     */
    E(0xF3EF); E(0x8300);                               /* mrs r3, apsr              */
    E(0xF84D); E(0x3D04);                               /* str r3, [sp, #-4]!        */
    E(0xEE10 | (sn >> 1)); E(0x0A10 | ((sn & 1) << 7)); /* vmov r0, sn               */
    E(0xEE10 | (sm >> 1)); E(0x1A10 | ((sm & 1) << 7)); /* vmov r1, sm               */
    E(0xF240); E(0x0200 | (op & 1));                    /* movw r2, #op              */
    E(0xF8DF); lits[nl].at = n; lits[nl].val = (uint32_t)(uintptr_t)(g_novfp ? um2_hthunk_lean : um2_hthunk); nl++; E(0xC000);   /* ldr r12, =thunk */
    E(0xF8DF); lits[nl].at = n; lits[nl].val = (uint32_t)(uintptr_t)um2_vmaxnm; nl++; E(0x3000);                                  /* ldr r3, =um2_vmaxnm */
    E(0x47E0);                                          /* blx r12                   */
    E(0xEE00 | (sd >> 1)); E(0x0A10 | ((sd & 1) << 7)); /* vmov sd, r0               */
    E(0xF85D); E(0x3B04);                               /* ldr r3, [sp], #4          */
    E(0xF383); E(0x8800);                               /* msr apsr_nzcvq, r3        */
    E(0xE8BD); E(0x500F);                               /* pop {r0-r3, r12, lr}      */
    int b_at = n; n += 2;
    if (n & 1) E(0xBF00);
    int lit_base = n; n += 2 * nl;
    uint32_t bytes = (uint32_t)n * 2, base;
    { int pb = patch_begin(site, 4, bytes, &base); if (pb <= 0) return pb < 0 && pb != -2; }
    for (int i = 0; i < nl; i++) {
        uint32_t laddr = base + (uint32_t)(lit_base + 2 * i) * 2;
        uint32_t ipc = base + (uint32_t)(lits[i].at - 1) * 2 + 4;
        k[lits[i].at] |= (uint16_t)(laddr - (ipc & ~3u));
        memcpy(k + lit_base + 2 * i, &lits[i].val, 4);
    }
    uint32_t bw = enc_bw(base + (uint32_t)b_at * 2, site + 4);
    k[b_at] = (uint16_t)(bw & 0xFFFF); k[b_at + 1] = (uint16_t)(bw >> 16);
    memcpy((void *)(uintptr_t)base, k, (size_t)bytes);
#undef E
    __builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)(base + bytes));
    patch_end(site, 4, base);
    g_nvmaxnm++;
    if (opt.bus_verbose) fprintf(stderr, "vmaxnm_patch: %08x v%snm s%u, s%u, s%u -> stub %08x%c", site, op ? "min" : "max", sd, sn, sm, base, 10);
    return 1;
}
/* qemu-user hands out its mmap area from 0x40000000 up, and guest_jump has just freed the stack that sat there: later mmaps (thread
 * stacks, ...) would land on top of the RP2350 peripheral addresses and make them plain memory.  Reserve that range before anything
 * else maps.  No-op on a real kernel (nothing is there). */
static void reserve_mmio(void) {
    static int done; if (done) return; done = 1;
    mmap((void *)0x40000000u, 0x00801000u, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);   /* where qemu's stack was: every peripheral below 0x40801000 */
}

static __thread uint32_t tl_rpc, tl_rn;
static int cpu_trap(int sig, Cpu *c, uint32_t fault_addr, int svc_from_syscall, uint32_t svc_hw) {
    reserve_mmio();
    uint32_t pc = c->r[15];
    if (opt.bus && !svc_from_syscall && is_patched(pc)) {      /* a signal that crossed a patch of this very site (any signal, before decode and skip): run the stub, not the old instruction's emulation */
        if (pc != tl_rpc) { tl_rpc = pc; tl_rn = 0; }
        if (++tl_rn < 1000) return ACT_CONT;                   /* the patcher parks the other core, so this is rare (stale fetch); 1000 in a row = a broken patch: fall through and report it */
    }
    int h = g_rd16(pc);
    uint16_t h1, h2 = 0; int len;
    if (opt.bus && (pc & ~7u) == 0xFFFFFFF8u) { g_excret++; exc_return(c); if (g_irq_on) irq_levels(); irq_poll(c); return ACT_CONT; }    /* EXC_RETURN fetched */
    g_events++; tl_n_trap++; g_lastpc = pc;
    g_ring[g_events & 31][0] = sig; g_ring[g_events & 31][1] = pc; g_ring[g_events & 31][2] = fault_addr;
    if (opt.trace) { printf("trap sig=%d pc=%08x addr=%08x cpsr=%08x r0=%08x r1=%08x\n", sig, pc, fault_addr, c->r[16], c->r[0], c->r[1]); }
    if (svc_from_syscall) {                       /* ptrace/seccomp: c->r[15] is the address after the svc */
        unsigned n = svc_hw & 0xFF; int isnew;
        Ev *e = ev_get(K_SVCN, n, 0, NULL); if (e) { e->n++; }
        e = ev_get(K_SVC, pc - 2, 0, &isnew);
        if (e) { e->n++; if (isnew) { e->x = n; e->y = c->r[0]; e->z = c->r[1]; logf_("[SVC] #%u at pc=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", n, pc - 2, c->r[0], c->r[1], c->r[2], c->r[3]); } }
        c->r[0] = 0;
        goto done;
    }
    if (h < 0 && (sig == SIGSEGV || sig == SIGBUS) && (pc & ~1u) < 0x8000) { h = 0xBF00; }   /* fetch from bootrom area: handled below */
    if (h < 0) { printf("STOP: sig %d with pc=%08x outside the image (fault addr %08x) -- guest jumped into the void\n", sig, pc, fault_addr); return ACT_STOP; }
    h1 = (uint16_t)h; len = t32(h1) ? 4 : 2;
    if (len == 4) { int t = g_rd16(pc + 2); if (t < 0) return ACT_STOP; h2 = (uint16_t)t; }
    if (!(c->r[16] & 0x20)) { printf("STOP: [core %d depth %d prim %u] ARM-state fault pc=%08x sig %d lr=%08x sp=%08x r0-r3=%08x %08x %08x %08x cpsr=%08x\n", tl_core, g_nv[tl_core & 1].depth, (unsigned)g_nv[tl_core & 1].prim, pc, sig, c->r[14], c->r[13], c->r[0], c->r[1], c->r[2], c->r[3], c->r[16]); return ACT_STOP; }

    if (len == 4 && (sig == SIGILL || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP)) {
        /* M-profile encodings that alias ARMv7-A instructions (TT = STREX with Rt=PC -> data abort; MRS/MSR SYSm -> CPSR ops)
         * are caught here whatever signal they produce; sites found by the load-time scan hold UDF.W and the original is looked up. */
        uint16_t o1 = h1, o2 = h2; int pi = pat_find(pc);
        if (pi >= 0) { o1 = g_pat[pi].h1; o2 = g_pat[pi].h2; }
        if (m33_shim(c, pc, o1, o2, pi >= 0)) goto done;
    }
    if ((sig == SIGBUS || sig == SIGSEGV) && len == 2 && (h1 & 0xFF00) == 0xBE00) sig = SIGTRAP;   /* BKPT may arrive as a prefetch-abort signal */
    if (sig == SIGILL || sig == SIGTRAP || sig == SIGFPE) {
        if ((sig == SIGILL || sig == SIGTRAP) && pc >= IMG_BASE && pc < IMG_TOP && (g_patched[((pc - IMG_BASE) >> 1) >> 3] >> (((pc - IMG_BASE) >> 1) & 7) & 1)) {
            unsigned n = g_patch_orig_n[(pc - IMG_BASE) >> 1]; int isnew;       /* patched svc */
            Ev *e = ev_get(K_SVCN, n, 0, NULL); if (e) e->n++;
            e = ev_get(K_SVC, pc, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = n; e->y = c->r[0]; e->z = c->r[1]; logf_("[SVC] #%u at pc=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", n, pc, c->r[0], c->r[1], c->r[2], c->r[3]); } }
            if (opt.bus) {
                if (e && (e->n == 6 || (e->n > 6 && e->n < 520 && ((e->n - 6) & 7) == 0)) && svc_patch(c, pc, n)) return ACT_CONT;     /* now `b.w stub`: run the site again (before any frame is built: no exception returns inside the span) */
                if (!exc_enter(c, pc + 2, 11)) { printf("STOP: cannot enter SVCall (vector unreadable)\n"); return ACT_STOP; } goto done; }
            c->r[0] = 0; skip(c, 2); goto done;
        }
        if (sig == SIGILL && len == 4 && (((h1 & 0xFFF0) == 0xFB90) || ((h1 & 0xFFF0) == 0xFBB0)) && (h2 & 0xF0F0) == 0xF0F0) {
            int sgn = (h1 & 0xFFF0) == 0xFB90, rd = (h2 >> 8) & 15, rn = h1 & 15, rm = h2 & 15, isnew;
            uint32_t a = c->r[rn], b = c->r[rm], q;
            if (!b) q = 0; else if (sgn) q = ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b); else q = a / b;
            c->r[rd] = q;
            if (div_patch(c, pc, sgn, rd, rn, rm)) g_npatched_div++;
            Ev *e = ev_get(K_ILL, pc, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, "%s [emulated]", sgn ? "SDIV" : "UDIV");
                logf_("[ILL] pc=%08x insn=%04x %04x %s emulated\n", pc, h1, h2, e->note); } }
            skip(c, 4); goto done;
        }
        if (sig == SIGILL && len == 4 && (h1 & 0xFF80) == 0xFE00 && (h2 & 0x0F50) == 0x0A00) {     /* VSEL.F32: emulate, then patch to a stub (see vsel_patch) */
            unsigned cc = (h1 >> 4) & 3, sd = ((h2 >> 12) & 15) * 2 + ((h1 >> 6) & 1), sn = (h1 & 15) * 2 + ((h2 >> 7) & 1), sm = (h2 & 15) * 2 + ((h2 >> 5) & 1);
            uint32_t *vf = tl_uc ? uc_vfp(tl_uc) : NULL; int isnew;
            if (vf) { uint32_t f = c->r[16]; int N = (f >> 31) & 1, Z = (f >> 30) & 1, V = (f >> 28) & 1; int t = cc == 0 ? Z : cc == 1 ? V : cc == 2 ? (N == V) : (!Z && N == V); vf[sd] = vf[t ? sn : sm]; }
            int st = vsel_patch(c, pc, cc, sd, sn, sm);
            Ev *e = ev_get(K_ILL, pc, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, "VSEL.F32 [%s%s]", vf ? "emulated" : "skipped, no VFP frame", st ? ", stub" : "");
                logf_("[ILL] pc=%08x insn=%04x %04x VSEL.F32 %s%s\n", pc, h1, h2, vf ? "emulated" : "skipped (no VFP state in the signal frame)", st ? ", patched to a stub" : ""); } }
            skip(c, 4); goto done;
        }
        if (sig == SIGILL && len == 4 && !g_novrint && (h1 & 0xFFBC) == 0xFEB8 && (h2 & 0x0F50) == 0x0A40) {     /* VRINTA/N/P/M.F32: emulate, then patch to a stub (see vrint_patch) */
            unsigned rm = h1 & 3, sd = ((h2 >> 12) & 15) * 2 + ((h1 >> 6) & 1), sm = (h2 & 15) * 2 + ((h2 >> 5) & 1);
            uint32_t *vf = tl_uc ? uc_vfp(tl_uc) : NULL; int isnew;
            if (vf) vf[sd] = um2_vrint(vf[sm], rm);
            int st = vf ? vrint_patch(c, pc, rm, sd, sm) : 0;
            Ev *e = ev_get(K_ILL, pc, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, "VRINT%c.F32 [%s%s]", "ANPM"[rm], vf ? "emulated" : "skipped, no VFP frame", st ? ", stub" : "");
                logf_("[ILL] pc=%08x insn=%04x %04x VRINT%c.F32 %s%s\n", pc, h1, h2, "ANPM"[rm], vf ? "emulated" : "skipped (no VFP state in the signal frame)", st ? ", patched to a stub" : ""); } }
            skip(c, 4); goto done;
        }
        if (sig == SIGILL && len == 4 && !g_novmaxnm && (h1 & 0xFFB0) == 0xFE80 && (h2 & 0x0F10) == 0x0A00) {     /* VMAXNM/VMINNM.F32: emulate, then patch to a stub (see vmaxnm_patch) */
            unsigned op = (h2 >> 6) & 1, sd = ((h2 >> 12) & 15) * 2 + ((h1 >> 6) & 1), sn = (h1 & 15) * 2 + ((h2 >> 7) & 1), sm = (h2 & 15) * 2 + ((h2 >> 5) & 1);
            uint32_t *vf = tl_uc ? uc_vfp(tl_uc) : NULL; int isnew;
            if (vf) vf[sd] = um2_vmaxnm(vf[sn], vf[sm], op);
            int st = vf ? vmaxnm_patch(c, pc, op, sd, sn, sm) : 0;
            Ev *e = ev_get(K_ILL, pc, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, "V%sNM.F32 [%s%s]", op ? "MIN" : "MAX", vf ? "emulated" : "skipped, no VFP frame", st ? ", stub" : "");
                logf_("[ILL] pc=%08x insn=%04x %04x V%sNM.F32 %s%s\n", pc, h1, h2, op ? "MIN" : "MAX", vf ? "emulated" : "skipped (no VFP state in the signal frame)", st ? ", patched to a stub" : ""); } }
            skip(c, 4); goto done;
        }
        uint16_t fh;
        if (sig == SIGILL && len == 4 && g_wr16 && fma_rewrite(h1, h2, &fh)) {
            /* Cortex-M33 FPv5 fused multiply-accumulate (VFMA/VFMS/VFNMA/VFNMS, single precision): the A9 has VFPv3 only.  Skipping it
             * (as for other unknown instructions) lost the accumulate AND cost a ~50 us trap per execution; Punch-Out's float audio
             * filter runs ~50k of them per second.  Rewrite the first halfword in place to the chained VMLA/VMLS/VNMLA/VNMLS of the
             * same operands (result differs from the fused one only in the last bit) and run the instruction again.  One halfword
             * store, so a second core hitting the same site at the same moment is harmless. */
            int isnew;
            g_wr16(pc, fh);
            Ev *e = ev_get(K_ILL, pc, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, "VFMA-class -> VMLA-class [rewritten]");
                logf_("[ILL] pc=%08x insn=%04x %04x VFPv4 fused multiply-accumulate rewritten to %04x (chained VMLA family)\n", pc, h1, h2, h1); } }
            if (e && e->n > 50) { printf("STOP: VFMA rewrite at %08x does not take effect\n", pc); return ACT_STOP; }
            goto done;
        }
        if (sig == SIGILL && len == 4) {
            uint16_t n1, n2; int isnew;
            if (v8_rewrite(h1, h2, &n1, &n2) && g_wr16) {
                Ev *e = ev_get(K_ILL, pc, 0, &isnew); int st = v8_stub(c, pc, h1, n1, n2);
                if (st == 2 && (e ? ++e->z : 100u) < 100) goto done;   /* the other core could not be parked just now: the site traps again at once (100 tries per site) */
                if (st != 1) { g_wr16(pc, n1); g_wr16(pc + 2, n2); }     /* no stub possible (IT block, no stub area): the plain instruction, without the barrier */
                if (e) { int first = !e->n; e->n++; if (first) { e->x = h1; e->y = h2; snprintf(e->note, sizeof e->note, st == 1 ? "LDA/STL -> %04x %04x + dmb [stub]" : "LDA/STL -> %04x %04x [rewritten]", n1, n2);
                    logf_("[ILL] pc=%08x insn=%04x %04x ARMv8 acquire/release %s %04x %04x\n", pc, h1, h2, st == 1 ? "stub with dmb ish around" : "rewritten to", n1, n2); } }
                if (e && e->n > 50 && st != 1) { printf("STOP: rewrite at %08x does not take effect\n", pc); return ACT_STOP; }
                goto done;                                 /* re-execute the patched instruction */
            }
        }
        char cb[96]; const char *cls = classify(h1, h2, len, cb, sizeof cb);
        int isnew; Ev *e = ev_get(sig == SIGTRAP ? K_TRAP : K_ILL, pc, 0, &isnew);
        if (e) { e->n++; if (isnew) { e->x = h1; e->y = sig == SIGTRAP ? c->r[14] : h2; e->z = c->r[0]; snprintf(e->note, sizeof e->note, "%s", cls ? cls : "?");
            logf_("[%s] pc=%08x insn=%04x %04x %s lr=%08x sp=%08x (skipped)\n", sig == SIGTRAP ? "TRAP" : "ILL", pc, h1, len == 4 ? h2 : 0, cls ? cls : "unclassified", c->r[14], c->r[13]); }
            if (sig == SIGTRAP && isnew) {         /* plausible return addresses (Thumb, inside the image) on the stack: a poor man's backtrace */
                printf("      stack return candidates (sp=%08x):", c->r[13]);
                for (int i = 0; i < 48; i++) {
                    int lo = g_rd16(c->r[13] + 4 * i), hi = g_rd16(c->r[13] + 4 * i + 2);
                    if (lo < 0 || hi < 0) break;
                    uint32_t w = (uint32_t)lo | ((uint32_t)hi << 16);
                    if ((w & 1) && w > IMG_BASE && w < IMG_TOP) printf(" +%02x:%08x", 4 * i, w);
                }
                printf("\n");
            }
            if (sig == SIGTRAP && e->n > 1000) { printf("STOP: bkpt at %08x hit %llu times (panic/exit loop, lr=%08x)\n", pc, (unsigned long long)e->n, e->y); return ACT_STOP; } }
        skip(c, len); goto done;
    }
    if (sig == SIGSEGV || sig == SIGBUS) {
        if (fault_addr >= IMG_BASE && fault_addr < IMG_TOP && !(pc == fault_addr)) {
            printf("note: fault inside image area at %08x (pc %08x) -- unexpected\n", fault_addr, pc);
        }
        if ((pc & ~1u) < 0x8000 && (pc & ~1u) == (fault_addr & ~1u)) {      /* call into the unmapped bootrom: log it, return 0 */
            int isnew; Ev *e = ev_get(K_ROM, pc & ~1u, 0, &isnew);
            if (e) { e->n++; if (isnew) { e->x = c->r[14]; e->y = c->r[0]; e->z = c->r[1];
                if ((pc & ~1u) == (ROM_LOOKUP & ~1u)) snprintf(e->note, sizeof e->note, "rom_table_lookup code='%c%c' flags=%x", (c->r[0] & 0x7F) >= 32 ? (int)(c->r[0] & 0x7F) : '?', ((c->r[0] >> 8) & 0x7F) >= 32 ? (int)((c->r[0] >> 8) & 0x7F) : '?', c->r[1]);
                else snprintf(e->note, sizeof e->note, "fake rom fn #%u called", ((pc & ~1u) - 0x7E00) / 4);
                logf_("[ROM] call %08x from lr=%08x r0=%08x r1=%08x r2=%08x %s\n", pc & ~1u, c->r[14], c->r[0], c->r[1], c->r[2], e->note); } }
            if ((pc & ~1u) == (ROM_LOOKUP & ~1u)) {      /* hand back a distinct fake entry per code so the call itself is logged later */
                uint32_t code = c->r[0] & 0xFFFF, k;
                for (k = 0; k < g_nrom && g_romcode[k] != code; k++) ;
                if (k == g_nrom && g_nrom < 64) g_romcode[g_nrom++] = code;
                c->r[0] = (0x7E00 + 4 * k) | 1;
            } else c->r[0] = 0;
            c->r[15] = c->r[14] & ~1u;
            if (c->r[14] & 1) c->r[16] |= 0x20; else c->r[16] &= ~0x20u;
            c->r[16] &= ~((0x3Fu << 10) | (3u << 25));
            goto done;
        }
        if (pc == fault_addr) { printf("STOP: instruction fetch fault at %08x\n", pc); return ACT_STOP; }
        int r = cpu_fault_mem(sig, c, fault_addr, h1, h2, len);
        if (r) return r;
        goto done;
    }
    printf("STOP: unexpected signal %d pc=%08x\n", sig, pc);
    return ACT_STOP;
done:
    if (opt.bus) irq_poll(c);
    if (g_events >= g_next_report) { g_next_report = g_events + opt.every; print_summary(0); }
    if (g_events >= opt.max_events) { printf("-- event limit reached (%llu)\n", (unsigned long long)g_events); return ACT_STOP; }
    if (!(g_events & 1023) && now() - g_t0 > opt.secs) { printf("-- time limit reached\n"); return ACT_STOP; }   /* one clock_gettime per 1024 traps (on_alarm also checks every 100 ms) */
    return ACT_CONT;
}

/* ------------------------------------------------------------------ summary */
static int cmp_n(const void *a, const void *b) { const Ev *x = *(Ev *const *)a, *y = *(Ev *const *)b; return x->n < y->n ? 1 : x->n > y->n ? -1 : 0; }


/* UM2_PROF=<hz>: CPU-time sampler (ITIMER_PROF -> SIGPROF), pc histogram per guest core (0, 1) and for helper threads (2).  PC diagnostics only
 * (qemu: share of guest instructions per place; the A9 cost per instruction is not the PC's). */
static __thread int tl_guest; static int g_prof_hz;
static struct { uint32_t pc, n; } g_prof[3][4096]; static uint32_t g_prof_sys[3][512];   /* samples just after a host svc, per syscall number */
static void on_prof(int sig, siginfo_t *si, void *vuc) {
    (void)sig; (void)si; uint32_t pc = ((ucontext_t *)vuc)->uc_mcontext.arm_pc & ~1u; int c = tl_guest ? (tl_core & 1) : 2;
    if ((pc < IMG_BASE || pc >= IMG_TOP) && (pc & 0xFFF) >= 4 && ((((ucontext_t *)vuc)->uc_mcontext.arm_cpsr & 0x20) ? *(const uint16_t *)(uintptr_t)(pc - 2) == 0xDF00 : *(const uint32_t *)(uintptr_t)(pc - 4) == 0xEF000000)) {
        uint32_t nr = ((ucontext_t *)vuc)->uc_mcontext.arm_r7; if (nr < 512) g_prof_sys[c][nr]++; }
    uint32_t k = (pc >> 1) * 2654435761u >> 20;
    for (int i = 0; i < 64; i++) { uint32_t j = (k + (uint32_t)i) & 4095; if (!g_prof[c][j].n) g_prof[c][j].pc = pc; if (g_prof[c][j].pc == pc) { g_prof[c][j].n++; return; } }
}
static void prof_start(void) {
    const char *e = getenv("UM2_PROF"); if (!e) return; g_prof_hz = atoi(e); if (g_prof_hz <= 0) g_prof_hz = 500;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = { {0, 1000000 / g_prof_hz}, {0, 1000000 / g_prof_hz} }; setitimer(ITIMER_PROF, &it, NULL);
}
static int cmp_prof(const void *a, const void *b) { uint32_t x = ((const uint32_t *)a)[1], y = ((const uint32_t *)b)[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void prof_dump(void) {
    if (!g_prof_hz) return;
    static const char *nm[3] = { "core 0", "core 1", "helpers" };
    for (int c = 0; c < 3; c++) {
        uint64_t tot = 0, img = 0, stub = 0, host = 0;
        for (int i = 0; i < 4096; i++) if (g_prof[c][i].n) { uint32_t p = g_prof[c][i].pc; tot += g_prof[c][i].n; if (p >= IMG_BASE && p < IMG_TOP) img += g_prof[c][i].n; else if (p >= STUB_BASE && p < STUB_BASE + STUB_SIZE) stub += g_prof[c][i].n; else host += g_prof[c][i].n; }
        printf("-- profile %s: %llu samples (%d Hz): image %llu, stubs %llu, loader/libc %llu\n", nm[c], (unsigned long long)tot, g_prof_hz, (unsigned long long)img, (unsigned long long)stub, (unsigned long long)host);
        static uint32_t l[4096][2]; int n = 0; for (int i = 0; i < 4096; i++) if (g_prof[c][i].n) { l[n][0] = g_prof[c][i].pc; l[n][1] = g_prof[c][i].n; n++; }
        qsort(l, (size_t)n, sizeof l[0], cmp_prof);
        int lim = getenv("UM2_PROFALL") ? n : 28;      /* UM2_PROFALL=1: every pc, for a per-function sum against the symbol table */
        for (int i = 0; i < n && i < lim; i++) printf("   %6u  %08x\n", l[i][1], l[i][0]);
        printf("-- syscalls %s (number:samples):", nm[c]); for (int i = 0; i < 512; i++) if (g_prof_sys[c][i]) printf(" %d:%u", i, g_prof_sys[c][i]); putchar('\n');
    }
}
/* UM2_PROFSLICE=1 (with UM2_PROF): at every heartbeat, the frame marks and the busiest pcs per guest core since the last one, to tell a slow stretch from a normal one.
 * UM2_PROFSLICE=<ms> (50 or more, 4 Nov 2026): every <ms> from a thread of its own instead, with the time and the interrupts delivered, to catch dips shorter than a heartbeat. */
static int g_slice_ms;
static void prof_slice(void) {
    static int on = -1; static uint32_t prev[3][4096]; static unsigned m0; static unsigned long q0;
    if (on < 0) on = g_prof_hz && getenv("UM2_PROFSLICE") != NULL;
    if (!on) return;
    unsigned long q = 0; for (int i = 0; i < NIRQ; i++) q += g_irq_cnt[i];
    fprintf(stderr, "prof slice: t=%.2f frame marks %u irqs %lu", now() - g_t0, g_marks - m0, q - q0); m0 = g_marks; q0 = q;
    for (int c = 0; c < 2; c++) {
        uint32_t top[8][2] = {{0}}, tot = 0;
        for (int i = 0; i < 4096; i++) {
            uint32_t d = g_prof[c][i].n - prev[c][i]; prev[c][i] = g_prof[c][i].n; if (!d) continue; tot += d;
            for (int k = 0; k < 8; k++) if (d > top[k][1]) { memmove(top[k + 1], top[k], (size_t)(7 - k) * sizeof top[0]); top[k][0] = g_prof[c][i].pc; top[k][1] = d; break; }
        }
        fprintf(stderr, " | core %d %u:", c, tot); for (int k = 0; k < 8 && top[k][1]; k++) fprintf(stderr, " %08x:%u", top[k][0], top[k][1]);
    }
    fputc('\n', stderr);
}
static void *slice_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "u2slice");
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    for (;;) {                                         /* absolute deadlines: the slices keep their length however long the print takes */
        ts.tv_nsec += (long)g_slice_ms * 1000000L; while (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL); prof_slice();
    }
    return NULL;
}
static void slice_start(void) {
    const char *e = getenv("UM2_PROFSLICE"); if (!g_prof_hz || !e || atoi(e) < 50) return;
    g_slice_ms = atoi(e); pthread_t th; if (pthread_create(&th, NULL, slice_thread, NULL)) g_slice_ms = 0; else pthread_detach(th);
}
static void print_summary(int final) {
    uint64_t tot[K_NKINDS] = {0};
    for (int i = 0; i < EVCAP; i++) if (g_ev[i].kind && g_ev[i].kind != K_OTHER) tot[g_ev[i].kind] += g_ev[i].n;
    printf("== %s t=%.1fs events=%llu | ILL %llu/%llu SEGV %llu/%llu BUS %llu/%llu TRAP %llu/%llu SVC %llu (distinct sites %llu, nums %llu) | MMIO words %llu | pc-samples %llu\n",
           final ? "FINAL" : "progress", now() - g_t0, (unsigned long long)g_events,
           (unsigned long long)tot[K_ILL], (unsigned long long)g_kdistinct[K_ILL], (unsigned long long)tot[K_SEGV], (unsigned long long)g_kdistinct[K_SEGV],
           (unsigned long long)tot[K_BUS], (unsigned long long)g_kdistinct[K_BUS], (unsigned long long)tot[K_TRAP], (unsigned long long)g_kdistinct[K_TRAP],
           (unsigned long long)tot[K_SVCN], (unsigned long long)g_kdistinct[K_SVC], (unsigned long long)g_kdistinct[K_SVCN],
           (unsigned long long)g_kdistinct[K_ADDR], (unsigned long long)tot[K_PC]);
    if (!final) { fflush(stdout); return; }
    prof_dump(); isr_dump();
    printf("-- timer-poll naps (60 us each): core 0 %lu, core 1 %lu;  frame marks: %u total, %u in the census window\n", g_naps[0], g_naps[1], g_marks, g_marks - g_cen_m0);
    printf("-- SIGURG attempts that did not deliver: pc in helper/loader code %lu, PRIMASK set %lu, other %lu; delivered inside a stub: %lu; irq_enter refused: masked/deep %lu, IT block %lu, nothing pending %lu, burst gap %lu; bursts cut %lu\n", g_irqf_pc, g_irqf_prim, g_irqf_other, g_irqs_stub, g_ie_why[0], g_ie_why[1], g_ie_why[2], g_ie_why[3], g_irq_bursts);
    if (g_irq_on) printf("-- alarm path: ALARM stores fired inline %lu, armed for the future %lu, irq thread signalled %lu, woke %lu, fired %lu (fast path %s)\n", g_al_inline, g_al_future, g_al_signal, g_irq_wakes, g_irq_fires, g_alarm_fast ? "on" : "off");
    if (g_irq_on) { printf("-- interrupts delivered (NVIC IRQ:count):"); for (int i = 0; i < NIRQ; i++) if (g_irq_cnt[i]) printf(" %d:%lu", i, g_irq_cnt[i]); printf("   signal kicks %lu; per core %lu/%lu, from SIGURG %lu/%lu, at end of a trap %lu, tail-chained %lu\n", g_irq_kicks, g_irq_pc[0], g_irq_pc[1], g_irq_async[0], g_irq_async[1], g_irq_trap, g_irq_chain); }
    static Ev *l[EVCAP];
    for (int kind = 1; kind < K_NKINDS; kind++) {
        if (kind == K_OTHER || kind == K_ADDR) continue;
        int n = 0; for (int i = 0; i < EVCAP; i++) if (g_ev[i].kind == kind) l[n++] = &g_ev[i];
        if (!n) continue;
        qsort(l, n, sizeof *l, cmp_n);
        printf("-- %s: %d distinct (top %d)\n", kname[kind], n, n < opt.top ? n : opt.top);
        for (int i = 0; i < n && i < opt.top; i++) {
            Ev *e = l[i];
            switch (kind) {
            case K_ILL: printf("   %8llu  pc=%08x  %04x %04x  %s\n", (unsigned long long)e->n, e->a, e->x, e->y, e->note); break;
            case K_TRAP: printf("   %8llu  pc=%08x  %04x  first lr=%08x r0=%08x  %s\n", (unsigned long long)e->n, e->a, e->x, e->y, e->z, e->note); break;
            case K_SEGV: case K_BUS: printf("   %8llu  pc=%08x  %s%d addr=%08x..%08x  %s\n", (unsigned long long)e->n, e->a, (e->z & 15) == 1 ? "ld" : "st", (e->z >> 4) * 8, e->x, e->y, e->note); break;
            case K_SVC: printf("   %8llu  pc=%08x  svc #%u (first r0=%08x r1=%08x)\n", (unsigned long long)e->n, e->a, e->x, e->y, e->z); break;
            case K_ROM: printf("   %8llu  rom %08x from lr=%08x r0=%08x r1=%08x  %s\n", (unsigned long long)e->n, e->a, e->x, e->y, e->z, e->note); break;
            case K_SVCN: printf("   %8llu  svc #%u\n", (unsigned long long)e->n, e->a); break;
            case K_PC: printf("   %8llu  pc~%08x\n", (unsigned long long)e->n, e->a); break;
            }
        }
    }
    /* MMIO address census, grouped by block */
    struct { const char *n; uint64_t r, w; int words; } g[96]; int ng = 0;
    int na = 0; for (int i = 0; i < EVCAP; i++) if (g_ev[i].kind == K_ADDR) l[na++] = &g_ev[i];
    for (int i = 0; i < na; i++) {
        const char *bn = blockname(l[i]->a); int j;
        for (j = 0; j < ng; j++) if (g[j].n == bn) break;
        if (j == ng && ng < 96) { g[ng].n = bn; g[ng].r = g[ng].w = 0; g[ng].words = 0; ng++; }
        if (j < ng) { g[j].r += l[i]->x; g[j].w += l[i]->y; g[j].words++; }
    }
    printf("-- MMIO by block (reads/writes/distinct words):\n");
    for (int j = 0; j < ng; j++) printf("   %-24s r=%-8llu w=%-8llu words=%d\n", g[j].n, (unsigned long long)g[j].r, (unsigned long long)g[j].w, g[j].words);
    qsort(l, na, sizeof *l, cmp_n);
    printf("-- hottest MMIO words:\n");
    for (int i = 0; i < na && i < opt.top; i++)
        printf("   %8llu  %08x  %-18s r=%llu w=%llu last=%08x\n", (unsigned long long)l[i]->n, l[i]->a, blockname(l[i]->a), (unsigned long long)l[i]->x, (unsigned long long)l[i]->y, l[i]->z);
    printf("-- last traps (oldest first):");
    { uint64_t cnt = g_events < 32 ? g_events : 32; for (uint64_t k = g_events - cnt + 1; k <= g_events; k++) { uint32_t *r = g_ring[k & 31]; printf(" [%d@%08x a=%08x]", (int)r[0], r[1], r[2]); } }
    putchar(10);
    if (g_haveregs) { printf("-- last sampled regs:"); for (int i = 0; i < 16; i++) printf(" %s=%08x", regn(i), g_lastregs[i]); printf(" cpsr=%08x", g_lastregs[16]); putchar(10); }
    if (!opt.bus) { lb_dump(stdout); um_dump(stdout); }
    fflush(stdout);
}
/* end of run: every loop-breaker firing (same pc re-reading one unmodelled register > 16 times got fake values) */
static void lb_dump(FILE *f) {
    static Ev *l[256]; int n = 0;
    for (int i = 0; i < EVCAP && n < 256; i++) if (g_ev[i].kind == K_OTHER && (g_ev[i].z & 1)) l[n++] = &g_ev[i];
    if (!n) return;
    qsort(l, n, sizeof *l, cmp_n);
    fprintf(f, "um2run: loop breaker fired at %d site%s (pc re-reading one unmodelled register; fake values handed back):\n", n, n == 1 ? "" : "s");
    for (int i = 0; i < n && i < 40; i++) {
        Ev *e = l[i]; char fk[160]; int o = 0;
        if (e->x == 0x7FFF) snprintf(fk, sizeof fk, "all 15");
        else for (int k = 0; k < 15 && o < (int)sizeof fk - 12; k++) if (e->x >> k & 1) o += snprintf(fk + o, sizeof fk - o, "%s%x", o ? "," : "", g_lb_cyc[k]);
        fprintf(f, "  %08x %-12s pc=%08x reads=%llu real=%08x fakes=%s\n", e->b, blockname(e->b), e->a, (unsigned long long)e->n, e->y, fk);
    }
    if (n > 40) fprintf(f, "  ... %d more\n", n - 40);
}
/* end of run: unmodelled MMIO by block.  Blocks whose leftover registers only need to read back what was written (pad/pin/clock setup, the modelled blocks' other registers) go on one line. */
static void um_dump(FILE *f) {
    static const char *cfg[] = { "IO_BANK0", "IO_QSPI", "PADS_BANK0", "PADS_QSPI", "CLOCKS", "RESETS", "PLL_SYS", "PLL_USB", "XOSC", "ROSC", "SYSCFG", "SYSINFO", "BUSCTRL", "ACCESSCTRL",
        "TICKS", "WATCHDOG", "XIP_CTRL", "QMI", "BOOTRAM", "PPB(SCS/NVIC/SysTick)", "PPB(ITM..)", "PPB", "SIO", "DMA", "PIO0", "PIO1", "PIO2", "SPI0" };
    pthread_mutex_lock(&g_um_m);
    int nu = 0; char line[1024]; int o = 0;
    for (int j = 0; j < g_num; j++) {
        int c = 0; for (unsigned k = 0; k < sizeof cfg / sizeof *cfg; k++) if (!strcmp(g_um[j].blk, cfg[k])) c = 1;
        if (c) { if (o < (int)sizeof line - 40) o += snprintf(line + o, sizeof line - (size_t)o, " %s %u", g_um[j].blk, g_um[j].words); continue; }
        if (!nu++) fprintf(f, "um2run: MMIO um2run does not model (once per word; reads give what was written, else 0):\n");
        fprintf(f, "  %-12s %3u words (%u first reads, %u first writes), first %08x", g_um[j].blk, g_um[j].words, g_um[j].r, g_um[j].w, g_um[j].first);
        if (g_um[j].pc) fprintf(f, " read at pc %08x", g_um[j].pc);
        fputc('\n', f);
    }
    if (o) fprintf(f, "um2run: shadow-only registers (config, read back as written), words per block:%s\n", line);
    pthread_mutex_unlock(&g_um_m);
}

static void heartbeat(pid_t child) {
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    fprintf(stderr, "hb t=%.1f ev=%llu lastpc=%08x self cpu user=%.2f sys=%.2f ctxsw vol=%ld invol=%ld",
            now() - g_t0, (unsigned long long)g_events, g_lastpc,
            ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6, ru.ru_nvcsw, ru.ru_nivcsw);
    if (child > 0) {
        char fn[48], buf[512]; snprintf(fn, sizeof fn, "/proc/%d/stat", (int)child);
        FILE *f = fopen(fn, "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
            char *rp = strrchr(buf, ')'); unsigned long ut = 0, st = 0; char state = '?';
            if (rp && sscanf(rp + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) >= 1)
                fprintf(stderr, " | child state=%c user=%.2f sys=%.2f", state, (double)ut / sysconf(_SC_CLK_TCK), (double)st / sysconf(_SC_CLK_TCK));
        }
    }
    fputc('\n', stderr);
    um2_clock_sync();
    if (!g_slice_ms) prof_slice();
}

static void setup_sched(void) {
    if (!g_opt_nonice) setpriority(PRIO_PROCESS, 0, -20);
    if (g_opt_cpu >= 0) { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_opt_cpu, &cs); if (sched_setaffinity(0, sizeof cs, &cs)) perror("sched_setaffinity"); }
    if (g_opt_rt > 0) { struct sched_param sp; sp.sched_priority = g_opt_rt; if (sched_setscheduler(0, SCHED_RR, &sp)) perror("sched_setscheduler(SCHED_RR)"); }
    errno = 0; int nice = getpriority(PRIO_PROCESS, 0);
    fprintf(stderr, "sched: policy=%d nice=%d rt=%d cpu=%d\n", sched_getscheduler(0), nice, g_opt_rt, g_opt_cpu);
}

static void sample_pc(uint32_t pc) {
    Ev *e = ev_get(K_PC, pc & ~0x1u, 0, NULL); if (e) { e->n++; g_kcount[K_PC]++; }
}

/* ------------------------------------------------------------------ guest setup (child / inproc) */
static void guest_load_and_map(void) {
    size_t map_len = IMG_TOP - IMG_BASE;
    void *m = mmap((void *)IMG_BASE, map_len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != (void *)IMG_BASE) { perror("mmap image (check /proc/sys/vm/mmap_min_addr, RWX policy)"); _exit(2); }
    if (!opt.no_psram) {
        /* PSRAM: the XIP window 0x11000000 and its uncached alias 0x15000000 (images stage romsets through the alias) show the same memory */
        void *p = MAP_FAILED; int fd = (int)syscall(SYS_memfd_create, "um2psram", 0);
        if (fd >= 0 && ftruncate(fd, PSRAM_SIZE) == 0) {
            p = mmap((void *)PSRAM_BASE, PSRAM_SIZE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
            if (p == (void *)PSRAM_BASE) mmap((void *)(PSRAM_BASE + 0x4000000u), PSRAM_SIZE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
        }
        if (p != (void *)PSRAM_BASE) p = mmap((void *)PSRAM_BASE, PSRAM_SIZE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != (void *)PSRAM_BASE) { perror("mmap psram (continuing without)"); }
    }
    /* XIP cache maintenance window (RP2350 0x18000000: CS0 flash, +16 MB CS1 PSRAM; the low 3 address bits pick the operation).
       We model no XIP cache, so a maintenance write has nothing to do; unmapped, each one trapped (Nigel Harrison's Dragon's Lair:
       a byte store at 0x19000002 + 8n per cache line, 980 traps a frame, 99% of all, 8 Nov 2026). Map it as a sink: 32 views of
       one 1 MB scratch file, so a long sweep costs 1 MB at most. Not fixed: if anything already lives there, the traps stay. */
    if (!getenv("UM2_NOXIPMAINT")) {
        int fd = (int)syscall(SYS_memfd_create, "um2xipmaint", 0); unsigned ok = 0;
        if (fd >= 0 && ftruncate(fd, 1u << 20) == 0)
            for (uint32_t a = 0x18000000u; a < 0x1A000000u; a += 1u << 20) {
                void *q = mmap((void *)(uintptr_t)a, 1u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (q == (void *)(uintptr_t)a) ok++; else if (q != MAP_FAILED) munmap(q, 1u << 20);
            }
        if (fd >= 0) close(fd);
        if (ok != 32) fprintf(stderr, "um2run: XIP maintenance window: %u of 32 MB mapped (the rest trap as before)\n", ok);
    }
    uint32_t words; memcpy(&words, g_file + 16, 4);
    memcpy((void *)IMG_BASE, g_file + 20, (size_t)words * 4);
    if (opt.bus && !getenv("UM2_NODIVPATCH")) stub_setup();
    if (opt.bus) romcopy_setup();
    if (opt.patch_svc) {
        uint16_t *h = (uint16_t *)IMG_BASE; size_t n = (size_t)words * 2, cnt = 0;
        static uint16_t o[(IMG_TOP - IMG_BASE) / 2]; memcpy(o, h, n * 2);   /* original halfwords: the cluster test must not see our own patches */
        for (size_t i = 0; i < n; i++) if ((h[i] & 0xFF00) == 0xDF00) {
            if (opt.bus) {      /* real dispatch: a false hit in literal data would corrupt it, so only the wrapper cluster (svc #n<=31 in groups of >=3 within 64 bytes) */
                if ((h[i] & 0xFF) > 31) continue;
                int near = 0; size_t lo = i > 32 ? i - 32 : 0;
                for (size_t j = lo; j < n && j <= i + 32; j++) if (j != i && (o[j] & 0xFF00) == 0xDF00 && (o[j] & 0xFF) <= 31) near++;
                if (near < 2) continue;
            }
            g_patched[i >> 3] |= 1 << (i & 7); g_patch_orig_n[i] = h[i] & 0xFF; h[i] = 0xDE00 | (h[i] & 0xFF); cnt++;
        }
        /* A wrapper a little apart from the cluster: "svc #n; bx lr" within 64 bytes of a patched site (9 Nov 2026: Vector Pinball's
         * svc #5, 54 bytes past its neighbour, ran as a real Linux system call with the guest's r7 as the number, and the kernel's
         * SIGILL for the bad number landed on the bx lr after it, which was then skipped). */
        if (opt.bus) for (size_t i = 0; i + 1 < n; i++) if ((o[i] & 0xFF00) == 0xDF00 && (o[i] & 0xFF) <= 31 && o[i + 1] == 0x4770 && !(g_patched[i >> 3] >> (i & 7) & 1)) {
            int near = 0; size_t lo = i > 32 ? i - 32 : 0;
            for (size_t j = lo; j < n && j <= i + 32; j++) if (j != i && (g_patched[j >> 3] >> (j & 7) & 1)) near++;
            if (!near) continue;
            g_patched[i >> 3] |= 1 << (i & 7); g_patch_orig_n[i] = o[i] & 0xFF; h[i] = 0xDE00 | (o[i] & 0xFF); cnt++;
            fprintf(stderr, "svc #%u at %08x: a wrapper apart from its cluster, patched too\n", o[i] & 0xFF, IMG_BASE + (unsigned)i * 2);
        }
        fprintf(stderr, "patched %zu svc halfwords to udf\n", cnt);
    }
    __builtin___clear_cache((char *)IMG_BASE, (char *)IMG_TOP);
}

#ifdef __arm__
__attribute__((noreturn)) static void guest_jump(void) {
    uint32_t sp = IMG_TOP, pc = g_entry | 1;
    /* Switch to the guest stack, then munmap 0x40000000..0x40801000: qemu-user parks the process stack there, which would
     * make RP2350 peripheral addresses look mapped (on a real Linux/ARM kernel that range is normally empty: a no-op). */
    __asm__ volatile("mov sp, %0\n\t"
                     "mov r4, %1\n\t"
                     "mov r0, #0x40000000\n\t"
                     "mov r1, #0x800000\n\t"
                     "add r1, r1, #0x1000\n\t"
                     "mov r7, #91\n\t"
                     "svc #0\n\t"
                     "bx r4" :: "r"(sp), "r"(pc) : "r0", "r1", "r4", "r7", "memory");
    __builtin_unreachable();
}

static void wr16_inproc(uint32_t a, uint16_t v) { *(volatile uint16_t *)(uintptr_t)a = v; __builtin___clear_cache((char *)(uintptr_t)a, (char *)(uintptr_t)a + 2); }
static int rd16_inproc(uint32_t a) { if (a < IMG_BASE || a + 2 > IMG_TOP || (a & 1)) return -1; return *(volatile uint16_t *)(uintptr_t)a; }

/* An exit is claimed once, or every thread that gets there while it runs starts its own bus_finish (VecOS, 1 Nov 2026: six at once from the timer, two producers
 * on the ring, the process alive 2 h after its limit).  The losers from the timer return, and the ticks that follow are the deadline; the others stop here. */
static volatile int g_exit_claimed, g_exit_rc; static volatile double g_exit_t;
static int exit_claim(int rc) { if (__sync_lock_test_and_set(&g_exit_claimed, 1)) return 0; g_exit_rc = rc; g_exit_t = now(); return 1; }

/* ---- in-process back-end */
static void on_fault(int sig, siginfo_t *si, void *vuc) {
    ucontext_t *uc = vuc; Cpu c;
    tl_uc = uc;
    memcpy(c.r, &uc->uc_mcontext.arm_r0, sizeof c.r);          /* r0..r10, fp, ip, sp, lr, pc, cpsr are contiguous */
    int act;
    if (opt.bus_verbose > 1) {      /* in-handler profile (two clock_gettime per trap: only for the verbose=2 frame-mark line) */
        struct timespec t0_, t1_; clock_gettime(CLOCK_MONOTONIC, &t0_);
        act = cpu_trap(sig, &c, (uint32_t)(uintptr_t)si->si_addr, 0, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1_); g_trap_ns += (uint64_t)((t1_.tv_sec - t0_.tv_sec) * 1000000000LL + (t1_.tv_nsec - t0_.tv_nsec)); g_trap_n++;
    } else act = cpu_trap(sig, &c, (uint32_t)(uintptr_t)si->si_addr, 0, 0);
    if (act == ACT_STOP) { if (!exit_claim(0)) for (;;) pause(); print_summary(1); fflush(stdout); bus_finish(); _exit(0); }
    memcpy(&uc->uc_mcontext.arm_r0, c.r, sizeof c.r);
}
#ifdef USE_SECCOMP
static void on_sys(int sig, siginfo_t *si, void *vuc) {
    ucontext_t *uc = vuc; Cpu c;
    memcpy(c.r, &uc->uc_mcontext.arm_r0, sizeof c.r);
    c.r[15] = (uint32_t)(uintptr_t)si->si_call_addr;
    int hw = rd16_inproc(c.r[15] - 2);
    int act = cpu_trap(SIGSYS, &c, 0, 1, hw < 0 ? 0 : hw);
    if (act == ACT_STOP) { print_summary(1); fflush(stdout); _exit(0); }
    uc->uc_mcontext.arm_r0 = c.r[0];
}
static void install_seccomp(void) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, IMG_BASE, 0, 3),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, IMG_TOP, 2, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog p = { sizeof f / sizeof *f, f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p)) { perror("seccomp"); exit(2); }
}
#endif
static volatile int g_ptx_up, g_guest_seen;   /* g_guest_seen: the image itself has produced a bus event (ptx may be up earlier: loading indicator) */
static const char *g_sd_path;
static double g_last_frame_t, g_bus_timeout = 60, g_frame_timeout = 30;    /* --bus: exit if the bus stays unused / no frame for this long */
static void on_alarm(int sig, siginfo_t *si, void *vuc) {
    ucontext_t *uc = vuc; (void)sig; (void)si;
    if (g_exit_claimed) { if (g_exit_t > 0 && now() - g_exit_t > 3) _exit(g_exit_rc); return; }
    memcpy(g_lastregs, &uc->uc_mcontext.arm_r0, sizeof g_lastregs); g_haveregs = 1;
    sample_pc(uc->uc_mcontext.arm_pc);
    if (++g_ticks % 20 == 0) { heartbeat(0); fflush(stdout); }
    { static int np = -1, done[8]; static uint32_t pa[8], pv[8]; static double pt[8];          /* UM2_POKE=addr:value:seconds[,..] (hex addr/value): PC test hook, stores a guest word once that many seconds after the start (e.g. a setting the menu would change) */
      if (np < 0) { np = 0; const char *e = getenv("UM2_POKE"); while (e && *e && np < 8) { char *q; pa[np] = (uint32_t)strtoul(e, &q, 16); if (*q == ':') q++; pv[np] = (uint32_t)strtoul(q, &q, 16); if (*q == ':') q++; pt[np] = strtod(q, &q); np++; if (*q == ',') q++; e = q; } }
      for (int i = 0; i < np; i++) if (!done[i] && now() - g_t0 >= pt[i] && pa[i] >= IMG_BASE && pa[i] < IMG_TOP - 4) { done[i] = 1; *(volatile uint32_t *)(uintptr_t)pa[i] = pv[i]; fprintf(stderr, "um2run: poked %08x = %08x\n", pa[i], pv[i]); } }
    if (now() - g_t0 > opt.secs) { if (!exit_claim(0)) return; printf("-- time limit reached\n"); print_summary(1); fflush(stdout); bus_finish(); _exit(0); }
    if (opt.bus && g_bus_timeout > 0 && !g_guest_seen && now() - g_t0 > g_bus_timeout) {
        fprintf(stderr, "um2run: no bus event within %.0f s, exiting (--bus-timeout)\n", g_bus_timeout); fflush(stdout); _exit(4); }
    if (ptx_core_stalled) g_last_frame_t = now();     /* the core is not fetching (paused?): nothing the image did, so wait for it, however long */
    if (opt.bus && g_frame_timeout > 0 && g_ptx_up && g_last_frame_t && now() - g_last_frame_t > g_frame_timeout) {
        if (!exit_claim(5)) return;
        fprintf(stderr, "um2run: no frame for %.0f s, exiting\n", g_frame_timeout);
        {   /* where it hung (8 Nov 2026, a rare M.A.C.H. 3 start-up hang): the thread the tick interrupted, and the other guest core, parked for its pc */
            const mcontext_t *m = &uc->uc_mcontext; uint32_t opc = quiesce_other(0, 0) ? g_q_pc : 0;
            fprintf(stderr, "um2run: hang: tid %ld (core %d) pc %08lx lr %08lx sp %08lx r0-r3 %08lx %08lx %08lx %08lx; other core pc %08x\n",
                    (long)syscall(SYS_gettid), tl_core, m->arm_pc, m->arm_lr, m->arm_sp, m->arm_r0, m->arm_r1, m->arm_r2, m->arm_r3, opc); }
        bus_finish(); _exit(5); }
}

/* ---- --bus glue: ptx sink, guest memory, core-1 thread */
/* Single-CPU scheduling (the DE10's Linux sees one CPU).  The guest threads spin on each other natively (core 0 waits for core 1's
 * frame_done, core 1 for the DMA BUSY flag that the helper clears ...); at the same RT priority every hand-over costs a whole
 * SCHED_RR slice (100 ms).  A priority gap does not work either (the higher thread's idle input reads then starve core 0 completely).
 * So both guest threads stay at the same priority and a fairness thread (two levels above, with the other helpers) sends each of them
 * SIGUSR1 every millisecond: the handler does sched_yield() (hand the CPU to the other guest thread, ending a native spin), and every
 * 8th tick sleeps 1 ms so that tasks below them, MiSTer Main included, always get >10 percent. */
void um2_rt_boost(int delta) {
    struct sched_param sp; int pol = sched_getscheduler(0);
    if ((pol == SCHED_RR || pol == SCHED_FIFO) && !sched_getparam(0, &sp) && sp.sched_priority + delta < 99) {
        sp.sched_priority += delta; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }
}
static double g_settle = 0, g_cpu1_after = 0;   /* --settle S: wait S s before touching the ring; --cpu1-after S: core 1 starts on the process CPU, moves to --cpu1 after S s */
static volatile pid_t g_c1_tid;
static unsigned g_donate_us = 1000, g_donate_period_us = 2000;
static volatile int g_boosted;
/* SIGURG attempts that did not deliver: pc in helper/loader code, PRIMASK set, other (IT block, depth); deliveries inside a stub */
static void on_irq(int sig, siginfo_t *si, void *vuc) {      /* SIGURG: asynchronous interrupt delivery */
    ucontext_t *uc = vuc; (void)sig; (void)si;
    static int nostub = -1; if (nostub < 0) nostub = getenv("UM2_NOIRQSTUB") != NULL;
    uint32_t pc = uc->uc_mcontext.arm_pc;
    /* Thumb code of the image, or of a patch stub (a stub is plain Thumb that saves what it clobbers: an exception frame below its pushes and a return to this very instruction are as good as in the image; 28 Oct 2026 -- it used to be image only,
     * and a tick that hit a stub waited for the irq thread's retry 150 us later).  The thunks / helpers are ARM or host code and still refuse. */
    int in_stub = pc >= STUB_BASE && pc < STUB_BASE + STUB_SIZE && !nostub;
    if (!((pc >= IMG_BASE && pc < IMG_TOP) || in_stub) || !(uc->uc_mcontext.arm_cpsr & 0x20)) { g_irqf_pc++; irq_wake_retry(); return; }       /* helper / ARM state: the irq thread retries */
    if (g_nv[tl_core & 1].prim) { g_irqf_prim++; return; }
    Cpu c; memcpy(c.r, &uc->uc_mcontext.arm_r0, sizeof c.r);
    tl_uc = uc;
    if (irq_enter(&c, uc)) { memcpy(&uc->uc_mcontext.arm_r0, c.r, sizeof c.r); g_irq_async[tl_core & 1]++; if (in_stub) g_irqs_stub++; } else g_irqf_other++;
}
static void on_donate(int sig, siginfo_t *si, void *vuc) {
    (void)sig; (void)si; (void)vuc;
    static __thread unsigned n;
    /* core 0 has CPU 0 to itself once core 1 sits on its own CPU (--cpu1): only core 1 shares its CPU with Main.  BEFORE the move (--cpu1-after)
     * both guests share CPU 0 and core 0 must donate too: on 27 Oct 2026 a natively spinning core 0 that never yielded starved core 1 (0 ms CPU) until the move. */
    if (g_cpu1 >= 0 && g_c1_own_cpu && !tl_core) return;
    if (++n % 8 == 0) { struct timespec ts = { 0, (long)g_donate_us * 1000L }; nanosleep(&ts, NULL); }   /* a share for tasks below us (Main) */
    else sched_yield();                                                                              /* the other guest thread, if runnable */
}
static void *fair_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "u2fair"); um2_rt_boost(2);
    for (;;) {
        struct timespec ts = { 0, (long)g_donate_period_us * 1000L }; nanosleep(&ts, NULL);
        static unsigned tk; tk++;
        (void)tk;
        if (!(g_cpu1 >= 0 && g_c1_own_cpu)) pthread_kill(g_th_core0, SIGUSR1);   /* on_donate would return at once */
        if (__atomic_load_n(&g_have_core1, __ATOMIC_ACQUIRE)) pthread_kill(g_th_core1, SIGUSR1);
    }
    return NULL;
}
/* --ringmon: once per second, from the heartbeat thread, print the core's view of the ring (read-only map of the DDR block):
 * commands and E cycles we are ahead of the core, its underrun counter and frame counter, and the longest wall-clock gap between two
 * sink calls (ring writes) in that second.  Min/max "ahead" are sampled every 10 ms. */
static volatile uint32_t *g_mon; static int g_ringmon;
static volatile double g_maxgap, g_lastw;
static void mon_open(void) {
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("ringmon: /dev/mem"); return; }
    void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x34000000u);
    if (m == MAP_FAILED) { perror("ringmon: mmap"); return; }
    g_mon = (volatile uint32_t *)m;
}
static double thr_cpu(pthread_t th) { clockid_t cid; struct timespec ts; if (pthread_getcpuclockid(th, &cid) || clock_gettime(cid, &ts)) return 0; return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void mon_sample(int final) {
    static double t_last; static uint64_t min_cmd = ~0ull, max_cmd, min_cyc = ~0ull, max_cyc; static uint32_t u_prev, f_prev; static int have; static uint64_t cc_prev;
    if (!g_mon) return;
    uint64_t rd = (uint64_t)g_mon[0x40 / 4] | ((uint64_t)g_mon[0x44 / 4] << 32);
    uint64_t wr = (uint64_t)g_mon[0x10 / 4] | ((uint64_t)g_mon[0x14 / 4] << 32);
    uint64_t cc = (uint64_t)g_mon[0x48 / 4] | ((uint64_t)g_mon[0x4C / 4] << 32);
    uint32_t fr = g_mon[0x50 / 4], un = g_mon[0x54 / 4];
    uint64_t ahead = wr - rd, lead = ptx_now_cycles() - cc;
    if (rd && wr >= rd) { if (ahead < min_cmd) min_cmd = ahead; if (ahead > max_cmd) max_cmd = ahead; if ((int64_t)lead < (int64_t)min_cyc) min_cyc = lead; if (lead > max_cyc) max_cyc = lead; }
    double t = now();
    if (!have) { have = 1; t_last = t; u_prev = un; f_prev = fr; cc_prev = cc; return; }
    if (t - t_last >= 1.0 || final) {
        fprintf(stderr, "ringmon t=%.0f ahead cmds %llu..%llu, cycles %lld..%lld (%.1f..%.1f ms), underruns %u (+%u), core frames +%u, played %.1f%% of real time, maxgap %.2f ms%c",
                t - g_t0, (unsigned long long)min_cmd, (unsigned long long)max_cmd, (long long)min_cyc, (long long)max_cyc, (double)(int64_t)min_cyc / 1500.0, (double)max_cyc / 1500.0,
                un, un - u_prev, fr - f_prev, (double)(cc - cc_prev) / (t - t_last) / 15000.0, g_maxgap * 1e3, 10);
        { static double c0p, c1p; double c0 = thr_cpu(g_th_core0), c1 = g_have_core1 ? thr_cpu(g_th_core1) : 0;
          static unsigned long i0p, i10p; unsigned long i0 = g_irq_cnt[0], i10 = g_irq_cnt[10];
          fprintf(stderr, "  cpu per second: core0 %.0f ms, core1 %.0f ms (of %.0f ms wall); irq0 +%lu irq10 +%lu, alarm lag %d ms\n", (c0 - c0p) * 1e3, (c1 - c1p) * 1e3, (t - t_last) * 1e3, i0 - i0p, i10 - i10p, g_irq_cnt[0] ? -(int32_t)(g_tm[0].target[0] - micros()) / 1000 : 0); c0p = c0; c1p = c1; i0p = i0; i10p = i10; }
        t_last = t; min_cmd = ~0ull; max_cmd = 0; min_cyc = ~0ull; max_cyc = 0; u_prev = un; f_prev = fr; cc_prev = cc; g_maxgap = 0;
    }
}
static void *hb_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "u2hb");
    um2_rt_boost(2);
    if (g_ringmon) mon_open();
    while (g_ptx_up) { struct timespec ts = { 0, 10 * 1000000L }; nanosleep(&ts, NULL); if (g_ptx_up) { ptx_poll(); if (g_ringmon) mon_sample(0); } }
    return NULL;
}
/* The ptx backend (MiSTer: the takeover handshake) starts at the first bus event, not at launch: the image boots for
 * seconds on this CPU without producing a bus cycle, and the core drops a takeover whose heartbeat is silent for 100 ms.
 * From then on a helper thread keeps the heartbeat moving while the guest is busy between stream operations. */
static pthread_mutex_t g_up_m = PTHREAD_MUTEX_INITIALIZER;   /* the guest and the loading indicator can both be first */
static void ptx_up(void) {
    if (g_ptx_up) return;
    pthread_mutex_lock(&g_up_m);
    if (!g_ptx_up) {
        if (opt.bus_verbose) fprintf(stderr, "ptx_up: init\n");
        if (ptx_init()) { fprintf(stderr, "ptx_init failed\n"); exit(1); }
        g_ptx_up = 1;
        if (opt.bus_verbose) fprintf(stderr, "ptx_up: thread\n");
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 16);
        if (!pthread_create(&th, &at, hb_thread, NULL)) pthread_detach(th);
    }
    pthread_mutex_unlock(&g_up_m);
}
/* the image's own first bus event arms the no-frame timer (as ptx_up did before the indicator could bring ptx up first) */
static void gup(void) { if (!g_guest_seen) { g_guest_seen = 1; g_last_frame_t = now(); } ptx_up(); }
static int g_hold; static void hq_flush(void);
static volatile int g_ld_done = 1; static int g_noload; static uint64_t g_drop, g_ld_cyc;   /* loading indicator, see below; g_ld_done = 1: not running */
/* ---- loading indicator (--bus, off with --no-loading).  Until the image has finished loading a helper thread publishes frames of its own at ~50 Hz: LOADING
 * plus a progress bar fed by um2sd's sector count (the total at the hand-over is kept in <image>.load; without it a block sweeps to and fro).  It starts after a
 * short grace (fast images never show it) and, since um2run sleeps --settle before anything runs, never touches the ring during settle.
 * --hold: the image may draw while it loads (Out Run: small text of its own, 9 frames/s).  Those frames still go to the ring, so one-off VIA and PSG set-up
 * survives, but dark and without frame marks; the hand-over waits for the card to calm down, see ld_ready.  An image without a card hands over at its first
 * frame.  Without --hold the image's first ring access ends the indicator.
 * Sync: g_ld_m is held while the thread builds and publishes one frame; ld_stop() (guest side, before its first ring access) takes it once, so an in-flight
 * frame completes, and sets g_ld_done for good.  From then on the thread never takes the lock again and the guest needs none: ptx_* is only ever called by one
 * side at a time and the ring holds whole indicator frames, then guest frames.  A thread, not the trap path: ptx_frame() paces on the core (20 ms per frame),
 * which inside a trap would stall the guest's own SD load.  Idle-only guest flushes (the boot's GPIO polling) are dropped meanwhile and counted in g_drop; the
 * stream time the indicator used is g_ld_cyc; guest time = ptx_now - g_ld_cyc + g_drop (um2bus works on differences of it: a jump would force frame marks). */
#define LD_GRACE 0.4          /* s before the first indicator frame */
#define LD_MINSEC 32          /* loads below this many sectors are not worth a progress bar (nor a sidecar) */
#define LD_WIN 0.5            /* s: a window of frame marks with fewer than LD_QRD sector reads ends the load; a game streaming music reads about 16 per window */
#define LD_QRD 40
#define LD_HOLDMAX 30.0       /* s from the first frame mark: give up waiting for the card */
static pthread_mutex_t g_ld_m = PTHREAD_MUTEX_INITIALIZER;
static volatile pthread_t g_ld_tid;
static unsigned g_ld_total, g_ld_frames;
static char g_ld_stack[1 << 17] __attribute__((aligned(64)));      /* static: a mmap'd thread stack could land in the 0x40000000 window under qemu */
enum { V_ORB = 0xD000, V_ORA, V_DDRB, V_DDRA, V_T1L, V_T1H, V_T2L = 0xD008, V_T2H, V_SR, V_ACR, V_PCR };
/* L O A D I N G on a 2x4 grid, nibbles x<<4|y; 0xFF = pen up (next point is a move), 0xFE = end */
static const uint8_t ld_font[7][10] = {
    { 0x04, 0x00, 0x20, 0xFE },
    { 0x01, 0x03, 0x14, 0x23, 0x21, 0x10, 0x01, 0xFE },
    { 0x00, 0x03, 0x14, 0x23, 0x20, 0xFF, 0x02, 0x22, 0xFE },
    { 0x00, 0x04, 0x14, 0x23, 0x21, 0x10, 0x00, 0xFE },
    { 0x04, 0x24, 0xFF, 0x14, 0x10, 0xFF, 0x00, 0x20, 0xFE },
    { 0x00, 0x04, 0x20, 0x24, 0xFE },
    { 0x23, 0x14, 0x03, 0x01, 0x10, 0x21, 0x22, 0x12, 0xFE } };
enum { LD_STEP = 640, LD_TY = -320, LD_BY = -2240, LD_BH = 1280, LD_BW = 12800, LD_Z = 0x50 };   /* beam units (as ptx_selftest): text 12800 x 2560, bar below, block centred */
static void ld_mux(int sel, int v) { ptx_write(V_ORA, (uint8_t)v); ptx_write(V_ORB, (uint8_t)(sel << 1)); ptx_advance_cycles(2); ptx_write(V_ORB, 1); }
static void ld_zero(void) { ptx_advance_cycles(20); ptx_write(V_PCR, 0xCC); ptx_advance_cycles(40); ptx_write(V_PCR, 0xCE); ld_mux(1, 0); }
/* one ramp (as ptx_selftest): the beam moves (dx, dy) * (n + 1) units in n + 2 cycles, lit or not. A lit stroke stays lit 5 cycles
 * more: the ramp runs on a little after the write that should end it, and the stroke would stop short of its corner */
static void ld_ramp(int dx, int dy, int n, int lit) {
    ld_mux(0, dy); ptx_write(V_ORA, (uint8_t)dx); ptx_write(V_SR, lit ? 0xFF : 0); ptx_write(V_T1L, (uint8_t)n); ptx_write(V_T1H, 0);
    ptx_advance_cycles(n + 2 + (lit ? 5 : 0)); ptx_write(V_SR, 0);
}
static void ld_glyph(const uint8_t *g, int ox, int oy) {
    ld_zero();
    int px = 0, py = 0, mv = 1, st = 0;
    for (; *g != 0xFE; g++) {
        if (*g == 0xFF) { mv = 1; continue; }
        int x = *g >> 4, y = *g & 15;
        if (!st) { ld_ramp((ox + x * LD_STEP) / 64, (oy + y * LD_STEP) / 64, 63, 0); st = 1; }   /* zero -> first point in one move */
        else ld_ramp(16 * (x - px), 16 * (y - py), 39, !mv);   /* 16 * 40 = one grid step */
        mv = 0; px = x; py = y;
    }
}
/* outline box, then `fill` * 128 units of fill (0..96), or fill < 0: a 24-step block sweeping to and fro (by fno); five zig-zag rows are the fill */
/* --message: an error the player can read on the Vectrex, for pitrexd and mister_run.sh (a pick that cannot start, a game that died).
 * '|' starts a new line. */
enum { MSG_STEP = 640, MSG_ADV = 1920, MSG_LINE = 3200, MSG_COLS = 20, MSG_LINES = 4, MSG_SECS = 20 };
static const char *g_msg;
static void msg_glyph(const uint8_t *g, int ox, int oy) {
    ld_zero();
    int px = 0, py = 0, mv = 1, st = 0;
    for (; *g != 0xFE; g++) {
        if (*g == 0xFF) { mv = 1; continue; }
        int x = *g >> 4, y = *g & 15;
        if (!st) { ld_ramp((ox + x * MSG_STEP) / 192, (oy + y * MSG_STEP) / 192, 191, 0); st = 1; }   /* zero -> first point: anywhere on the screen, so a slow ramp */
        else ld_ramp(16 * (x - px), 16 * (y - py), 39, !mv);   /* 16 * 40 = one grid step */
        mv = 0; px = x; py = y;
    }
}
static void msg_frame(char lines[][MSG_COLS + 1], int n) {
    uint64_t t0 = ptx_now_cycles(); unsigned fc = opt.frame_cycles ? opt.frame_cycles : 30000;
    ptx_write(V_DDRA, 0xFF); ptx_write(V_DDRB, 0x9F); ptx_write(V_ORB, 1); ptx_write(V_ACR, 0x98); ptx_write(V_SR, 0);
    ld_mux(2, LD_Z);
    for (int l = 0; l < n; l++) {
        int len = (int)strlen(lines[l]), ox = -len * MSG_ADV / 2, oy = (n - 1) * MSG_LINE / 2 - l * MSG_LINE - 2 * MSG_STEP;
        for (int i = 0; i < len; i++)
            for (unsigned k = 0; k < sizeof ptx_font / sizeof *ptx_font; k++)
                if (ptx_font[k].c == lines[l][i]) { msg_glyph(ptx_font[k].s, ox + i * MSG_ADV, oy); break; }
    }
    uint64_t used = ptx_now_cycles() - t0;
    if (used + 2 < fc) ptx_advance_cycles((uint32_t)(fc - used - 2));
    ptx_write(V_T2L, 0x30); ptx_write(V_T2H, 0x75);
    ptx_frame();
}
static void ld_pace(void);
/* no image, no guest: draw the text for MSG_SECS, then hand the Vectrex back.  Paced like the loading indicator, so it sleeps almost all the time */
static int msg_main(void) {
    char lines[MSG_LINES][MSG_COLS + 1]; int n = 0, c = 0;
    memset(lines, 0, sizeof lines);
    for (const char *p = g_msg; *p && n < MSG_LINES; p++) {
        if (*p == '|') { n++; c = 0; continue; }
        if (c < MSG_COLS) lines[n][c++] = (char)toupper((unsigned char)*p);
    }
    if (n < MSG_LINES) n++;
    g_ld_done = 0; ptx_up(); ptx_boot_phase = 1;
    for (double t = now(); now() - t < MSG_SECS; ) { ld_pace(); msg_frame(lines, n); }
    g_ptx_up = 0; ptx_shutdown();
    return 0;
}
/* the image's VIA registers the indicator overwrites, as it last wrote them (dark: PCR with CB2 low) */
static uint8_t g_ld_via[16]; static unsigned g_ld_seen; static uint64_t g_ld_gw;   /* g_ld_gw: stream time of the image's own dark frames */
static void ld_restore(int real) {
    static const uint8_t r[] = { 0x3, 0x2, 0x0, 0xB, 0xC };
    for (unsigned i = 0; i < sizeof r; i++) if (g_ld_seen >> r[i] & 1) ptx_write((uint16_t)(0xD000 | r[i]), r[i] == 0xC && !real ? (g_ld_via[0xC] & 0x1F) | 0xC0 : g_ld_via[r[i]]);
}
static void ld_bar(int fill, unsigned fno) {
    ld_zero();
    ld_ramp(-LD_BW / 2 / 64, LD_BY / 64, 63, 0);
    ld_ramp(LD_BW / 128, 0, 127, 1); ld_ramp(0, LD_BH / 64, 63, 1); ld_ramp(-LD_BW / 128, 0, 127, 1); ld_ramp(0, -LD_BH / 64, 63, 1);
    int p = 0, len = fill;
    if (fill < 0) { int ph = fno % 64; ph = ph < 32 ? ph : 63 - ph; p = ph * 72 / 31; len = 24; }
    if (len <= 0) return;
    ld_ramp(4, 4, 63, 0);                                            /* 256 units in from the corner */
    if (p) ld_ramp(p, 0, 127, 0);
    for (int k = 0; k < 5; k++) { ld_ramp(k & 1 ? -len : len, 0, 127, 1); if (k < 4) ld_ramp(0, 3, 63, 1); }
}
static void ld_frame(unsigned fno) {
    uint64_t t0 = ptx_now_cycles(); unsigned fc = opt.frame_cycles ? opt.frame_cycles : 30000;
    ptx_write(V_DDRA, 0xFF); ptx_write(V_DDRB, 0x9F); ptx_write(V_ORB, 1); ptx_write(V_ACR, 0x98); ptx_write(V_SR, 0);
    ld_mux(2, LD_Z);
    for (int i = 0; i < 7; i++) ld_glyph(ld_font[i], -LD_BW / 2 + i * 1920, LD_TY);
    unsigned long rd = um2sd_present() ? um2sd_reads() : 0;
    ld_bar(g_ld_total >= LD_MINSEC && um2sd_present() ? (rd >= g_ld_total ? 96 : (int)(rd * 96 / g_ld_total)) : -1, fno);
    uint64_t used = ptx_now_cycles() - t0;
    if (used + 2 < fc) ptx_advance_cycles((uint32_t)(fc - used - 2));
    ptx_write(V_T2L, 0x30); ptx_write(V_T2H, 0x75);                   /* T2 reload = frame boundary, as um2bus's frame mark */
    ld_restore(0);
    ptx_frame();
    g_ld_cyc += ptx_now_cycles() - t0; g_ld_frames++;
}
/* outside the lock: MiSTer: wait until the core is within a frame of the end of the stream (<= 200 ms); file backend: one frame period */
static void ld_pace(void) {
    ptx_status_t st; ptx_get_status(&st);
    if (!st.valid) { struct timespec ts = { 0, 20000000L }; nanosleep(&ts, NULL); return; }
    for (int i = 0; i < 100 && !g_ld_done; i++) {
        ptx_get_status(&st);
        if ((int64_t)(ptx_now_cycles() - st.core_cycles) <= (int64_t)(opt.frame_cycles ? opt.frame_cycles : 30000)) return;
        struct timespec ts = { 0, 2000000L }; nanosleep(&ts, NULL);
    }
}
static void *ld_thread(void *arg) {
    (void)arg; g_ld_tid = pthread_self();
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGALRM); sigaddset(&m, SIGUSR1); pthread_sigmask(SIG_BLOCK, &m, NULL);   /* the PC sampler and donate ticks belong to the guest threads */
    um2_rt_boost(2);
    for (double t = now(); now() - t < LD_GRACE && !g_ld_done; ) usleep(10000);
    if (g_ld_done) return NULL;
    ptx_up(); ptx_boot_phase = 1;
    for (unsigned f = 0; !g_ld_done; f++) {
        ld_pace();
        pthread_mutex_lock(&g_ld_m);
        if (!g_ld_done) ld_frame(f);
        pthread_mutex_unlock(&g_ld_m);
    }
    return NULL;
}
/* guest side, before its first ring access (and at exit): wait out an in-flight indicator frame and retire the indicator for good */
static void ld_stop(void) {
    if (g_ld_done) return;
    if (pthread_equal(pthread_self(), g_ld_tid)) { g_ld_done = 1; return; }       /* exit() from the indicator thread itself (file backend frame limit) */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
    int r = pthread_mutex_timedlock(&g_ld_m, &ts);                                  /* timed: an exit path may interrupt the thread that holds it */
    g_ld_done = 1; if (!r) pthread_mutex_unlock(&g_ld_m);
    ptx_boot_phase = 0;
}
static void ld_take(void) {
    if (g_ld_done) return;
    ld_stop();
    ld_restore(1);
    unsigned long n = um2sd_present() ? um2sd_reads() : 0;
    if (opt.bus) um2bus_ring_mark();
    if (g_ld_frames || opt.bus_verbose) fprintf(stderr, "loading: image took the ring at %.1f s after %u indicator frames, %lu sector reads (known total %u)\n", now() - g_t0, g_ld_frames, n, g_ld_total);
    if (n >= LD_MINSEC && n != g_ld_total && opt.file) {                                         /* remember the total; unwritable path: silently skipped */
        char p[1100]; snprintf(p, sizeof p, "%s.load", opt.file); FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "%lu\n", n); fclose(f); g_ld_total = (unsigned)n; }
    }
}
static uint64_t gnow(void) { return g_ld_done ? ptx_now_cycles() - g_ld_cyc + g_drop : g_drop + g_ld_gw; }   /* the image's own stream time */
/* called at a frame mark while the indicator runs: has the image finished loading? */
static int ld_ready(void) {
    static double t0, tw; static unsigned long rw; static int off = -1;
    if (off < 0) off = getenv("UM2_NOLDHOLD") != NULL;
    if (off || !um2sd_present()) return 1;
    double t = now(); unsigned long rd = um2sd_reads();
    if (!t0) { t0 = tw = t; rw = rd; return 0; }
    if (t - t0 > LD_HOLDMAX) return 1;
    if (t - tw < LD_WIN) return 0;
    if (opt.bus_verbose) fprintf(stderr, "loading: %.1f s, %lu sector reads in the last window\n", t - g_t0, rd - rw);
    if (rd - rw < LD_QRD) return 1;
    tw = t; rw = rd; return 0;
}
static void ld_init(void) {
    if (g_noload || !opt.file) return;
    char p[1100]; snprintf(p, sizeof p, "%s.load", opt.file); FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%u", &g_ld_total) != 1) g_ld_total = 0; fclose(f); }
    g_ld_done = 0;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstack(&at, g_ld_stack, sizeof g_ld_stack);
    if (pthread_create(&th, &at, ld_thread, NULL)) { perror("loading indicator"); g_ld_done = 1; } else pthread_detach(th);
}

static int hs_cmp(const void *a, const void *b) { const Hs *x = a, *y = b; return x->n < y->n ? 1 : x->n > y->n ? -1 : 0; }
static void hs_dump(void) {
    if (g_hs_on <= 0) return;
    { static Hs q[512]; int m = 0; uint64_t tt = 0; for (int i = 0; i < 512; i++) if (g_sc[i].n) { q[m++] = g_sc[i]; tt += g_sc[i].n; }
      qsort(q, (size_t)m, sizeof q[0], hs_cmp); unsigned f2 = g_marks - g_cen_m0; if (!f2) f2 = 1;
      fprintf(stderr, "STUBS: %d sites, %llu stub calls over %u frames (%.1f per frame)\n", m, (unsigned long long)tt, f2, (double)tt / f2);
      for (int i = 0; i < m && i < 24; i++) fprintf(stderr, "STUB %08x %8llu %7.2f/f %s %08x..%08x %s\n", q[i].pc, (unsigned long long)q[i].n, (double)q[i].n / f2, q[i].ld ? "ld" : "st", q[i].amin, q[i].amax, blockname(q[i].amin)); }
    static Hs l[HSCAP]; int n = 0; uint64_t tot = 0, tot_p = 0;
    for (int i = 0; i < HSCAP; i++) if (g_hs[i].n) { l[n++] = g_hs[i]; tot += g_hs[i].n; }
    qsort(l, (size_t)n, sizeof l[0], hs_cmp);
    unsigned fr = g_marks - g_cen_m0; if (!fr) fr = 1;
    fprintf(stderr, "SITES: %d sites, %llu memory traps over %u frames (%.1f per frame); exc-return faults %lu (%.1f per frame)\n", n, (unsigned long long)tot, fr, (double)tot / fr, g_excret, (double)g_excret / fr);
    for (int i = 0; i < n && i < 40; i++) {
        Hs *s = &l[i]; int pat = is_patched(s->pc) || bm_get(g_pcover, s->pc);
        if (pat) tot_p += s->n;
        fprintf(stderr, "SITE %08x %8llu %6.2f/f %s%d %08x..%08x %-14s %s tries %u: %s\n", s->pc, (unsigned long long)s->n, (double)s->n / fr, s->ld ? "ld" : "st", s->size * 8, s->amin, s->amax,
                blockname(s->amin), pat ? "PATCHED" : "open   ", s->tries, pat ? "(traps before the patch)" : g_why_txt[s->why < HW_NKIND ? s->why : 0]);
    }
}
/* Per-thread CPU time and scheduling counts at the end of a run, from /proc/self/task (UM2_NOTHREADS=1: off).  The PC sampler (UM2_PROF) is a
 * process-wide ITIMER_PROF whose signal lands on whichever thread the tick found running, so its per-core sample counts say little about how long
 * each guest core really ran; this does.  cpu = user+sys seconds; wait = time runnable but not running (CONFIG_SCHEDSTATS kernels only). */
static void thr_dump(FILE *f) {
    if (getenv("UM2_NOTHREADS")) return;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    struct dirent *e;
    fprintf(f, "um2run: threads (cpu s = user+sys; ctxsw = voluntary/involuntary; run/wait s from schedstat when the kernel has it), run %.1f s:\n", now() - g_t0);
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[320], b[1024]; snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
        FILE *g = fopen(p, "r"); if (!g) continue;
        size_t n = fread(b, 1, sizeof b - 1, g); fclose(g); b[n] = 0;
        char *o = strchr(b, '('), *c = strrchr(b, ')'); if (!o || !c || c < o) continue;
        *c = 0; char nm[32]; snprintf(nm, sizeof nm, "%s", o + 1);
        unsigned long ut = 0, st = 0; int cpu = -1;
        { char *q = c + 2; int fld = 3; while (*q && fld < 40) { if (fld == 14) ut = strtoul(q, NULL, 10); if (fld == 15) st = strtoul(q, NULL, 10); if (fld == 39) cpu = atoi(q); while (*q && *q != ' ') q++; if (*q) q++; fld++; } }
        unsigned long vol = 0, inv = 0;
        snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name);
        if ((g = fopen(p, "r"))) { char l[128]; while (fgets(l, sizeof l, g)) { sscanf(l, "voluntary_ctxt_switches: %lu", &vol); sscanf(l, "nonvoluntary_ctxt_switches: %lu", &inv); } fclose(g); }
        unsigned long long rn = 0, wn = 0; int ss = 0;
        snprintf(p, sizeof p, "/proc/self/task/%s/schedstat", e->d_name);
        if ((g = fopen(p, "r"))) { ss = fscanf(g, "%llu %llu", &rn, &wn) == 2 && (rn || wn); fclose(g); }
        fprintf(f, "  %-8s tid %s cpu %.2f (user %.2f sys %.2f) on CPU %d, ctxsw %lu/%lu", nm, e->d_name, (double)(ut + st) / hz, (double)ut / hz, (double)st / hz, cpu, vol, inv);
        if (ss) fprintf(f, ", run %.2f wait %.2f", rn * 1e-9, wn * 1e-9);
        fputc('\n', f);
    }
    closedir(d);
    char pr[512]; if (ptx_pace_report(pr, sizeof pr) > 0) fprintf(f, "um2run: %s\n", pr);
}
static void bus_atexit(void) { if (!exit_claim(0)) for (;;) pause(); bus_finish(); }
static void bus_finish(void) {
    rh_dump(); hs_dump();
    if (!opt.bus) return;
    if (opt.bus_verbose) { print_summary(1); fflush(stdout); }
    fprintf(stderr, "um2run: %llu traps, %u stubs (%u LDA/STL with dmb); %u branch targets known, %lu patch attempts postponed (an exception returns inside)\n", (unsigned long long)g_events, g_nstubs, g_nv8, g_nbtgt, g_patch_frame);
    if (g_nvrint) fprintf(stderr, "um2run: %u VRINT sites became stubs\n", g_nvrint);
    if (g_nvmaxnm) fprintf(stderr, "um2run: %u VMAXNM/VMINNM sites became stubs\n", g_nvmaxnm);
    if (g_nsvcp) { fprintf(stderr, "um2run: %u SVC veneers became stubs; native SVC entries (call number:count):", g_nsvcp); for (int q = 0; q < 256; q++) if (g_svc_nat[q]) fprintf(stderr, " %d:%lu", q, g_svc_nat[q]); fputc(10, stderr); }
    lb_dump(stderr); um_dump(stderr);
    thr_dump(stderr);
    um2bus_stats(stderr); um2sd_stats();
    if (getenv("UM2_HASHMEM") && !opt.no_psram) {      /* test hook: FNV-1a of the guest's PSRAM (where the images stage what they read from the SD card): what the guest really received */
        uint64_t h = 0xcbf29ce484222325ull; const uint32_t *p = (const uint32_t *)(uintptr_t)PSRAM_BASE;
        for (uint32_t i = 0; i < PSRAM_SIZE / 4; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        fprintf(stderr, "um2run: PSRAM hash %016llx\n", (unsigned long long)h);
    }
    if (getenv("UM2_PSRAMUSE") && !opt.no_psram) {     /* diagnostics: how much of the 8 MB PSRAM the guest wrote (highest and number of non-zero 4 KiB pages): an image that needs more than we model shows up here */
        const uint32_t *p = (const uint32_t *)(uintptr_t)PSRAM_BASE; uint32_t hi = 0, used = 0;
        for (uint32_t pg = 0; pg < PSRAM_SIZE / 4096; pg++) { int nz = 0; for (uint32_t i = 0; i < 1024 && !nz; i++) nz = p[pg * 1024 + i] != 0; if (nz) { used++; hi = pg + 1; } }
        fprintf(stderr, "um2run: PSRAM use: %u of %u pages non-zero, highest page end %u KiB of %u KiB\n", used, (unsigned)(PSRAM_SIZE / 4096), hi * 4, (unsigned)(PSRAM_SIZE / 1024));
    }
#ifdef UM2_VFPTEST
    fprintf(stderr, "vfptest: %u helper calls from stubs, %u of them on the guest stack\n", g_vt_calls, g_vt_guest);
#endif
    um2aud_stop();
    if (g_ptx_up) { ld_stop(); if (g_hold) hq_flush(); g_ptx_up = 0; ptx_shutdown(); }
}
static inline void gap_note(void) { if (g_ringmon > 1) { double t = now(); if (g_lastw && t - g_lastw > g_maxgap) g_maxgap = t - g_lastw; g_lastw = t; } }
/* --hold: a whole frame's commands are queued here and handed to the ring only at the next frame mark (or before a read, or after 50 ms),
 * so the core never starves in the middle of a list: with the producer slower than real time, cmd-by-cmd publication made the ring
 * run dry mid-vector (see DESIGN.md).  Costs one frame of latency.  Stream time is tracked in g_vnow meanwhile. */
typedef struct { uint8_t kind, data; uint16_t addr; uint32_t n; } HQ;
static HQ g_hq[1 << 16]; static uint32_t g_nhq; static uint64_t g_vnow; static double g_hq_t0;
static void hq_flush(void) {
    if (!g_ld_done) {       /* the indicator owns the ring: idle time is dropped and counted, the image's writes go in dark, whole queues between indicator frames */
        pthread_mutex_lock(&g_ld_m);
        if (!g_ld_done) {
            uint64_t t0 = ptx_now_cycles();
            for (uint32_t i = 0; i < g_nhq; i++) {
                if (g_hq[i].kind) { g_drop += g_hq[i].n; continue; }
                uint16_t a = g_hq[i].addr; uint8_t d = g_hq[i].data; unsigned r = a & 15;
                if ((a & 0xF000) == 0xD000) {
                    if (r == 8 || r == 9) { g_drop++; continue; }           /* no T2 reload: it would mark a frame */
                    g_ld_via[r] = d; g_ld_seen |= 1u << r;
                    if (r == 0xA) d = 0; else if (r == 0xC) d = (d & 0x1F) | 0xC0;   /* blanking: shift register empty, CB2 low */
                }
                ptx_write(a, d);
            }
            g_ld_gw += ptx_now_cycles() - t0; g_nhq = 0;
            pthread_mutex_unlock(&g_ld_m);
            return;
        }
        pthread_mutex_unlock(&g_ld_m);
    }
    for (uint32_t i = 0; i < g_nhq; i++) {
        if (g_hq[i].kind == 0) { gap_note(); ptx_write(g_hq[i].addr, g_hq[i].data); } else ptx_advance_cycles(g_hq[i].n);
    }
    g_nhq = 0;
}
static void hq_add(int kind, uint16_t a, uint8_t d, uint32_t n) {
    if (!g_nhq) g_hq_t0 = now();
    if (g_nhq == (1u << 16) - 1 || ((g_nhq & 255) == 255 && now() - g_hq_t0 > 0.05)) hq_flush();
    g_hq[g_nhq++] = (HQ){ (uint8_t)kind, d, a, n };
}
/* Analog stick. The comparator (PB5) compares the DAC (port A, signed) with the sample/hold cap, NOT with the live pot: PB0=0 connects the pot selected by
 * PB2:1 (0 X1, 1 Y1, 2 X2, 3 Y2) to the cap, PB0=1 holds what it has taken. Games sample one axis (enable, wait, inhibit) and then write another select value
 * or PB0 pattern while they read (BattleZone: enable sel 1, inhibit with sel 0, compare => Y; the old live-select model read X again, so Y was a copy of X).
 * Modelled here for every image (instant charge); UM2_NOSH=1 = the old live select. --pot "FRAME:X1,Y1[,X2,Y2];..." injects stick values from frame mark FRAME on (PC tests). */
static int g_npot; static unsigned g_potf[16]; static int8_t g_potv[16][4];
static int8_t pot_get(int i) { ptx_input_t in; ptx_get_input(&in); int8_t v = in.pot[i & 3]; for (int j = 0; j < g_npot; j++) if (g_marks >= g_potf[j]) v = g_potv[j][i & 3]; return v; }
static int g_sh_off = -1, g_sh_en, g_sh_sel; static int8_t g_sh_cap, g_sh_dac; static uint8_t g_sh_ddrb;
static void sh_write(uint16_t a, uint8_t d) {
    if ((a & 0xF000) != 0xD000) return;
    switch (a & 0xF) {
    case 0x0: if (!(d & 1)) { g_sh_en = 1; g_sh_sel = (d >> 1) & 3; } else { if (g_sh_en) g_sh_cap = pot_get(g_sh_sel); g_sh_en = 0; } break;   /* charge ends when PB0 rises */
    case 0x1: case 0xF: g_sh_dac = (int8_t)d; break;
    case 0x2: g_sh_ddrb = d; break;
    }
}
static uint8_t sh_read(uint16_t a, uint8_t v) {
    if (g_sh_off < 0) g_sh_off = getenv("UM2_NOSH") != NULL;
    if (g_sh_off || (a & 0xF) != 0 || (a & 0xF000) != 0xD000 || (g_sh_ddrb & 0x20)) return v;
    int8_t cap = g_sh_en ? pot_get(g_sh_sel) : g_sh_cap;           /* mux on: the cap follows the pot */
    return (uint8_t)((v & ~0x20) | (cap > g_sh_dac ? 0x20 : 0));
}
static void k_out(uint16_t a, uint8_t d) { if (g_hold) { hq_add(0, a, d, 0); g_vnow += 1; } else { ld_take(); gap_note(); ptx_write(a, d); } }
static void k_write_raw(uint16_t a, uint8_t d) { gup(); sh_write(a, d); k_out(a, d); }
/* The SDK's re-zero primes the zero reference with a value meant for a real console's analogue error (0x23, then 0xFF for
 * 4 cycles). The core has no such error and its reference settles at about -1, so every ramp picks up a constant drift and
 * rows of text lean.  Keep the reference at 0: charge it once, then leave the mux closed on that channel.  Env UM2_ZEROGAME: the image's own value.
 * Only the core sees the closed mux: the same select (PB2:1 = 1, PB0 = 0) samples stick Y1, so the stick model gets the game's own value. */
static void k_advance(uint32_t c);
static void k_write(uint16_t a, uint8_t d) {
    static int game = -1, primed;
    if (a == V_ORB && (d & 7) == 2) {
        if (game < 0) game = getenv("UM2_ZEROGAME") != NULL;
        if (!game) {
            gup();
            if (!primed) { primed = 1; k_out(V_ORA, 0); k_out(V_ORB, d); k_advance(16); }
            sh_write(a, d); k_out(a, d | 1);
            return;
        }
    }
    k_write_raw(a, d);
}
/* --keys "FRAME:BYTE,FRAME:BYTE,...": PC testing only. From frame mark FRAME on, a controller read (port A, nothing pressed = 0xFF) returns BYTE
 * (PSG reg 14, active low: button 1..4 = bit 0..3; coin = button 3 = 0xFB, start = button 4 = 0xF7). */
static unsigned g_keyf[16]; static unsigned g_keyv[16]; static int g_nkeys;
static uint8_t key_now(void) { uint8_t v = 0xFF; for (int i = 0; i < g_nkeys; i++) if (g_marks >= g_keyf[i]) v = (uint8_t)g_keyv[i]; return v; }
static struct { uint16_t a; uint8_t v, k; unsigned n; } g_rh[48]; static int g_nrh;
static void rh_note(uint16_t a, uint8_t v, uint8_t k) { for (int i = 0; i < g_nrh; i++) if (g_rh[i].a == a && g_rh[i].v == v && g_rh[i].k == k) { g_rh[i].n++; return; } if (g_nrh < 48) { g_rh[g_nrh].a = a; g_rh[g_nrh].v = v; g_rh[g_nrh].k = k; g_rh[g_nrh++].n = 1; } }
static void rh_dump(void) { if (opt.bus_verbose < 2) return; for (int i = 0; i < 32; i++) if (g_tm_reads[i]) fprintf(stderr, "timer reg %02x reads %u (loop-breaker overrode %u)\n", i * 4, g_tm_reads[i], g_tm_broken[i]); for (int i = 0; i < g_nrh; i++) fprintf(stderr, "read reg %x val %02x key %02x: %u\n", g_rh[i].a & 0xF, g_rh[i].v, g_rh[i].k, g_rh[i].n); }
static uint8_t ld_read(uint16_t a) { pthread_mutex_lock(&g_ld_m); uint64_t t0 = ptx_now_cycles(); uint8_t v = ptx_read(a); g_ld_gw += ptx_now_cycles() - t0; pthread_mutex_unlock(&g_ld_m); return v; }
static uint8_t k_read(uint16_t a) { gup(); if (!g_hold) ld_take(); uint8_t v; if (g_hold) { hq_flush(); v = g_ld_done ? ptx_read(a) : ld_read(a); g_vnow = gnow(); } else v = ptx_read(a); v = sh_read(a, v); if (g_nkeys && (a & 0xF) == 1) { uint8_t k = key_now(); if (k != 0xFF) v = k; } if (opt.bus_verbose > 1) rh_note(a, v, g_nkeys ? key_now() : 0); return v; }
static void k_advance(uint32_t c) { gup(); if (g_hold) { hq_add(1, 0, 0, c); g_vnow += c; } else if (g_ld_done) ptx_advance_cycles(c); else g_drop += c; }
static uint64_t k_now(void) { gup(); if (g_hold) { if (!g_vnow) g_vnow = gnow(); return g_vnow; } return gnow(); }
static void k_frame(void) {
    g_marks++;
    { static unsigned cr; static int cri = -1; if (cri < 0) { const char *e = getenv("UM2_CENRESET"); cr = e ? (unsigned)atoi(e) : 0; cri = 1; }   /* census window: forget everything before frame cr (diagnostics; patched sites stay patched) */
      if (cr && g_marks == cr) { hs_reset(); g_excret = 0; memset(g_ev, 0, sizeof g_ev); memset(g_kdistinct, 0, sizeof g_kdistinct); memset(g_irq_cnt, 0, sizeof g_irq_cnt); g_irq_kicks = 0; g_irq_pc[0] = g_irq_pc[1] = g_irq_async[0] = g_irq_async[1] = 0; g_irq_trap = g_irq_chain = 0; g_cen_m0 = g_marks; g_events = 0; fprintf(stderr, "um2run: census reset at frame %u\n", cr); } }
    if (g_hs_on > 0 && !(g_marks % 50)) { static uint64_t pe; static unsigned long px; if (g_events < pe) pe = 0; if (g_excret < px) px = 0; fprintf(stderr, "FRAMES %u: +%llu traps +%lu exc-returns since the last mark (%.1f round trips per frame)\n", g_marks, (unsigned long long)(g_events - pe), g_excret - px, (double)(g_events - pe + g_excret - px) / 50.0); pe = g_events; px = g_excret; }
    gup();
    if (g_hold && !g_ld_done && !ld_ready()) {                          /* still loading: no frame mark, and no faster than 50 Hz (a late frame has nothing to wait for, so the card load never stalls here) */
        static double t; hq_flush(); g_vnow = gnow();
        double d = 0.02 - (now() - t); if (d > 0) { struct timespec ts = { 0, (long)(d * 1e9) }; nanosleep(&ts, NULL); }
        g_last_frame_t = t = now();
        return;
    }
    ld_take(); if (g_hold) { hq_flush(); g_vnow = gnow(); } ptx_frame(); g_last_frame_t = now();
    /* Once the game is producing lists, core 1 (the real-time consumer) runs one level above core 0: core 0's native spin-waits then only
     * use the CPU while core 1 sleeps (pacing naps, ring waits) instead of taking half of it.  Not from the start: during boot core 1's
     * idle input reads would starve core 0 before it publishes the first list. */
    static int nf; if (++nf == 6 && !getenv("UM2_NOBOOST")) { um2_rt_boost(1); g_boosted = 1; }
}
static uint8_t *k_host_ptr(uint32_t a, uint32_t len) {
    if (a >= IMG_BASE && (uint64_t)a + len <= IMG_TOP) return (uint8_t *)(uintptr_t)a;
    if (!opt.no_psram && a >= PSRAM_BASE && (uint64_t)a + len <= (uint64_t)PSRAM_BASE + PSRAM_SIZE) return (uint8_t *)(uintptr_t)a;
    if (!opt.no_psram && a >= PSRAM_BASE + 0x4000000u && (uint64_t)a + len <= (uint64_t)PSRAM_BASE + 0x4000000u + PSRAM_SIZE) return (uint8_t *)(uintptr_t)a;
    return NULL;
}
static uint32_t k_ext_load(uint32_t a, int size) { return mmio_load_base(a, size, 0); }
static void k_ext_store(uint32_t a, int size, uint32_t v) { mmio_store_base(a, size, v); }

static uint32_t g_c1_vtor, g_c1_sp, g_c1_entry;
static void install_sigstack(void) {
    static char stacks[4][1 << 16] __attribute__((aligned(64))); static int nst; size_t sz = 1 << 16; void *m = stacks[nst++ & 3];
    stack_t ss = { m, 0, sz }; sigaltstack(&ss, NULL);
}
/* --cpu1-normal (10 Nov 2026): once core 1 has its own CPU, it leaves real-time scheduling for SCHED_OTHER at nice -20, so that Linux on
 * that CPU (the USB/Bluetooth softirq work, MiSTer Main) gets it whenever it needs it, without a fixed donation; core 0, which plays the
 * cart's sound, stays real-time.  Never before the move: on one shared CPU a priority gap starves a guest thread (see um2_rt_boost). */
static int g_c1_normal;
static void c1_go_normal(pid_t tid) {
    struct sched_param sp = { 0 };
    if (sched_setscheduler(tid, SCHED_OTHER, &sp)) { perror("core1 SCHED_OTHER"); return; }
    if (setpriority(PRIO_PROCESS, (id_t)tid, -20)) perror("core1 nice");
    fprintf(stderr, "core 1: normal scheduling, nice -20\n");
}
static void *core1_thread(void *arg) {
    (void)arg;
    sigset_t e; sigemptyset(&e); pthread_sigmask(SIG_SETMASK, &e, NULL);
    install_sigstack();
    tl_core = 1; tl_guest = 1; hstack_set(1); pthread_setname_np(pthread_self(), "u2core1");
    g_c1_tid = (pid_t)syscall(SYS_gettid);
    if (g_cpu1 >= 0 && g_cpu1_after <= 0) { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_cpu1, &cs); if (sched_setaffinity(0, sizeof cs, &cs)) perror("core1 affinity"); else { g_c1_own_cpu = 1; if (g_c1_normal) c1_go_normal(0); } }
    g_th_core1 = pthread_self(); __atomic_store_n(&g_have_core1, 1, __ATOMIC_RELEASE);      /* published after the thread id (review item 7) */
    uint32_t sp = g_c1_sp, ent = g_c1_entry | 1;
    fprintf(stderr, "core 1: sp=%08x entry=%08x\n", sp, ent);
    { const char *d = getenv("UM2_C1_DELAY"); if (d) { double s = atof(d); struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&ts, NULL); } }   /* PC test hook: core 1 starts late (starvation experiments) */
    __asm__ volatile("mov sp, %0\n\t"
                     "bx %1" :: "r"(sp), "r"(ent) : "memory");
    return NULL;
}
/* Black-screen guard (26 Oct 2026): Main's MiSTer_fb video-mode setup needs CPU1; a spinning RT guest thread pinned there from the
 * first instant starved it and the monitor stayed black.  So core 1 starts on the CPU core 0 uses and only moves after a settle time. */
static void *c1_mover(void *arg) {
    (void)arg;
    struct timespec ts = { (time_t)g_cpu1_after, (long)((g_cpu1_after - (time_t)g_cpu1_after) * 1e9) }; nanosleep(&ts, NULL);
    while (!g_c1_tid) usleep(10000);
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_cpu1, &cs);
    if (sched_setaffinity(g_c1_tid, sizeof cs, &cs)) perror("core1 move"); else { g_c1_own_cpu = 1; fprintf(stderr, "core 1 moved to CPU %d after %.1f s\n", g_cpu1, g_cpu1_after); if (g_c1_normal) c1_go_normal(g_c1_tid); }
    return NULL;
}
static void k_launch_core1(uint32_t vtor, uint32_t sp, uint32_t entry) {
    g_c1_vtor = vtor; g_c1_sp = sp; g_c1_entry = entry;
    g_last_frame_t = now();      /* the image is still booting until here (Punch-Out reads SD for 20 s first): count the no-frame timeout from the core-1 launch */
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 16);
    if (pthread_create(&th, &at, core1_thread, NULL)) perror("pthread_create(core1)");
    else pthread_detach(th);
    if (g_cpu1 >= 0 && g_cpu1_after > 0) { pthread_t mt; pthread_attr_t ma; pthread_attr_init(&ma); pthread_attr_setstacksize(&ma, 1 << 16);
        if (!pthread_create(&mt, &ma, c1_mover, NULL)) pthread_detach(mt); }
}
static void k_irq_poke(void) { if (g_irq_on) irq_levels(); }
/* Only once on_donate is installed: SIGUSR1's default action kills the process, silently (the launch deaths of 28 and 31 Oct 2026, whenever
 * setup took longer than the 2 ms before the first tick). */
static void fair_start(void) {
    int pol = sched_getscheduler(0); if ((pol == SCHED_RR || pol == SCHED_FIFO) && !getenv("UM2_NOFAIR")) { pthread_t ft; pthread_attr_t fa; pthread_attr_init(&fa); pthread_attr_setstacksize(&fa, 1 << 16);
        if (!pthread_create(&ft, &fa, fair_thread, NULL)) pthread_detach(ft); }
}
static void bus_setup(void) {
    /* qemu-user parks the process stack (argv/envp) in the 0x40000000 window that guest_jump unmaps; getenv() from the
     * signal handlers would fault.  Move the environment and argv string to the heap first (no-op cost on real Linux). */
    { int n = 0; while (environ[n]) n++;
      char **e = malloc((size_t)(n + 1) * sizeof *e);
      for (int i = 0; i < n; i++) e[i] = strdup(environ[i]);
      e[n] = NULL; environ = e;
      if (opt.file) opt.file = strdup(opt.file);
      if (opt.capture) opt.capture = strdup(opt.capture); }
    volatile uint32_t *dma_rw = NULL;
    if (!getenv("UM2_NOROPAGE")) {
        /* DMA register page: read-only for the guest (polls are free), writable through an alias for the model. */
        int fd = (int)syscall(SYS_memfd_create, "um2dma", 0);
        if (fd >= 0 && ftruncate(fd, 4096) == 0) {
            void *rw = mmap((void *)0x70001000u, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            void *ro = mmap((void *)0x50000000u, 4096, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
            if (rw != MAP_FAILED && ro == (void *)0x50000000u) dma_rw = (volatile uint32_t *)rw;
            else perror("um2run: DMA page");
        } else perror("um2run: memfd_create");
    }
    g_th_core0 = pthread_self(); pthread_setname_np(pthread_self(), "u2core0");
    um2bus_cfg_t c; memset(&c, 0, sizeof c);
    c.vwrite = k_write; c.vread = k_read; c.advance = k_advance; c.now = k_now; c.frame = k_frame;
    c.host_ptr = k_host_ptr; c.ext_load = k_ext_load; c.ext_store = k_ext_store; c.launch_core1 = k_launch_core1;
    c.dma_mirror = dma_rw; c.c1_own_cpu = &g_c1_own_cpu; c.sysclk = sysclk_hz; c.irq_poke = k_irq_poke;
    c.verbose = opt.bus_verbose; c.frame_cycles = opt.frame_cycles; c.capture_path = opt.capture;
    um2bus_init(&c);
    if (g_sd_path) { g_sd_path = strdup(g_sd_path); if (um2sd_open(g_sd_path, opt.bus_verbose)) exit(1); }
    ld_init();
    irq_init();
}

static int run_inproc(void) {
    if (!opt.patch_svc && !opt.unsafe
#ifdef USE_SECCOMP
        && 0
#endif
        ) { fprintf(stderr, "inproc mode needs --patch-svc (or a -DUSE_SECCOMP build); refusing: a real svc would run a host syscall chosen by r7\n"); return 2; }
    g_rd16 = rd16_inproc; g_wr16 = wr16_inproc;
    static char altstack[1 << 16] __attribute__((aligned(64)));
    stack_t ss = { altstack, 0, sizeof altstack }; sigaltstack(&ss, NULL);
    if (opt.bus) { bus_setup(); atexit(bus_atexit); }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigfillset(&sa.sa_mask);
    sigdelset(&sa.sa_mask, SIGKILL);
    sigaction(SIGILL, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGTRAP, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
#ifdef USE_SECCOMP
    sa.sa_sigaction = on_sys; sigaction(SIGSYS, &sa, NULL);
#endif
    sa.sa_sigaction = on_alarm; sigaction(SIGALRM, &sa, NULL);
    sa.sa_sigaction = on_donate; sigaction(SIGUSR1, &sa, NULL);
    if (opt.bus) fair_start();
    sa.sa_sigaction = on_quiesce; sigaction(SIGUSR2, &sa, NULL);
    sa.sa_sigaction = on_irq; sigaction(SIGURG, &sa, NULL);       /* asynchronous interrupt delivery (SIGUSR2 is the patch quiesce) */
    guest_load_and_map();
    hstack_set(0); tl_guest = 1; prof_start(); slice_start();
    g_t0 = now();
    struct itimerval it = { {0, 100000}, {0, 100000} }; setitimer(ITIMER_REAL, &it, NULL);
#ifdef USE_SECCOMP
    install_seccomp();
#endif
    guest_jump();
}

/* ---- ptrace back-end */
static volatile sig_atomic_t g_tick;
static void on_tick(int s) { (void)s; g_tick = 1; }
static int g_memfd = -1;
static int rd16_ptrace(uint32_t a) { uint16_t v; if (pread(g_memfd, &v, 2, a) != 2) return -1; return v; }

static pid_t g_pid;
static void wr16_ptrace(uint32_t a, uint16_t v) {
    uint32_t base = a & ~3u; errno = 0;
    long w = ptrace(PTRACE_PEEKTEXT, g_pid, (void *)(uintptr_t)base, 0);
    if (errno) return;
    uint32_t u = (uint32_t)w, sh = (a & 2) * 8;
    u = (u & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    ptrace(PTRACE_POKETEXT, g_pid, (void *)(uintptr_t)base, (void *)(uintptr_t)u);
}
static int tick_service(pid_t pid, int *sample_pending) {
    g_ticks++;
    if (g_ticks % 20 == 0) { heartbeat(pid); fflush(stdout); }
    if (now() - g_t0 > opt.secs) { printf("-- time limit reached\n"); return 1; }
    if (!*sample_pending) { *sample_pending = 1; kill(pid, SIGSTOP); }
    return 0;
}

static int run_ptrace(void) {
    g_rd16 = rd16_ptrace; g_wr16 = wr16_ptrace;
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (!pid) {
        guest_load_and_map();
        if (ptrace(PTRACE_TRACEME, 0, 0, 0)) { perror("PTRACE_TRACEME"); _exit(2); }
        raise(SIGSTOP);
        guest_jump();
    }
    int st; g_pid = pid;
    if (waitpid(pid, &st, 0) < 0 || !WIFSTOPPED(st)) { fprintf(stderr, "child did not stop (status %x)\n", st); return 2; }
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
    char p[64]; snprintf(p, sizeof p, "/proc/%d/mem", pid);
    g_memfd = open(p, O_RDONLY);
    if (g_memfd < 0) { perror("open /proc/pid/mem"); kill(pid, SIGKILL); return 2; }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_tick; sigaction(SIGALRM, &sa, NULL);     /* no SA_RESTART: waitpid must return EINTR */
    struct itimerval it = { {0, 100000}, {0, 100000} }; setitimer(ITIMER_REAL, &it, NULL);
    g_t0 = now();

    uint32_t svc_ret = 0; int in_sys = 0, svc_pending = 0, sample_pending = 0, resume_sig = 0, stop = 0;
    while (!stop) {
        if (ptrace(PTRACE_SYSCALL, pid, 0, (void *)(long)resume_sig)) { perror("PTRACE_SYSCALL"); break; }
        resume_sig = 0;
        for (;;) {
            pid_t w = waitpid(pid, &st, 0);
            int we = errno;
            if (g_tick) { g_tick = 0; if (tick_service(pid, &sample_pending)) { stop = 1; break; } }   /* also when waitpid did NOT return EINTR */
            if (w < 0 && we == EINTR) continue;
            if (w < 0) { errno = we; perror("waitpid"); stop = 1; }
            break;
        }
        if (stop) break;
        if (WIFEXITED(st)) { printf("-- guest exited with status %d\n", WEXITSTATUS(st)); pid = 0; break; }
        if (WIFSIGNALED(st)) { printf("-- guest killed by signal %d\n", WTERMSIG(st)); pid = 0; break; }
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st);
        unsigned long ur[18];
        if (sig == (SIGTRAP | 0x80)) {                          /* syscall stop */
            if (ptrace(PTRACE_GETREGS, pid, 0, ur)) continue;
            in_sys = (ur[12] == 0);          /* ARM: ip is 0 at syscall entry, 1 at exit */
            if (in_sys) {                                       /* entry */
                uint32_t pc = ur[15];
                if (pc > IMG_BASE && pc <= IMG_TOP && (ur[16] & 0x20)) {
                    int hw = rd16_ptrace(pc - 2);
                    if (hw >= 0 && (hw & 0xFF00) == 0xDF00) {
                        Cpu c; for (int i = 0; i < 17; i++) c.r[i] = ur[i];
                        int act = cpu_trap(SIGSYS, &c, 0, 1, hw);
                        svc_pending = 1; svc_ret = c.r[0];
                        ptrace(PTRACE_SET_SYSCALL, pid, 0, (void *)(long)-1);
                        if (act == ACT_STOP) { stop = 1; break; }
                        continue;
                    }
                    printf("note: syscall from inside the image at pc=%08x without an svc halfword before it (hw=%04x)\n", pc, hw < 0 ? 0 : hw);
                }
            } else if (svc_pending) {                           /* exit: deliver r0 */
                svc_pending = 0;
                ur[0] = svc_ret;
                ptrace(PTRACE_SETREGS, pid, 0, ur);
            }
            continue;
        }
        if (sig == SIGSTOP) {
            if (sample_pending && !ptrace(PTRACE_GETREGS, pid, 0, ur)) { for (int i = 0; i < 17; i++) g_lastregs[i] = ur[i]; g_haveregs = 1; sample_pc(ur[15]); }
            sample_pending = 0;
            continue;
        }
        if (sig == SIGILL || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP || sig == SIGFPE) {
            siginfo_t si; memset(&si, 0, sizeof si);
            ptrace(PTRACE_GETSIGINFO, pid, 0, &si);
            if (ptrace(PTRACE_GETREGS, pid, 0, ur)) { perror("GETREGS"); break; }
            Cpu c; for (int i = 0; i < 17; i++) c.r[i] = ur[i];
            int act = cpu_trap(sig, &c, (uint32_t)(uintptr_t)si.si_addr, 0, 0);
            if (act == ACT_STOP) break;
            for (int i = 0; i < 17; i++) ur[i] = c.r[i];
            ptrace(PTRACE_SETREGS, pid, 0, ur);
            continue;                                           /* signal suppressed */
        }
        resume_sig = sig;                                       /* any other signal: pass through */
    }
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    print_summary(1);
    return 0;
}
#endif /* __arm__ */

/* ------------------------------------------------------------------ static census (--parse) */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16s(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int dec_bw(const uint8_t *img, uint32_t addr, uint32_t *tgt) {    /* b.w (T4) */
    uint16_t a = rd16s(img + (addr - IMG_BASE)), b = rd16s(img + (addr - IMG_BASE) + 2);
    if ((a & 0xF800) != 0xF000 || (b & 0xD000) != 0x9000) return 0;
    uint32_t S = (a >> 10) & 1, J1 = (b >> 13) & 1, J2 = (b >> 11) & 1, i1 = !(J1 ^ S), i2 = !(J2 ^ S);
    int32_t off = (S << 24) | (i1 << 23) | (i2 << 22) | ((a & 0x3FF) << 12) | ((b & 0x7FF) << 1);
    if (S) off |= 0xFE000000;
    *tgt = addr + 4 + off; return 1;
}
static int dec_bl(const uint8_t *img, uint32_t addr, uint32_t *tgt) {    /* bl (T1) */
    uint16_t a = rd16s(img + (addr - IMG_BASE)), b = rd16s(img + (addr - IMG_BASE) + 2);
    if ((a & 0xF800) != 0xF000 || (b & 0xD000) != 0xD000) return 0;
    uint32_t S = (a >> 10) & 1, J1 = (b >> 13) & 1, J2 = (b >> 11) & 1, i1 = !(J1 ^ S), i2 = !(J2 ^ S);
    int32_t off = (S << 24) | (i1 << 23) | (i2 << 22) | ((a & 0x3FF) << 12) | ((b & 0x7FF) << 1);
    if (S) off |= 0xFE000000;
    *tgt = addr + 4 + off; return 1;
}

static int do_parse(void) {
    if (g_filesz < 20 || memcmp(g_file, "2CMU", 4)) { printf("%s: bad magic\n", opt.file); return 1; }
    uint32_t ver = rd32(g_file + 4), blocks = rd32(g_file + 8), load = rd32(g_file + 12), words = rd32(g_file + 16);
    size_t paylen = (size_t)words * 4;
    printf("== %s\n  header: version=0x%x blocks=%u load=0x%08x words=0x%x (%zu bytes payload) file=%zu bytes, trailing=%lld bytes\n",
           opt.file, ver, blocks, load, words, paylen, g_filesz, (long long)g_filesz - 20 - (long long)paylen);
    if (words > (g_filesz - 20) / 4) { printf("  payload exceeds file!\n"); return 1; }
    if (g_filesz > 20 + paylen) {
        const uint8_t *t = g_file + 20 + paylen; size_t n = g_filesz - 20 - paylen;
        printf("  trailer: first bytes"); for (size_t i = 0; i < 16 && i < n; i++) printf(" %02x", t[i]);
        printf("  strings:"); for (size_t i = 8; i + 4 < n && i < 120; i++) if (t[i] >= 32 && t[i] < 127) { size_t j = i; printf(" \""); while (j < n && t[j] >= 32 && t[j] < 127 && j - i < 24) putchar(t[j++]); printf("\""); i = j; if (j > 120) break; }
        printf("\n");
    }
    const uint8_t *img = g_file + 20;
    uint32_t vt[16]; for (int i = 0; i < 16; i++) vt[i] = rd32(img + 4 * i);
    printf("  vectors: SP=%08x reset=%08x NMI=%08x HardFault=%08x SVCall=%08x DebugMon=%08x PendSV=%08x SysTick=%08x\n", vt[0], vt[1], vt[2], vt[3], vt[11], vt[12], vt[14], vt[15]);
    /* SVCall chain */
    uint32_t sv = vt[11] & ~1u; uint32_t t1;
    if (sv < load || sv + 12 > load + paylen) printf("  SVCall handler %08x is outside the image\n", sv);
    else {
        const uint8_t *hp = img + (sv - load);
        printf("  SVCall @%08x: %04x %04x %04x %04x", sv, rd16s(hp), rd16s(hp + 2), rd16s(hp + 4), rd16s(hp + 6));
        if (rd16s(hp) == 0xBE00) printf("  => default bkpt trap: NO svc handler (image does not use svc)\n");
        else if (dec_bw(img, sv, &t1)) {
            /* dec_bw uses IMG_BASE-relative; image is loaded at IMG_BASE so this is right for load==IMG_BASE */
            printf("  => b.w %08x\n", t1);
            const uint8_t *h2p = img + (t1 - load);
            uint16_t a = rd16s(h2p), b = rd16s(h2p + 2), c2 = rd16s(h2p + 8);
            uint32_t tb;
            int pat = a == 0x4668 && b == 0xB510 && dec_bl(img, t1 + 4, &tb) && c2 == 0xBD10;
            printf("  uvm2_svc_handler @%08x: %04x %04x | %04x %04x | %04x\n", t1, a, b, rd16s(h2p + 4), rd16s(h2p + 6), c2);
            if (pat) printf("  => PATTERN HOLDS: mov r0,sp; push {r4,lr}; bl uvm2_svc_dispatch=%08x; pop {r4,pc}\n", tb);
            else printf("  => pattern does NOT hold\n");
        } else printf("  => not a b.w\n");
    }
    if (vt[12] && (vt[12] & ~1u) != (vt[11] & ~1u)) {
        uint32_t d = vt[12] & ~1u;
        if (d >= load && d + 8 <= load + paylen) printf("  vector 12 @%08x: %04x %04x %04x %04x (non-default)\n", d, rd16s(img + d - load), rd16s(img + d - load + 2), rd16s(img + d - load + 4), rd16s(img + d - load + 6));
    }
    /* raw svc scan: every aligned halfword 0xDFnn */
    for (int pass = 0; pass < 2; pass++) {
        size_t lim = pass == 0 ? (paylen < 65536 ? paylen : 65536) : paylen;
        int cnt[256] = {0}; size_t tot = 0;
        for (size_t i = 0; i + 2 <= lim; i += 2) { uint16_t h = rd16s(img + i); if ((h & 0xFF00) == 0xDF00) { cnt[h & 0xFF]++; tot++; } }
        printf("  raw 0xDFnn halfwords (%s): %zu;", pass == 0 ? "first 64 KB" : "whole image", tot);
        for (int i = 0; i < 256; i++) if (cnt[i]) printf(" #%d x%d", i, cnt[i]);
        printf("\n");
    }
    /* instruction-aligned sweep from the reset vector (approximate: literal pools desync it briefly) */
    uint32_t start = (vt[1] & ~1u) - load;
    struct { const char *n; int c; } cats[64]; int nc = 0;
    int svcn[256] = {0}; size_t svct = 0, ins = 0;
    for (size_t o = start; o + 2 <= paylen;) {
        uint16_t h1 = rd16s(img + o), h2 = 0; int len = t32(h1) ? 4 : 2;
        if (len == 4) { if (o + 4 > paylen) break; h2 = rd16s(img + o + 2); }
        char cb[96]; const char *cl = classify(h1, h2, len, cb, sizeof cb);
        ins++;
        if (cl && !strncmp(cl, "coproc", 6) && (((h2 >> 8) & 15) < 4 || ((h2 >> 8) & 15) > 7)) cl = NULL;   /* p0-p3, p8+ are almost always data mis-decoded; RCP is p4-p7 */
        if (cl) {
            if (!strncmp(cl, "svc", 3)) { svcn[h1 & 0xFF]++; svct++; }
            else {
                char key[64]; if (!strncmp(cl, "coproc", 6)) { sscanf(cl, "coproc %s", key); snprintf(cb, sizeof cb, "coproc %s", key); cl = cb; }
                int i; for (i = 0; i < nc; i++) if (!strcmp(cats[i].n, cl)) break;
                if (i == nc && nc < 64) { cats[nc].n = strdup(cl); cats[nc].c = 0; nc++; }
                if (i < nc) cats[i].c++;
            }
        }
        o += len;
    }
    printf("  sweep from reset (%zu insns): svc x%zu;", ins, svct);
    for (int i = 0; i < 256; i++) if (svcn[i]) printf(" #%d x%d", i, svcn[i]);
    printf("\n");
    for (int i = 0; i < nc; i++) printf("    %-70s x%d\n", cats[i].n, cats[i].c);
    return 0;
}

/* ------------------------------------------------------------------ main */
static void usage(void) {
    fprintf(stderr,
        "usage: um2run [options] image.um2\n"
        "  --parse            static census only (header, vectors, SVCall chain, opcode scan)\n"
        "  --mode ptrace|inproc   trap back-end (default ptrace; inproc needs --patch-svc, used for qemu)\n"
        "  --patch-svc        inproc: rewrite 0xDFnn halfwords to udf so svc is catchable without seccomp\n"
        "  --events N         stop after N events (default 200000)\n"
        "  --secs S           stop after S seconds (default 20)\n"
        "  --every N          progress line every N events (default 20000)\n"
        "  --top K            rows per summary table (default 30)\n"
        "  --log N            max first-occurrence log lines (default 400)\n"
        "  --quiet            no per-event first-occurrence lines\n"
        "  --no-psram         do not map 8 MB RAM at 0x11000000\n"
        "  --trace            print every trap (very verbose)\n"
        "  --no-prepatch      do not replace TT/MRS/MSR SYSm sites by UDF.W at load (then only caught if they fault)\n"
        "  --live             flush every log line (default: buffered, flushed every 2 s and at exit)\n"
        "  --rt N             run as SCHED_RR priority N (1..99); --cpu N pin to core N; --no-nice skip nice -20\n"
        "  --cpu1 N [--cpu1-after S]  pin core 1 to CPU N (after S s on the process CPU: black-screen guard)\n"
        "  --cpu1-normal      core 1 leaves real-time scheduling (SCHED_OTHER, nice -20) once on its own CPU\n"
        "  --settle S         sleep S s before starting (let Main finish its video setup)\n"
        "  --no-canned        reads return only what was written (else 0)\n"
        "  --bootrom FILE     the RP2350 boot ROM image (32 KB, bootrom-combined.bin from github.com/raspberrypi/pico-bootrom-rp2350)\n"
        "                     for images that read it as data (Centipede 0.3a: its noise); without it the area reads as zero\n"
        "  --bus              run the image for real: svc dispatch, RP2350 PIO/DMA/SIO device model, core 1 as a thread,\n"
        "                     bus traffic -> ptx stream (PTX_BACKEND=file|mister, see src/ptx_*.c); implies inproc + svc patching\n"
        "  --bus-verbose      model log on stderr (twice: every DMA run); --capture FILE records the model inputs for replay\n"
        "  --no-loading       no LOADING screen + progress bar while the image has not drawn its first frame (<image>.load keeps the total)\n"
        "  --message TEXT     no image: show TEXT on the Vectrex for 20 s ('|' = new line; 4 lines of 20 at most), then hand it back\n"
        "  --frame-cycles N   idle-pad each stream frame to N E cycles (default 30000, 0 = none)\n"
        "  --keys F:B,F:B     PC tests: from frame mark F on, controller reads (PSG reg 14) return byte B (0xFE = button 1, 0xFF = none)\n"
        "  --pot F:X1,Y1[,X2,Y2];F:..  PC tests: from frame mark F on, the stick pots read these signed values (through the sample/hold model)\n");
}

int main(int argc, char **argv) {
    um2_clock_init();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--parse")) opt.parse = 1;
        else if (!strcmp(a, "--mode") && i + 1 < argc) opt.ptrace_mode = !strcmp(argv[++i], "ptrace");
        else if (!strcmp(a, "--patch-svc")) opt.patch_svc = 1;
        else if (!strcmp(a, "--unsafe")) opt.unsafe = 1;
        else if (!strcmp(a, "--events") && i + 1 < argc) opt.max_events = strtoull(argv[++i], 0, 0);
        else if (!strcmp(a, "--secs") && i + 1 < argc) opt.secs = atof(argv[++i]);
        else if (!strcmp(a, "--every") && i + 1 < argc) opt.every = strtoull(argv[++i], 0, 0);
        else if (!strcmp(a, "--top") && i + 1 < argc) opt.top = atoi(argv[++i]);
        else if (!strcmp(a, "--log") && i + 1 < argc) opt.logmax = atoi(argv[++i]);
        else if (!strcmp(a, "--quiet")) opt.quiet = 1;
        else if (!strcmp(a, "--trace")) opt.trace = 1;
        else if (!strcmp(a, "--no-prepatch")) g_noprepatch = 1;
        else if (!strcmp(a, "--live")) g_live = 1;
        else if (!strcmp(a, "--rt") && i + 1 < argc) g_opt_rt = atoi(argv[++i]);
        else if (!strcmp(a, "--cpu") && i + 1 < argc) g_opt_cpu = atoi(argv[++i]);
        else if (!strcmp(a, "--no-nice")) g_opt_nonice = 1;
        else if (!strcmp(a, "--no-psram")) opt.no_psram = 1;
        else if (!strcmp(a, "--no-canned")) opt.no_canned = 1;
        else if (!strcmp(a, "--bus")) { opt.bus = 1; opt.ptrace_mode = 0; opt.patch_svc = 1; opt.max_events = 4000000000ull; opt.every = 5000000; opt.secs = 300; }
        else if (!strcmp(a, "--bus-verbose")) opt.bus_verbose++;
        else if (!strcmp(a, "--bus-timeout") && i + 1 < argc) g_bus_timeout = atof(argv[++i]);
        else if (!strcmp(a, "--frame-timeout") && i + 1 < argc) g_frame_timeout = atof(argv[++i]);
        else if (!strcmp(a, "--ringmon")) g_ringmon = 1;
        else if (!strcmp(a, "--ringmon-gap")) g_ringmon = 2;   /* also the longest gap between ring writes: a clock read per write, which slows the producer */
        else if (!strcmp(a, "--hold")) g_hold = 1;
        else if (!strcmp(a, "--no-loading")) g_noload = 1;
        else if (!strcmp(a, "--message") && i + 1 < argc) g_msg = argv[++i];
        else if (!strcmp(a, "--pot") && i + 1 < argc) { char *q = argv[++i]; while (*q && g_npot < 16) { g_potf[g_npot] = (unsigned)strtoul(q, &q, 0); if (*q == ':') q++; for (int k = 0; k < 4; k++) { g_potv[g_npot][k] = 0; if (*q && *q != ';') { g_potv[g_npot][k] = (int8_t)strtol(q, &q, 0); if (*q == ',') q++; } } g_npot++; if (*q == ';') q++; } }
        else if (!strcmp(a, "--keys") && i + 1 < argc) { char *q = argv[++i]; while (*q && g_nkeys < 16) { g_keyf[g_nkeys] = (unsigned)strtoul(q, &q, 0); if (*q == ':') q++; g_keyv[g_nkeys++] = (unsigned)strtoul(q, &q, 0); if (*q == ',') q++; } }
        else if (!strcmp(a, "--sd") && i + 1 < argc) g_sd_path = argv[++i];
        else if (!strcmp(a, "--bootrom") && i + 1 < argc) {
            const char *bp = argv[++i]; FILE *bf = fopen(bp, "rb");
            g_bootrom = malloc(0x8000 + 1);
            if (!bf || !g_bootrom || fread(g_bootrom, 1, 0x8000 + 1, bf) != 0x8000) { fprintf(stderr, "um2run: %s is not a 32 KB boot ROM image: the boot ROM area reads as zero\n", bp); free(g_bootrom); g_bootrom = NULL; }
            if (bf) fclose(bf);
        }
        else if (!strcmp(a, "--cpu1") && i + 1 < argc) g_cpu1 = atoi(argv[++i]);
        else if (!strcmp(a, "--settle") && i + 1 < argc) g_settle = atof(argv[++i]);
        else if (!strcmp(a, "--cpu1-after") && i + 1 < argc) g_cpu1_after = atof(argv[++i]);
        else if (!strcmp(a, "--cpu1-normal")) g_c1_normal = 1;
        else if (!strcmp(a, "--donate") && i + 1 < argc) g_donate_us = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--capture") && i + 1 < argc) opt.capture = argv[++i];
        else if (!strcmp(a, "--frame-cycles") && i + 1 < argc) opt.frame_cycles = (unsigned)strtoul(argv[++i], 0, 0);
        else if (a[0] == '-') { usage(); return 2; }
        else opt.file = a;
    }
    if (g_sd_path) { int rc = um2card_lock(g_sd_path); if (rc) return rc; }
    if (g_msg) return msg_main();
    if (!opt.file) { usage(); return 2; }
    if (g_settle > 0) { fprintf(stderr, "settle %.1f s\n", g_settle); struct timespec ts = { (time_t)g_settle, (long)((g_settle - (time_t)g_settle) * 1e9) }; nanosleep(&ts, NULL); }
    FILE *f = fopen(opt.file, "rb");
    if (!f) { perror(opt.file); return 1; }
    fseek(f, 0, SEEK_END); g_filesz = ftell(f); fseek(f, 0, SEEK_SET);
    if (g_filesz > sizeof g_file) { fprintf(stderr, "%s: too big (> 1 MiB)", opt.file); fputc(10, stderr); return 1; }
    if (fread(g_file, 1, g_filesz, f) != g_filesz) { perror("read"); return 1; }
    fclose(f);
    setvbuf(stdout, g_outbuf, g_live ? _IOLBF : _IOFBF, sizeof g_outbuf);
    if (opt.parse) return do_parse();
    if (g_filesz < 20 || memcmp(g_file, "2CMU", 4) || rd32(g_file + 12) != IMG_BASE) { fprintf(stderr, "not a 2CMU image loading at 0x20000000\n"); return 1; }
    uint32_t words = rd32(g_file + 16);
    if (words > (g_filesz - 20) / 4 || words > (IMG_TOP - IMG_BASE) / 4) { fprintf(stderr, "bad length\n"); return 1; }     /* no words * 4: it wraps for a malformed header */
    {   uint32_t ver = rd32(g_file + 4), blocks = rd32(g_file + 8), sp0 = words ? rd32(g_file + 20) : 0, rst = words > 1 ? rd32(g_file + 24) : 0;
        if (ver != 0x1 && ver != 0x101) fprintf(stderr, "um2run: header version 0x%x: made for a newer UVMC2 format, may not run\n", ver);    /* seen: 0x1, 0x101 (BattleZone 0.3a) */
        if (blocks != 1) fprintf(stderr, "um2run: header says %u blocks: only the first is loaded, may not run\n", blocks);
        if (sp0 < IMG_BASE || sp0 > 0x20082000u || (sp0 & 3) || !(rst & 1) || (rst & ~1u) < IMG_BASE || (rst & ~1u) >= IMG_BASE + words * 4) {    /* an Arm image starts with its vector table */
            fprintf(stderr, "um2run: %s has no Cortex-M vector table (SP %08x, reset %08x): not an Arm image (RISC-V UVMC2 images are not supported)\n", opt.file, sp0, rst);
            return 3;
        }
    }
    g_entry = rd32(g_file + 20 + 4) & ~1u;
    if (!g_noprepatch) prepatch(g_file + 20, words);
    if (!getenv("UM2_NOBTGT")) btgt_scan(g_file + 20, words);     /* UM2_NOBTGT=1 (test): only the local scan, as before */
    g_next_report = opt.every;
    setup_sched();
    printf("um2run: %s entry=%08x events<=%llu secs<=%.0f mode=%s\n", opt.file, g_entry, (unsigned long long)opt.max_events, opt.secs, opt.ptrace_mode ? "ptrace" : "inproc");
#ifdef __arm__
    return opt.ptrace_mode ? run_ptrace() : run_inproc();
#else
    fprintf(stderr, "dynamic mode needs an ARM build (only --parse works on this host)\n");
    return 2;
#endif
}

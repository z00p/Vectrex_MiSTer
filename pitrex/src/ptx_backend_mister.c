/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx_backend_mister.c - DE10-Nano backend: a command ring in the DDR3 that
 * the Vectrex core reads through its DDR arbiter. Same /dev/mem + mmap
 * approach as Main_MiSTer's shmem_map().
 *
 * Layout (byte offsets from PTX_DDR_BASE; see PROTOCOL.md):
 *   0x0000 ARM header : magic, version, log2 entries + session id (word 1; the core ignores
 *                       the id), wr_idx (u64), heartbeat
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PTX_DDR_BASE
#define PTX_DDR_BASE   0x34000000u
#endif
#ifndef PTX_DEVMEM
#define PTX_DEVMEM     "/dev/mem"             /* tests/ptx_wait_test.c builds with a plain file here and PTX_DDR_BASE=0 */
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
#define PTX_LEAD_FRAMES 2u                    /* frames the ARM may run ahead (um2run's mister_run.sh sets PTX_LEAD=1) */
#define PTX_SEQ_TIMEOUT_MS 200                /* core input counter must move at least this often */
#define PTX_NAME_RECHECK_MS 1000              /* re-read /tmp/CORENAME at least this often, changed or not */
#define PTX_NAME_STAT_MS 20                   /* stat() it at most this often (about once a frame) */

typedef struct {
	volatile uint32_t magic, version, log2n;
	volatile uint32_t session;               /* word 1 high half: our random id; the core never reads it */
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

volatile int ptx_boot_phase;   /* see ptx.h; defined for every platform */

static int memfd = -1;
static volatile uint8_t *base;
static arm_hdr_t *hdr;
static volatile int attached;     /* m_open finished: the audio thread may use the block (base, st and hdr appear one by one, milliseconds apart) */
static core_status_t *st;
static volatile ptx_input_t *in;
static volatile uint64_t *ring;
static volatile uint64_t *aud;         /* PCM ring */
static uint64_t aud_wr;                /* PCM words written */
static uint64_t aud_part;              /* samples collected for the next word */
static unsigned aud_nsamp;
static uint64_t wr, published, rd_cache;   /* rd_cache: last rd_idx seen (it only grows between re-attaches) */
static uint64_t frame_end[8];
static uint64_t pushed_cyc;             /* stream time of everything written to the ring (see m_push) */
static int cyc_off_ok;                  /* m_wait's offset between pushed_cyc and the core's core_cycles is known */
static uint32_t n_frames;
static int lead_f = -1;                 /* frames ahead: PTX_LEAD_FRAMES, or env PTX_LEAD (0-6) for tests */
/* Pacing statistics (ptx_pace_report): was the producer ahead of the core at the frame marks (it had to wait: the stream length, i.e. the
 * Vectrex, sets the speed) or behind (it never waited: the host sets the speed)?  Counters only, no clock reads. */
static unsigned long ps_frames, ps_waited, ps_naps, ps_sleeps, ps_fullnaps, ps_behind;   /* ps_sleeps: naps that were a predicted sleep (m_wait) */
static uint64_t ps_lead_sum, ps_lead_max;   /* commands written but not yet fetched by the core, at each frame mark */
static uint32_t hb;
static volatile sig_atomic_t core_lost;   /* set once we no longer own the block: never write again */
static uint32_t session;               /* random id written to the header; another value read back = another producer */
static uint32_t last_seq, last_rd_lo;
static long last_seq_ms, last_name_ms;
static struct stat name_st;            /* /tmp/CORENAME as last read: re-read only when this changes */
static int name_st_ok, name_ours;

/* Core ownership.
 *
 * The block at PTX_DDR_BASE is only ours while the Vectrex-PiTrex core is the loaded core;
 * after a core change another core may use that DDR region, and our /dev/mem mapping stays
 * valid regardless. User space cannot revoke a mapping, so the rule is: prove ownership
 * before every publish and at the start of every ring batch, and never write once it fails.
 * A check passes only if all of these hold:
 *   - /tmp/CORENAME (MiSTer Main writes it when it loads a core) names our core. It is stat()ed
 *     every 20 ms and re-read only when inode/mtime/size change, or every second;
 *   - the core's signature is still there and its input counter keeps moving (it stops when
 *     the FPGA is reconfigured or PiTrex Mode is turned off) and its read index never goes
 *     backwards (it restarts from zero on a reload);
 *   - the session id we put in the header is still there (otherwise another producer took over).
 *
 * What remains: Main reconfigures the FPGA before it updates /tmp/CORENAME, and the input
 * counter is only judged stalled after PTX_SEQ_TIMEOUT_MS, so for up to that long after the
 * old core has gone this process can still write: at most one ring batch (<= 32 entries) plus
 * one header word per check. scripts/pitrexd.sh also kills the program when CORENAME changes.
 * Both are best effort; only the core-loading lifecycle could close the window entirely. */

/* um2run supplies a cheaper clock (um2run/um2clock.c: the MiSTer kernel has no vDSO, so clock_gettime is a 1.3 us syscall) */
__attribute__((weak)) long ptx_clock_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static long now_ms(void) { return ptx_clock_ms(); }

/* Raw syscalls only, so the signal handler can use it too. Returns the name's length. */
static int read_corename(char *name, size_t sz)
{
	int fd = open("/tmp/CORENAME", O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 0;
	ssize_t n = read(fd, name, sz - 1);
	close(fd);
	if (n < 0) n = 0;
	while (n && (name[n - 1] == '\n' || name[n - 1] == '\r' || name[n - 1] == ' ')) n--;
	name[n] = 0;
	return (int)n;
}

static int name_is_ours(const char *name)
{
	return strcasecmp(name, "Vectrex-PiTrex") == 0 || strcmp(name, "VECTREX") == 0;
}

/* Rule 1 (no writes unless the Vectrex core is running): Main names the loaded core in
   /tmp/CORENAME. Uncached; used while waiting and by the signal handler. */
static int vectrex_core_loaded(void)
{
	char name[64];
	return read_corename(name, sizeof name) && name_is_ours(name);
}

/* The same, but only as expensive as a stat() unless the file changed. A file caught empty
   (Main truncates it before it writes the new name) is retried briefly, then counts as gone. */
static long last_stat_ms;
static int corename_ours(long t)
{
	/* stat() at most every PTX_NAME_STAT_MS (18 Nov 2026; it was every check, 1000 syscalls a second).  A rename is now seen up to 20 ms after
	 * Main writes it, not 1 ms; Main writes it only after reconfiguring the FPGA, and the input counter's PTX_SEQ_TIMEOUT_MS (200 ms) already
	 * bounds the window after that (see "What remains" above). */
	if (name_st_ok && t - last_stat_ms < PTX_NAME_STAT_MS && t >= last_stat_ms) return name_ours;
	last_stat_ms = t;
	struct stat s;
	if (stat("/tmp/CORENAME", &s) != 0) { name_st_ok = 0; return 0; }
	int same = name_st_ok && s.st_ino == name_st.st_ino && s.st_size == name_st.st_size &&
	           s.st_mtim.tv_sec == name_st.st_mtim.tv_sec && s.st_mtim.tv_nsec == name_st.st_mtim.tv_nsec;
	if (same && t - last_name_ms < PTX_NAME_RECHECK_MS) return name_ours;
	int ours = vectrex_core_loaded();
	for (int i = 0; !ours && i < 5; i++) {
		char name[64];
		if (read_corename(name, sizeof name)) break;   /* a real, different name */
		struct timespec ts = { 0, 2 * 1000000L };
		nanosleep(&ts, NULL);
		ours = vectrex_core_loaded();
	}
	if (stat("/tmp/CORENAME", &name_st) == 0) name_st_ok = 1;
	name_ours = ours;
	last_name_ms = t;
	return ours;
}

/* The core writes its status block (signature "PTXC", input sequence counter)
   only while PiTrex Mode is on; a moving counter proves it is live right now,
   which stale RAM cannot. */
static int core_signature(void) { return st->build_magic == 0x43585450u; }

/* B7 cores count every dropped takeover (reset, timeout, hand-back) in word 11 bits 63..48.
   A change means the core reset the machine and stopped listening to us; it will not take
   over again until it has seen the magic cleared. Older cores keep it at 0. */
static uint16_t epoch0;
static long     attach_ms;
static int      epoch_changed;
static uint16_t core_epoch(void) { return (uint16_t)(st->build >> 16); }

/* 1 while we still own the block; otherwise 0 and *why says what failed. No exit, no writes. */
static int still_ours(const char **why)
{
	if (core_lost) { *why = "ownership already lost"; return 0; }
	long t = now_ms();
	if (!corename_ours(t))              { *why = "another core was loaded"; return 0; }
	if (!core_signature())              { *why = "the core's signature is gone (PiTrex Mode off?)"; return 0; }
	uint32_t seq = in->seq;
	if (seq != last_seq) { last_seq = seq; last_seq_ms = t; }
	if (t - last_seq_ms >= PTX_SEQ_TIMEOUT_MS) { *why = "the core's input counter stopped"; return 0; }
	uint32_t rd_lo = (uint32_t)st->rd_idx;
	if ((int32_t)(rd_lo - last_rd_lo) < 0) { *why = "the core's read index went backwards (reloaded)"; return 0; }
	last_rd_lo = rd_lo;
	if (hdr->session != session)        { *why = "another producer took over the header"; return 0; }
	if (core_epoch() != epoch0) {
		static char msg[96];         /* the drop cause (pitrex_reset: 0 video_reset 1 status0 2 OSD reset 3 rom_download 4 save_hold 5 delayed_reset 6 bios; bit 7 = arbiter reset; none = heartbeat timeout) */
		snprintf(msg, sizeof msg, "the core was reset (back to the BIOS; epoch %u->%u, cause 0x%02x)", epoch0, core_epoch(), (unsigned)((st->build >> 8) & 0xFF));
		epoch_changed = 1; *why = msg; return 0; }
	return 1;
}

static long last_chk_ms;
static void check_core_alive(void)
{
	const char *why;
	/* at most once per millisecond: the ownership test costs a stat() plus several uncached DDR reads, and a producer that publishes a
	 * 32-command batch every ~30 us (UVMC2 images) spent most of its time here.  The window it leaves is about 1 ms of stream. */
	{ long t = now_ms(); if (t == last_chk_ms && !core_lost) return; last_chk_ms = t; }
	if (still_ours(&why)) return;
	/* B7 also drops the takeover when Main downloads a file into the core, and an MGL menu entry
	   sends the overlay and the .ptx a second or two after the core loads. Early on, attach again
	   instead of exiting; later, an epoch change is a real reset and we hand the Vectrex back. */
	static int reattached;
	if (epoch_changed && hdr && reattached < 4 && (now_ms() - attach_ms < 8000 || n_frames == 0 || ptx_boot_phase)) {   /* n_frames == 0: no picture yet, a slow boot (27 Oct 2026) must not turn a start-up drop into a fatal reset; ptx_boot_phase: same while a launcher shows its own loading frames */
		reattached++;
		hdr->magic = 0;
		__sync_synchronize();
		{ struct timespec ts = { 0, 200000L }; nanosleep(&ts, NULL); }
		epoch0 = core_epoch();
		epoch_changed = 0;
		wr = published = rd_cache = st->rd_idx;
		cyc_off_ok = 0;              /* the core counts its stream time from zero again */
		n_frames = 0;                /* frame_end[] belongs to the old ring position: pacing on it would wait for ever (it is only read for n_frames > PTX_LEAD_FRAMES, after being rewritten) */
		last_seq_ms = now_ms();      /* a fresh takeover: the core gets a full PTX_SEQ_TIMEOUT_MS to move its input counter again (review item 5, 27 Oct 2026) */
		hdr->wr_idx = wr;
		last_rd_lo = (uint32_t)wr;
		hdr->hb_lo = ++hb;
		hdr->hb_hi = ~hb ^ PTX_MAGIC;
		__sync_synchronize();
		hdr->magic = PTX_MAGIC;
		fprintf(stderr, "ptx: the core dropped the takeover during start-up (a file load?) - attached again\n");
		return;
	}
	if (epoch_changed && hdr) {      /* still our core and our block: release it so it can attach again */
		hdr->magic = 0;
		__sync_synchronize();
	}
	core_lost = 1;
	fprintf(stderr, "ptx: %s - stopping (no further writes)\n", why);
	exit(3);
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

static void publish_raw(void)
{
	__sync_synchronize();           /* commands visible before the index */
	hdr->wr_idx = wr;
	published = wr;
}

static void publish(void)
{
	check_core_alive();             /* never publish into a block we may no longer own */
	publish_raw();
}

/* Ctrl-C, kill or a dropped SSH session: clear the magic so the core hands back at once
   (a real crash is still caught by the 100 ms heartbeat timeout) - but only if we can still
   prove, right now and with async-signal-safe calls, that the block is ours. If the core has
   changed (the supervisor in pitrexd.sh sends SIGTERM exactly then) we leave it untouched. */
static void on_signal(int sig)
{
	(void)sig;
	if (!core_lost && hdr && hdr->session == session && vectrex_core_loaded() && core_signature())
		hdr->magic = 0;
	_exit(0);
}

static uint32_t new_session(void)
{
	uint32_t v = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) { if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = 0; close(fd); }
	if (!v) v = (uint32_t)now_ms() * 2654435761u ^ (uint32_t)getpid();
	return v ? v : 1;
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
	memfd = open(PTX_DEVMEM, O_RDWR | O_SYNC | O_CLOEXEC);
	if (memfd < 0) { perror("ptx: " PTX_DEVMEM); return -1; }
	void *p = mmap(0, PTX_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, PTX_DDR_BASE);
	if (p == MAP_FAILED) { perror("ptx: mmap"); close(memfd); return -1; }
	base = p;
	st   = (core_status_t *)(base + 0x0040);
	in   = (volatile ptx_input_t *)(base + 0x0080);
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
	long t = now_ms();
	last_seq = in->seq; last_seq_ms = t;
	last_rd_lo = (uint32_t)st->rd_idx;
	name_st_ok = 0;
	if (!corename_ours(t)) {         /* the core changed between the signature and now */
		fprintf(stderr, "ptx: the Vectrex core was unloaded\n");
		munmap((void *)base, PTX_MAP_SIZE); base = NULL; close(memfd); return -1;
	}
	session = new_session();
	hdr  = (arm_hdr_t *)(base + 0x0000);
	ring = (volatile uint64_t *)(base + 0x1000);
	aud  = (volatile uint64_t *)(base + 0x1000 + PTX_RING_N * 8u);
	/* Handlers first: they only touch the block once session is in the header (and then only
	   if ownership still holds), so a signal during setup cannot leave a stray magic. */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	hdr->magic = 0;                  /* a B7 core attaches only after seeing the magic clear */
	__sync_synchronize();
	{                                /* the core polls the header every ~2 us; a plain sleep, as */
		struct timespec ts = { 0, 100000L };   /* nap_us() would run the ownership checks */
		nanosleep(&ts, NULL);        /* before our session id is in the header */
	}
	hdr->session = session;          /* word 1 high half: ignored by the core, read back by every check */
	wr = published = st->rd_idx;     /* resume wherever the core is */
	hdr->version = PTX_VERSION;
	hdr->log2n = PTX_RING_LOG2;
	hdr->wr_idx = wr;
	aud_wr = st->aud_rd & (2u * PTX_AUD_N - 1);   /* B6 cores: start the PCM ring where the core is */
	aud_nsamp = 0;
	hdr->aud_wr = aud_wr;
	epoch0 = core_epoch();
	attach_ms = now_ms();
	hb_tick();
	__sync_synchronize();
	hdr->magic = PTX_MAGIC;          /* with a moving coded heartbeat: the core takes over */
	attached = 1;
	return 0;
}

static void m_push(uint64_t cmd)
{
	if (wr == published) check_core_alive();   /* start of a ring batch: the ring write below is a DDR write */
	if (wr - rd_cache >= PTX_RING_N - PTX_PUBLISH) {      /* rd_idx only ever grows: re-read the (uncached) DDR word only when the cached one says full */
		rd_cache = st->rd_idx;
		while (wr - rd_cache >= PTX_RING_N - PTX_PUBLISH) {   /* ring full: wait */
			if (published != wr) publish();
			ps_fullnaps++;
			nap_us(50);
			rd_cache = st->rd_idx;
		}
	}
	ring[wr & (PTX_RING_N - 1)] = cmd;
	wr++;
	{ uint32_t op = PTX_CMD_OP(cmd);     /* the stream time written, counted as the player counts it (pitrex_player.sv): a bus op is delta + 1 E cycles, a WAIT delta */
	  if (op == PTX_OP_WRITE || op == PTX_OP_READ) pushed_cyc += PTX_CMD_DELTA(cmd) + 1u; else if (op == PTX_OP_WAIT) pushed_cyc += PTX_CMD_DELTA(cmd); }
	if (wr - published >= PTX_PUBLISH) publish();
}

static uint64_t pend_target, pend_wr, pend_cyc; static int pend;   /* the wait m_frame leaves to m_wait */
static uint64_t frame_cyc[8];          /* pushed_cyc at each frame_end[] */
/* One sleep per frame wait (18 Nov 2026, review D3; it napped 200 us at a time, each nap a syscall plus the ownership check).  The core's
 * core_cycles counts the stream time it has played since it took over, so it is pushed_cyc (counted the same way) less an offset: the takeover
 * point and the few commands its FIFO holds.  The offset is learnt where the two meet, when a short poll sees rd_idx pass a frame end.
 * Remaining wait = (frame end - core time) x 2/3 us.  Not the producer's stream_cycles: that includes time it moved on without writing yet
 * (a PiTrex program's T2 wait), which the core only sees with the next command. */
static int64_t cyc_off;
static void m_frame(uint64_t stream_cycles)
{
	(void)stream_cycles;
	publish();                       /* checks ownership first */
	hb_tick();
	/* Stay at most PTX_LEAD_FRAMES frames ahead of what the core has fetched
	   (its FIFO is tiny, so fetched ~ played): enough slack for Linux
	   scheduling, little enough that input still feels immediate. */
	frame_end[n_frames % 8] = wr;
	frame_cyc[n_frames % 8] = pushed_cyc;
	n_frames++;
	if (lead_f < 0) { const char *e = getenv("PTX_LEAD"); lead_f = e && *e >= '0' && *e <= '6' ? *e - '0' : (int)PTX_LEAD_FRAMES; }
	if (n_frames <= (unsigned)lead_f) return;
	pend_target = frame_end[(n_frames - 1 - lead_f) % 8]; pend_cyc = frame_cyc[(n_frames - 1 - lead_f) % 8]; pend_wr = wr; pend = 1;
}

/* How long to sleep before polling: the predicted time to the frame end less a margin for the core_cycles snapshot (it is refreshed every 4096
 * of the core's clocks) and the offset's own error (one poll).  0: poll (offset unknown, nearly there, or a prediction that makes no sense). */
#define PTX_WAIT_MARGIN_US 500
static long wait_sleep_us(uint64_t target_cyc)
{
	uint64_t cc = st->core_cycles;
#ifdef PTX_WAIT_POLL_ONLY
	cyc_off_ok = 0;                          /* tests/ptx_wait_test.c: the old 200 us polling, for comparison */
#endif
	if (!cyc_off_ok || !cc) return 0;        /* 0: a core that does not report it (or has not started) */
	int64_t left = (int64_t)target_cyc - ((int64_t)cc + cyc_off);   /* E cycles */
	long us = (long)(left * 2 / 3) - PTX_WAIT_MARGIN_US;
	if (us < 200) return 0;
	return us > 20000 ? 20000 : us;  /* a frame at most per sleep: the heartbeat and the ownership check run after each */
}

/* The pacing wait for the last m_frame, apart from it (18 Nov 2026): um2run publishes the frame under its bus lock and
 * waits here only after releasing it, as core 1 waits on the PIO on a real cart while core 0, the DMA and the timer go on.
 * Holding the lock across this wait blocked core 0's next DMA access for the whole wait, so in a Pause its timer
 * interrupts stopped and were replayed afterwards.
 *
 * Nothing here decides that the core is paused (18 Nov 2026, review A10).  The core's Pause button stops the machine, so neither rd_idx nor
 * core_cycles moves: the prediction then stays the same and the wait sleeps it in chunks of at most 20 ms until the core plays again, and
 * without a prediction the polls back off from 200 us to 2 ms while nothing moves, and go back to 200 us when anything does.  This thread
 * just sleeps; the rest of the program runs on, as the cart's other core does on a stopped console.  (Until then a "stalled" flag, set after
 * 2 s, made the wait nap 2 ms and parked the other guest threads in 2 ms naps: that stopped the game's timer interrupts again on the MiSTer.) */
static void m_wait(void)
{
	if (!pend) return;
	pend = 0;
	uint64_t target = pend_target, wr = pend_wr, target_cyc = pend_cyc;
	long wait0_ms = 0, last_us = 0, poll_us = 200, moved_ms;
	uint64_t seen_rd = 0, seen_cc = 0; int told = 0;
	{ uint64_t rd = st->rd_idx; uint64_t lead = wr > rd ? wr - rd : 0;
	  ps_frames++; ps_lead_sum += lead; if (lead > ps_lead_max) ps_lead_max = lead;
	  if (rd < target) { ps_waited++; wait0_ms = now_ms(); } else ps_behind++; }
	moved_ms = wait0_ms;
	for (;;) {
		uint64_t rd = st->rd_idx, cc = st->core_cycles;
		if (rd >= target) break;
		if (rd != seen_rd || cc != seen_cc) { seen_rd = rd; seen_cc = cc; poll_us = 200; moved_ms = now_ms(); }
		else if (poll_us < 2000) poll_us *= 2;
		ps_naps++;
		long us = wait_sleep_us(target_cyc);
		if (us) ps_sleeps++;
		last_us = us ? us : poll_us;
		nap_us(last_us);
		if (!told && now_ms() - moved_ms >= 2000) {   /* a log line only: nothing changes behaviour on it */
			fprintf(stderr, "ptx: the core has fetched nothing for 2 s (rd_idx %llu < %llu) - its Pause button? PiTrex Mode off?\n",
			        (unsigned long long)rd, (unsigned long long)target);
			told = 1;
		}
	}
	/* caught within one short poll: the core is at this frame end now, so learn (or refresh) the offset between the two stream times */
	if (wait0_ms && last_us == 200) { uint64_t cc = st->core_cycles; if (cc) { cyc_off = (int64_t)target_cyc - (int64_t)cc; cyc_off_ok = 1; } }
	if (told) fprintf(stderr, "ptx: the core is fetching again after %ld s\n", (now_ms() - wait0_ms) / 1000);
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
	const char *why;
	if (hdr && !core_lost) {
		if (still_ours(&why)) {
			publish_raw();
			hdr->magic = 0;              /* core hands back to the 6809 at once */
		} else {
			core_lost = 1;               /* not ours any more (or a newer producer is): leave the block alone */
		}
	}
	munmap((void *)base, PTX_MAP_SIZE);
	close(memfd);
	base = NULL;
}

/* Heartbeat only, from a helper thread (um2run: the guest can be busy for seconds between bus operations).  Ownership checks
 * stay on the producing thread (publish/nap), so nothing here races with the reattach logic. */
static void m_poll(void)
{
	if (core_lost || !hdr) return;
	hb_tick();
}

const ptx_backend_t ptx_backend_mister = { m_open, m_push, m_frame, m_input, m_status, m_close, m_poll, m_wait };

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
	return attached && base && !core_lost && st->build_magic == 0x43585450u && (st->build & 0xFF) >= 6;
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
	check_core_alive();              /* the PCM ring and aud_wr are DDR writes too */
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

/* One line for um2run's summary: the core's own counters plus ours (see ps_* above).  Returns the length, 0 when there is nothing to say. */
int ptx_pace_report(char *buf, unsigned n)
{
	if (!base || !st || !ps_frames) return 0;       /* base: st points into the block, which m_close unmaps */
	return snprintf(buf, n, "ptx pacing: %lu frame marks, producer ahead by more than %u frames at %lu (waited %lu naps, %lu of them predicted sleeps), not ahead at %lu; commands unfetched at a mark: avg %.0f max %llu; ring-full naps %lu; core frames %u underruns %u",
	                ps_frames, lead_f < 0 ? PTX_LEAD_FRAMES : (unsigned)lead_f, ps_waited, ps_naps, ps_sleeps, ps_behind, (double)ps_lead_sum / (double)ps_frames, (unsigned long long)ps_lead_max, ps_fullnaps, st->frames, st->underruns);
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
int      ptx_pace_report(char *buf, unsigned n) { (void)buf; (void)n; return 0; }
uint32_t ptx_audio_space(void) { return 0; }
void     ptx_audio_write(const int16_t *s, uint32_t n) { (void)s; (void)n; }
#endif

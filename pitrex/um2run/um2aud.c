/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2aud.c -- see um2aud.h.  Resampler (windowed sinc, polyphase) + jitter buffer + .wav dump for the cart's I2S audio.
 * One producer (the pacing thread in um2bus.c calls put); pump runs on that thread (bus lock try-locked) or on the guest's frame mark (bus lock
 * held); g_sink_m guards the queue and the resampler, g_wav_m the .wav.
 */
#define _GNU_SOURCE
#include "um2aud.h"
#include "ptx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

#define OUT_HZ 32000u
#define QCAP   (1u << 15)                 /* 32 kHz samples waiting for the core (1 s) */
#define NPH    64                         /* polyphase sub-positions */
#define MAXT   256                        /* kernel taps */

static int g_on, g_log;
static int (*g_trylock)(void); static void (*g_unlock)(void);
static uint32_t g_pre_ms = 60;
static FILE *g_wav; static uint64_t g_wav_n; static pthread_mutex_t g_wav_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sink_m = PTHREAD_MUTEX_INITIALIZER;     /* the 32 kHz queue and resampler: put() from the pacing thread, pump() from it or from the guest's frame mark */
static int16_t g_q[QCAP]; static uint32_t g_qh, g_qn;                 /* output queue (32 kHz) */
static struct { uint64_t in, out, core, dropped, padded, gaps, pumps, busy, notready, resets; int16_t peak; } S;

/* ---- resampler: arbitrary rate -> 32 kHz */
static uint32_t g_rate;                    /* current input rate (0 = none) */
static double g_r;                         /* input samples per output sample */
static int g_H, g_T;                       /* kernel half width / taps */
static float g_tab[NPH][MAXT];
static float *g_in; static uint32_t g_incap, g_inn;                  /* input history, g_in[0] is absolute index g_base */
static uint64_t g_base; static double g_pos;                         /* g_pos = absolute input position of the next output sample */

static void rs_reset(void) { g_inn = 0; g_base = 0; g_pos = 0; if (g_rate) { /* H zeros ahead: the first output is centred on sample 0 */ } }
static void rs_setup(uint32_t rate) {
	g_rate = rate; g_r = (double)rate / OUT_HZ;
	double fc = (g_r > 1.0 ? 1.0 / g_r : 1.0) * 0.93;                 /* cutoff, relative to the input Nyquist */
	double W = 7.0 / fc;                                                /* 7 zero crossings of the cut-off sinc to each side */
	g_H = (int)ceil(W) + 1; if (g_H > MAXT / 2 - 1) g_H = MAXT / 2 - 1; g_T = 2 * g_H + 1;
	for (int ph = 0; ph < NPH; ph++) {
		double fr = (double)ph / NPH, sum = 0;
		for (int t = 0; t < g_T; t++) {
			double x = (t - g_H) - fr;                                  /* input offset of tap t from the output position */
			double s = x == 0 ? fc : sin(M_PI * fc * x) / (M_PI * x);
			double u = x / (g_H + 1.0), w = fabs(u) >= 1 ? 0 : 0.42 + 0.5 * cos(M_PI * u) + 0.08 * cos(2 * M_PI * u);   /* Blackman */
			g_tab[ph][t] = (float)(s * w); sum += s * w;
		}
		for (int t = 0; t < g_T; t++) g_tab[ph][t] = (float)(g_tab[ph][t] / sum);        /* unity gain at DC for every phase */
	}
	rs_reset(); S.resets++;
}
/* The jack DAC is AC coupled; BattleZone's buffers sit at a large negative offset (about -20000), which would eat the mixer's headroom: one-pole high pass, 7 Hz. */
static float g_hx, g_hy; static int g_hinit = 1;
static int16_t hp(int16_t v) {
	if (g_hinit) { g_hinit = 0; g_hx = v; g_hy = 0; return 0; }
	g_hy = (float)v - g_hx + 0.99863f * g_hy; g_hx = v;
	long r = lrintf(g_hy); return (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}
static void q_push(int16_t v) {
	if (v > S.peak) S.peak = v; else if (-v > S.peak) S.peak = (int16_t)-v;
	S.out++;
	if (g_wav) { pthread_mutex_lock(&g_wav_m); if (g_wav) { fwrite(&v, 2, 1, g_wav); g_wav_n++; } pthread_mutex_unlock(&g_wav_m); }   /* the .wav gets everything */
	if (g_qn >= QCAP) { S.dropped++; return; }
	g_q[(g_qh + g_qn++) & (QCAP - 1)] = v;
}
static void rs_run(const int16_t *s, uint32_t n) {
	if (g_rate == OUT_HZ) { for (uint32_t i = 0; i < n; i++) q_push(hp(s[i])); return; }
	if (g_inn + n + (uint32_t)g_T + 8 > g_incap) { g_incap = g_inn + n + (uint32_t)g_T + 4096; g_in = realloc(g_in, g_incap * sizeof *g_in); if (!g_in) { g_incap = 0; return; } }
	if (g_inn == 0 && g_base == 0) { for (int i = 0; i < g_H; i++) g_in[g_inn++] = 0; g_base = (uint64_t)0 - (uint64_t)g_H; }   /* zero history before sample 0 */
	for (uint32_t i = 0; i < n; i++) g_in[g_inn++] = (float)s[i];
	for (;;) {
		double fl = floor(g_pos); int64_t ip = (int64_t)fl;
		if ((int64_t)g_base + (int64_t)g_inn <= ip + g_H) break;                 /* need samples up to ip + H */
		int ph = (int)((g_pos - fl) * NPH + 0.5); if (ph >= NPH) { ph = 0; ip++; if ((int64_t)g_base + (int64_t)g_inn <= ip + g_H) break; }
		const float *k = g_tab[ph], *x = g_in + (ip - g_H - (int64_t)g_base);
		float a = 0; for (int t = 0; t < g_T; t++) a += k[t] * x[t];
		long v = lrintf(a); q_push(hp((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v)));
		g_pos += g_r;
	}
	/* forget history older than H + 1 samples before the next output */
	int64_t keep = (int64_t)floor(g_pos) - g_H - 1; int64_t drop = keep - (int64_t)g_base;
	if (drop > 0 && drop <= (int64_t)g_inn) { memmove(g_in, g_in + drop, (g_inn - (uint32_t)drop) * sizeof *g_in); g_inn -= (uint32_t)drop; g_base += (uint64_t)drop; }
}

/* ---- .wav */
static void wav_hdr(FILE *f, uint32_t datalen) {
	uint8_t h[44]; memcpy(h, "RIFF", 4); uint32_t v = 36 + datalen; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
	v = 16; memcpy(h + 16, &v, 4); uint16_t w = 1; memcpy(h + 20, &w, 2); memcpy(h + 22, &w, 2); v = OUT_HZ; memcpy(h + 24, &v, 4);
	v = OUT_HZ * 2; memcpy(h + 28, &v, 4); w = 2; memcpy(h + 32, &w, 2); w = 16; memcpy(h + 34, &w, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &datalen, 4);
	fwrite(h, 1, 44, f);
}
static void wav_patch(void) {
	pthread_mutex_lock(&g_wav_m);
	if (g_wav) { long p = ftell(g_wav); fflush(g_wav); fseek(g_wav, 0, SEEK_SET); wav_hdr(g_wav, (uint32_t)(g_wav_n * 2)); fseek(g_wav, p, SEEK_SET); fflush(g_wav); }
	pthread_mutex_unlock(&g_wav_m);
}
void um2aud_finish(void) {
	pthread_mutex_lock(&g_wav_m);
	FILE *f = g_wav; g_wav = NULL;
	if (f) { fflush(f); fseek(f, 0, SEEK_SET); wav_hdr(f, (uint32_t)(g_wav_n * 2)); fclose(f); }
	pthread_mutex_unlock(&g_wav_m);
}

void um2aud_init(int (*trylock)(void), void (*unlock)(void)) {
	static int done; if (done) return; done = 1;
	g_on = getenv("UM2_NOAUDIO") == NULL; g_log = getenv("UM2_AUDLOG") != NULL;
	g_trylock = trylock; g_unlock = unlock;
	{ const char *e = getenv("UM2_AUDPRE"); if (e) { int v = atoi(e); g_pre_ms = v < 0 ? 0 : v > 90 ? 90 : (uint32_t)v; } }
	const char *w = getenv("UM2_WAV");
	if (g_on && w && *w) { g_wav = fopen(w, "wb"); if (g_wav) { wav_hdr(g_wav, 0); atexit(um2aud_finish); } else perror(w); }
}
int um2aud_enabled(void) { return g_on; }

void um2aud_put(const int16_t *s, uint32_t n, uint32_t rate, uint64_t gap_ns) {
	if (!g_on || !n) return;
	pthread_mutex_lock(&g_sink_m);
	if (rate != g_rate) rs_setup(rate);
	if (gap_ns > 1000000ull) {                         /* a stall of the transfer: silence in the stream, filter history is stale */
		uint64_t z = gap_ns * OUT_HZ / 1000000000ull; S.gaps++; S.padded += z;
		if (z > OUT_HZ) z = OUT_HZ;
		rs_reset(); g_hinit = 1;
		while (z--) q_push(0);
	}
	S.in += n;
	rs_run(s, n);
	pthread_mutex_unlock(&g_sink_m);
}

/* Waits for a pump that is inside ptx_audio_write -- but not for ever: this runs from the exit paths (SIGALRM handler: time limit, no frame, SIGTERM), which may have interrupted the very
 * thread that holds g_sink_m (28 Oct 2026: um2run hung at exit on the MiSTer, six threads in on_alarm -> bus_finish -> um2aud_stop, kill -9 needed). */
void um2aud_stop(void) {
	for (int i = 0; i < 100; i++) {
		if (!pthread_mutex_trylock(&g_sink_m)) { g_on = 0; pthread_mutex_unlock(&g_sink_m); return; }
		struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
	}
	g_on = 0;
}

void um2aud_pump(int have_bus) {
	if (!g_on) return;
	static unsigned wavtick; if (!have_bus && g_wav && ++wavtick >= 500) { wavtick = 0; wav_patch(); }
	if (!g_qn) return;                                                      /* (racy read: only a hint) */
	if (!have_bus) {
		if (!ptx_audio_ready()) {                                           /* PC / old core: nothing to feed, the .wav has it all */
			pthread_mutex_lock(&g_sink_m); S.notready++; g_qh = (g_qh + g_qn) & (QCAP - 1); g_qn = 0; pthread_mutex_unlock(&g_sink_m); return; }
		if (g_trylock && g_trylock()) { S.busy++; return; }                 /* the guest holds the bus lock (frame pacing): next tick, or its frame mark pumps */
	}
	pthread_mutex_lock(&g_sink_m);
	if (!g_on) { pthread_mutex_unlock(&g_sink_m); if (!have_bus && g_unlock) g_unlock(); return; }
	S.pumps++;
	static int primed; static uint64_t lastpad;
	if (!ptx_audio_ready()) { primed = 0; g_qh = (g_qh + g_qn) & (QCAP - 1); g_qn = 0; }
	else {
		uint32_t sp = ptx_audio_space();
		int32_t lvl = sp ? 3200 - (int32_t)sp : 3200;                       /* samples queued in the core (ptx keeps up to 3200) */
		int16_t z[512]; memset(z, 0, sizeof z);
		int32_t pre = (int32_t)(g_pre_ms * OUT_HZ / 1000), pad = 0;
		if (!primed) { primed = 1; pad = pre - lvl; }                       /* first data: run ahead by the jitter buffer, with silence */
		else if (lvl < (int32_t)(OUT_HZ / 125) && S.pumps - lastpad > 125) { pad = pre - lvl; lastpad = S.pumps; if (g_log) fprintf(stderr, "um2aud: core queue ran dry (%d samples), re-priming\n", (int)lvl); }
		while (pad > 0 && sp) { uint32_t k = (uint32_t)pad > 512 ? 512 : (uint32_t)pad; if (k > sp) k = sp; ptx_audio_write(z, k); pad -= (int32_t)k; sp -= k; lvl += (int32_t)k; S.padded += k; }
		/* too far ahead (the core consumes slower than the stream arrives): drop the oldest */
		int32_t over = lvl + (int32_t)g_qn - (pre + (int32_t)(OUT_HZ * 60 / 1000));
		if (over > 0) { uint32_t d = (uint32_t)over > g_qn ? g_qn : (uint32_t)over; g_qh = (g_qh + d) & (QCAP - 1); g_qn -= d; S.dropped += d; }
		while (g_qn && sp) {
			int16_t buf[512]; uint32_t k = g_qn > 512 ? 512 : g_qn; if (k > sp) k = sp;
			for (uint32_t i = 0; i < k; i++) buf[i] = g_q[(g_qh + i) & (QCAP - 1)];
			ptx_audio_write(buf, k); g_qh = (g_qh + k) & (QCAP - 1); g_qn -= k; sp -= k; S.core += k;
		}
	}
	pthread_mutex_unlock(&g_sink_m);
	if (!have_bus && g_unlock) g_unlock();
}

void um2aud_stats(FILE *f) {
	if (!S.in && !S.out) return;
	fprintf(f, "um2aud: %llu samples in (rate %u Hz), %llu out at 32 kHz (peak %d), to the core %llu, silence added %llu, dropped %llu, stalls %llu; pumps %llu (bus busy %llu, core not ready %llu)%s\n",
		(unsigned long long)S.in, g_rate, (unsigned long long)S.out, S.peak, (unsigned long long)S.core, (unsigned long long)S.padded, (unsigned long long)S.dropped,
		(unsigned long long)S.gaps, (unsigned long long)S.pumps, (unsigned long long)S.busy, (unsigned long long)S.notready, g_wav ? " [wav]" : "");
}

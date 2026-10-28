/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2aud -- sink for the cart's own audio (the I2S jack output of a UVMC2 image).  See DESIGN.md, "Cart audio".
 *
 * um2bus.c recovers the sample stream from the paced DMA into PIO2 and hands it over here: mono 16-bit at the I2S
 * frame rate of the image (32 kHz for the SDK module, 192 kHz in BattleZone).  This file resamples to the 32 kHz
 * mono PCM the core's mixer takes (ptx_audio_write), keeps a small jitter buffer ahead of it and, on the PC, dumps
 * the same stream to a .wav (UM2_WAV=file) so it can be listened to.
 *
 * Environment: UM2_NOAUDIO=1 off (the model then behaves as before: DMA words are dropped), UM2_WAV=file.wav,
 * UM2_AUDPRE=<ms> jitter buffer in the core's PCM queue (default 60, 100 ms is the most ptx_audio_space allows).
 */
#ifndef UM2AUD_H
#define UM2AUD_H
#include <stdint.h>
#include <stdio.h>

void um2aud_init(int (*trylock)(void), void (*unlock)(void));   /* trylock: taken around every ptx_audio_* call (0 = got it); NULL = no lock */
int  um2aud_enabled(void);
/* n mono samples at `rate` Hz; gap_ns = silence that preceded the first one (the previous block ended that long ago) */
void um2aud_put(const int16_t *s, uint32_t n, uint32_t rate, uint64_t gap_ns);
void um2aud_pump(int have_bus);  /* push what is queued to the core (non-blocking); have_bus = the caller already holds the bus lock (the guest's frame mark) */
void um2aud_stats(FILE *f);
void um2aud_finish(void);        /* close the .wav (also called at exit) */
void um2aud_stop(void);          /* no more ptx_audio_* calls (before ptx_shutdown unmaps the core's block) */

#endif

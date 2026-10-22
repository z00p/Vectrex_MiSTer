/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx.h - PiTrex-on-MiSTer stream layer.
 *
 * The PiTrex library believes it is driving a real Vectrex bus. Here every bus
 * write becomes a timestamped command in a one-way stream, and every read is
 * answered locally by a shadow VIA/PSG plus a small input block. Time is
 * "stream time" in Vectrex E-cycles (1.5 MHz); delays advance it instead of
 * busy-waiting. See PROTOCOL.md.
 */
#ifndef PTX_H
#define PTX_H

#include <stdint.h>

#define PTX_E_HZ          1500000u
#define PTX_NS_PER_CYCLE  (2000.0 / 3.0)

/* Command word: op[63:56] delta[55:40] addr[39:24] data[23:16] */
#define PTX_OP_WRITE      0x01u
#define PTX_OP_WAIT       0x02u
#define PTX_OP_READ       0x04u   /* side-effect-only read; data ignored */
#define PTX_OP_FRAME      0x10u
#define PTX_OP_RESET_SYNC 0x7Fu

#define PTX_MAX_DELTA     0xFFFFu

static inline uint64_t ptx_cmd(uint32_t op, uint32_t delta, uint32_t addr, uint32_t data)
{
	return ((uint64_t)(op & 0xFF) << 56) | ((uint64_t)(delta & 0xFFFF) << 40) |
	       ((uint64_t)(addr & 0xFFFF) << 24) | ((uint64_t)(data & 0xFF) << 16);
}
#define PTX_CMD_OP(w)    ((uint32_t)((w) >> 56) & 0xFF)
#define PTX_CMD_DELTA(w) ((uint32_t)((w) >> 40) & 0xFFFF)
#define PTX_CMD_ADDR(w)  ((uint32_t)((w) >> 24) & 0xFFFF)
#define PTX_CMD_DATA(w)  ((uint32_t)((w) >> 16) & 0xFF)

/* Written by the core once per frame (MiSTer) or faked by the file backend. */
typedef struct {
	int8_t  pot[4];      /* x1, y1, x2, y2 - same order as the core's mux (PB2:1) */
	uint8_t psg_ioa;     /* PSG port A input byte (buttons, active low) */
	uint8_t pad[3];
	uint32_t seq;
} ptx_input_t;

/* Core status block (MiSTer backend only; valid = 0 elsewhere). */
typedef struct {
	int      valid;
	uint64_t rd_idx;        /* commands the core has fetched */
	uint64_t core_cycles;   /* stream time the core has played */
	uint32_t frames, underruns;
	uint32_t core_build;    /* PiTrex core build number, 0 if not reported */
} ptx_status_t;

/* Backend: where commands go and where input comes from. */
typedef struct {
	int  (*open)(void);
	void (*push)(uint64_t cmd);                 /* append one command */
	void (*frame)(uint64_t stream_cycles);      /* frame boundary: publish + pace */
	void (*input)(ptx_input_t *out);            /* latest input block */
	void (*status)(ptx_status_t *out);          /* core status, for diagnostics */
	void (*close)(void);
} ptx_backend_t;

extern const ptx_backend_t ptx_backend_file;    /* PC harness: raw stream file */
extern const ptx_backend_t ptx_backend_mister;  /* DE10-Nano: DDR ring via /dev/mem */

/* Stream layer API (ptx_core.c) */
int      ptx_init(void);                 /* picks backend from PTX_BACKEND env */
void     ptx_shutdown(void);
void     ptx_write(uint16_t addr, uint8_t data);
uint8_t  ptx_read(uint16_t addr);
void     ptx_advance_cycles(uint32_t cycles);
void     ptx_advance_ns(uint32_t ns);
void     ptx_wait_until_cycle(uint64_t cycle);
uint64_t ptx_now_cycles(void);
uint32_t ptx_now_us(void);
void     ptx_frame(void);                /* call once per frame (v_WaitRecal) */
void     ptx_get_status(ptx_status_t *out);
void     ptx_get_input(ptx_input_t *out);

/* PCM audio into the core's mixer (MiSTer, B6 cores and later; no-ops elsewhere):
 * 16-bit signed mono at 32 kHz. ptx_audio_space() says how many samples to write now
 * to stay about 100 ms ahead. */
int      ptx_audio_ready(void);
uint32_t ptx_audio_space(void);
void     ptx_audio_write(const int16_t *s, uint32_t n);

#endif

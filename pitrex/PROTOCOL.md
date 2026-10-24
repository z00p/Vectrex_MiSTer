# PiTrex-on-MiSTer stream protocol

Version 0, core build B5 (October 2026). Core side: `rtl/pitrex/` and the hooks in `Vectrex.sv`
and `rtl/vectrex.vhd`. ARM side: `pitrex/src/`.

## The idea
A program on the DE10-Nano's ARM never waits for the FPGA. It writes **timestamped VIA writes**
into a ring in shared DDR3, and the core replays them at exact E-clock times (1.5 MHz) on its
own VIA, with the 6809 held in reset. The core's integrators, ramps, sound and renderer then
behave as they would for a real 6809.

The ARM answers every VIA read itself:
- timers and interrupt flags from a shadow VIA that runs on stream time;
- buttons and the joystick comparator from an input block that the core writes back.

So there are no round trips. The PiTrex vector library only ever addresses VIA registers (the
PSG, DAC, multiplexer and integrators all sit behind the VIA), so replaying VIA writes covers
everything it does.

## Shared block
Base: byte address `0x34000000` in DDR3, i.e. 64-bit word `0x06800000`. That's above the
renderer's framebuffer and artwork areas. Offsets below are in 64-bit words.

| Word | Written by | Content |
|---|---|---|
| 0 | ARM | bits 31..0 magic `"PTRX"` (`0x58525450`), bits 63..32 version (0) |
| 1 | ARM | bits 31..0 ring size as log2 of the entry count (16); bits 63..32 session id (random per run; the program stops if it ever reads back a different one) |
| 2 | ARM | `wr_idx`: entries written so far (free-running, 64-bit) |
| 3 | ARM | heartbeat: bits 31..0 counter, bits 63..32 = `~counter ^ "PTRX"` |
| 4 | ARM | B6: audio `wr`: PCM words written (free-running; the core uses bits 13..0) |
| 8 | core | `rd_idx`: entries fetched so far |
| 9 | core | stream time in E cycles |
| 10 | core | bits 31..0 frames, bits 63..32 underruns |
| 11 | core | bits 31..0 signature `"PTXC"` (`0x43585450`), bits 39..32 build number, bits 63..48 (B7) epoch: counts every dropped takeover (reset, timeout, hand-back) |
| 12 | core | B6: audio `rd`: PCM words fetched, 14 bits (wraps at 16,384) |
| 16 | core | input: x1, y1, x2, y2 (signed 8-bit pots), PSG port A (buttons, active low), pad bits |
| 17 | core | input sequence number (changes every 0.5 ms while the core is listening) |
| 0x200 | ARM | the command ring: 65,536 entries of 64 bits |
| 0x10200 | ARM | B6: the PCM audio ring: 8,192 words, each four 16-bit signed samples (first in bits 15..0) |

The core polls the header every 2 µs when idle, fetches up to 32 commands per 256-byte burst,
and writes the status and input words every 0.5 ms.

The renderer's DDR arbiter only lets PiTrex reach `VFB_PITREX_BASE`..`VFB_PITREX_LAST`
(`rtl/videodr0me_fb/vfb_layout_pkg.sv`); accesses outside it are dropped and reads return
zeros. B6 widens it to cover the audio ring. Anything added to this block must widen it too.

### Audio (B6 and later)
Mono 16-bit PCM at 32 kHz goes straight into the core's mixer, next to the Vectrex's own
sound. The ARM keeps about 100 ms queued: it writes samples into the ring, then word 4. The
core fetches up to 16 words per burst into a 64-word FIFO and plays one sample every 750
cycles of the 24 MHz clock; if the FIFO runs dry it holds the last sample. The PCM channel
is mixed at half scale and saturated, only while a program is active. The distance between
`wr` and `rd` is taken modulo 16,384. A core without it (build < 6) simply ignores the ring:
`ptx_audio_ready()` tells a program whether it can use it.

## Command word
```
bits 63..56  opcode
bits 55..40  delta: E cycles to wait BEFORE this command (0..65535)
bits 39..24  address (VIA $D000-$D00F, or elsewhere in the Vectrex map)
bits 23..16  data
bits 15..0   reserved (0)
```
| Opcode | Name | Meaning |
|---|---|---|
| `0x01` | WRITE | after `delta`, one bus write of `data` to `address` (one E period) |
| `0x02` | WAIT | wait `delta` cycles (for gaps longer than a WRITE's delta can hold) |
| `0x04` | READ | after `delta`, one bus read of `address`; the value is discarded. Used for reads with side effects, e.g. clearing VIA flags |
| `0x10` | FRAME | frame marker: counted in word 10 |
| `0x7F` | RESET_SYNC | sent when a program starts; takes no bus time |

If the ring runs dry, the core **pauses the whole machine** until more commands arrive, rather
than drawing with broken timing. Each pause is counted as an underrun in word 10.

## Presence handshake
Agreed with Videodr0me: with the option Off nothing changes, and with it On nothing changes
until a live program is present.

- **PiTrex Mode Off:** the core makes no DDR accesses at all.
- **PiTrex Mode On:** the core only *listens*. The 6809 keeps running (BIOS, cartridge) while
  the core writes words 8-17, including the `"PTXC"` signature and a moving sequence number.
- **Program start (read-only until confirmed):** `/tmp/CORENAME` must be `Vectrex-PiTrex` (or
  `VECTREX` for a direct load). The program then waits, reading only, until word 11 shows `"PTXC"`
  and word 17 is moving. Only then does it write the header: it clears the magic (B7 attaches only
  after seeing it clear), writes its session id, `wr_idx` = the core's `rd_idx`, records the epoch,
  then the heartbeat, then the magic.
- **Heartbeat:** the ARM bumps word 3 every frame and in every wait. The core accepts a sample
  only if the magic is right, the high half matches `~low ^ "PTRX"`, and the low half differs
  from the last accepted value. A leftover magic plus a stray write from another program will
  not pass.
- **Takeover:** the header must say version 0 and ring log2 16. From B7 the first valid heartbeat
  is only a baseline: the core takes over when a later, different one arrives, so a stale header
  left in memory cannot start a takeover. The core then resets the machine, holds the 6809 in reset
  and starts fetching once that reset has finished (B7: no command is consumed earlier).
- **Hand-back:** when the magic is cleared (normal exit, or SIGINT/SIGTERM/SIGHUP), or no valid
  new heartbeat arrives for 100 ms (a crash), the core resets the machine and the 6809 runs
  again.
- **Reset (B7):** any machine or video reset drops the takeover and the 6809 boots the BIOS; the
  epoch in word 11 advances. The core will not take over again while the magic stays set, so the
  program sees the epoch change, clears the magic and exits. Starting the program again attaches
  normally.
- **Pause (B7):** the PCM channel stops consuming samples while the machine is paused.
- **Watchdog on the ARM:** before every publish and every batch of ring writes the program
  checks that `/tmp/CORENAME` still names our core (`Vectrex-PiTrex`, or `VECTREX` for a direct
  load; re-read whenever the file changes), that word 17 moved within 200 ms, that word 8
  (`rd_idx`) has not gone backwards and that its session id is still in word 1. If any check
  fails it exits without writing anything else. `pitrexd` also stops every PiTrex program as
  soon as `/tmp/CORENAME` changes to another core. A user-space program cannot be revoked
  instantly: on hardware it stops about 0.4 s after another core is loaded.

## ARM side (`pitrex/src/`)
- **Stream time:** `ptx_core.c` keeps stream time in E cycles. Every write takes one cycle, and
  the PiTrex library's delays (patched by `tools/patch_lib.py`) add to stream time instead of
  busy-waiting.
- **Shadow VIA:** T1/T2 counters and IFR are computed from stream time and the writes issued.
  Ports and DDRs hold the last values written.
- **Reads:**
  - buttons (PSG register 14 through port A) come from the input block;
  - the joystick comparator (port B bit 5) is `pot > DAC`, using the input block and the last
    port A write.
- **Pacing:** the program stays at most 2 frames ahead of `rd_idx`. The core's command FIFO
  holds 128 entries, so "fetched" is close to "played", and input latency stays around two
  frames.
- **Audio (B6):** `ptx_audio_ready()`, `ptx_audio_space()` and `ptx_audio_write()` in
  `ptx_backend_mister.c`; `games/tonetest` is a two-minute test tone.
- **Backends:** `ptx_backend_mister.c` writes the DDR block through `/dev/mem`, and needs root.
  `ptx_backend_file.c` writes the stream to a file for the PC harness (`tools/ptxplay`).

## Core side (`rtl/pitrex/`)
- **`pitrex_ddr.sv` (clk_125):** a read client and a write client on `vfb_ddr_arbiter`,
  between composition and artwork upload in priority. It holds the presence logic and passes
  commands through a 128-entry asynchronous FIFO.
- **`pitrex_player.sv` (clk_sys, 24 MHz):** counts each `delta` on the E-clock enable, then
  drives the bus on the falling-E edge the 6809 would use. In `vectrex.vhd` the CPU's address,
  data and R/W outputs are multiplexed with the player's, so every downstream decode (VIA, RAM,
  cartridge, the renderer's Timer 2 frame detector) is unchanged.
- **`pitrex_audio.sv` (clk_sys, B6):** plays the PCM channel from its FIFO at 32 kHz.
- **OSD:** System > **PiTrex Mode** (status bit 36). Core Info shows the PiTrex build.

## Frame boundaries
Videodr0me's renderer starts a new displayed frame when VIA Timer 2 is reloaded, as the BIOS
does in `Wait_Recal`. The PiTrex library does this every frame. A program that doesn't use
the library (for example the UVMC2 runtime) must reload T2 once per frame, or the display
keeps showing the previous image.

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
| 1 | ARM | ring size as log2 of the entry count (16) |
| 2 | ARM | `wr_idx`: entries written so far (free-running, 64-bit) |
| 3 | ARM | heartbeat: bits 31..0 counter, bits 63..32 = `~counter ^ "PTRX"` |
| 8 | core | `rd_idx`: entries fetched so far |
| 9 | core | stream time in E cycles |
| 10 | core | bits 31..0 frames, bits 63..32 underruns |
| 11 | core | bits 31..0 signature `"PTXC"` (`0x43585450`), bits 39..32 build number |
| 16 | core | input: x1, y1, x2, y2 (signed 8-bit pots), PSG port A (buttons, active low), pad bits |
| 17 | core | input sequence number (changes every 0.5 ms while the core is listening) |
| 0x200 | ARM | the command ring: 65,536 entries of 64 bits |

The core polls the header every 2 µs when idle, fetches up to 32 commands per 256-byte burst,
and writes the status and input words every 0.5 ms.

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
- **Program start (read-only until confirmed):** `/tmp/CORENAME` must be `VECTREX`. The program
  then waits, reading only, until word 11 shows `"PTXC"` and word 17 is moving. Only then does
  it write the header: `wr_idx` starts at the core's `rd_idx`, then the heartbeat, then the
  magic.
- **Heartbeat:** the ARM bumps word 3 every frame and in every wait. The core accepts a sample
  only if the magic is right, the high half matches `~low ^ "PTRX"`, and the low half differs
  from the last accepted value. A leftover magic plus a stray write from another program will
  not pass.
- **Takeover:** on the first valid heartbeat the core resets the machine, holds the 6809 in
  reset and starts fetching.
- **Hand-back:** when the magic is cleared (normal exit, or SIGINT/SIGTERM/SIGHUP), or no valid
  new heartbeat arrives for 100 ms (a crash), the core resets the machine and the 6809 runs
  again.
- **Watchdog on the ARM:** while running, the program checks that word 17 moves (within
  500 ms) and that `/tmp/CORENAME` is still `VECTREX` (once a second). If either fails, it
  exits without writing anything else.

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
- **OSD:** System > **PiTrex Mode** (status bit 36). Core Info shows the PiTrex build.

## Frame boundaries
Videodr0me's renderer starts a new displayed frame when VIA Timer 2 is reloaded, as the BIOS
does in `Wait_Recal`. The PiTrex library does this every frame. A program that doesn't use
the library (for example the UVMC2 runtime) must reload T2 once per frame, or the display
keeps showing the previous image.

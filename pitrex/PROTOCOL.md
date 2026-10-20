# PiTrex to MiSTer stream protocol (v0, implemented 20 Oct 2026)

Implemented in this branch: core side in `rtl/pitrex/`, ARM side in `pitrex/src/`.

## Principle
The ARM never waits on the FPGA. It writes **timestamped VIA writes** into a ring in shared
DDR. The core replays them at exact E-clock (1.5 MHz) times into its own VIA, so the
simulated integrators, ramps and beam behave exactly as on real hardware. The ARM answers
all reads itself from a shadow VIA plus a per-frame input block written by the core.

## Videodr0me's answers (17 Oct)
- Full sources ship with the release (Sunday night). The core replaces the old Vectrex core in
  MiSTer-devel, either as one big PR or as a full replace with new history.
- Route PiTrex output to whatever hardware registers it targets; the core simulates
  practically all the hardware. Talking to his vector render pipeline directly is also allowed.
- The core already uses DDRAM (one framebuffer plus 4 overlay artwork planes, one per
  supported resolution) and SDRAM (his logic runs at 125 MHz). It has a **DDR arbiter**: add a
  stage, fetch **~256-byte chunks**, priority **just above the artwork loader** and below the
  time-critical clients. "Plenty of room left" in DDR.
- He expects the core side to be easy and the ARM side to be the main risk.
- Checked 17 Oct: the PiTrex vector library only ever addresses **VIA registers** (port B 94×,
  port A 57×, DDR_a 10×, then cntl, IFR, T1, T2, ACR, SR). The PSG, DAC, mux and integrators
  all sit behind the VIA, so replaying VIA writes covers everything the library does.

## Shared region (base: pick a free area once the core's DDR map is known; 64-bit words to match DDRAM_*)
| Offset | Size | Writer | Content |
|---|---|---|---|
| 0x0000 | 64 B | ARM | Header: magic "PTRX", version, ring size, ARM write index, ARM heartbeat |
| 0x0040 | 64 B | core | Status: core read index, current E-cycle count (64-bit), frame counter, underrun count |
| 0x0080 | 64 B | core | Input block: buttons (4 per pad × 2), analog X/Y per pad (signed 8-bit), sequence number |
| 0x1000 | N × 8 B | ARM | Command ring (power of two entries, e.g. 64 K entries = 512 KB) |

## Command word (64-bit)
```
bits 63..56  opcode
bits 55..40  delta cycles (E-cycles to wait BEFORE executing; 0..65535)
bits 39..24  address (VIA 0xD000-0xD00F, or other Vectrex space)
bits 23..16  data
bits 15..0   reserved / opcode-specific
```
Opcodes (initial set):
- `0x01 WRITE`: after `delta` cycles, perform a bus write of `data` to `address`.
- `0x02 WAIT`: wait `delta` cycles (for long gaps; WRITE covers short ones).
- `0x03 WAIT_FLAG`: wait until a VIA IFR bit (data = mask) is set. This is a safety net if
  the shadow and core timers drift; normally unused.
- `0x10 FRAME`: frame marker (lets the core count frames and tell underrun from idle).
- `0x7F RESET_SYNC`: ARM restarted; the core resets its VIA state and stream time.

## ARM side (pitrexio-mister.c)
- Keeps `stream_time` (E-cycles). Every library delay (`DELAY_CYCLES`, `WAIT_CYCLE_NANO`,
  `waitCycleMarkEnd`, ...) adds to it instead of busy-waiting.
- Shadow VIA: T1/T2 counters and IFR, computed from `stream_time` and the writes issued; DDR
  and port registers hold the last written values; the DAC value comes from the last port A
  write.
- Reads:
  - IFR / T1 / T2: from the shadow.
  - Buttons (PSG reg 14 via port A): from the input block.
  - Joystick comparator (port B bit 5): computed as `axis > dac` from the input block (exact
    sign convention to verify against the core and Malban's code).
- Flow control: if the ring is nearly full, sleep briefly (this is the only waiting). If the ARM
  falls behind, the core sees an underrun and holds the beam with zero blanking (counted in
  status).
- Pacing: the ARM stays at most 2 frames ahead of the core's fetched index (`rd_idx`); the
  core's FIFO is only 128 entries, so fetched is close to played.

## Core hook points (read from the released source, f6df13e, 18 Oct 2026)
- Clocks: the whole machine **and** the DDR arbiter run on `clk_sys` = 24 MHz
  (`rtl/pll.v`), so there is no clock-domain crossing. `cpu_en` pulses every 4 clocks
  (6 MHz), and the CPU wrapper `rtl/mc6809.v` derives E/Q from it (E = 1.5 MHz), so
  **E keeps running when the CPU is halted**.
- HALT: `rtl/mc6809.v` instantiates `mc6809is` with `.nHALT(1'b1)` and leaves `.BA()`
  unconnected. Expose both: drive nHALT low in PiTrex mode and use BA as "bus is ours".
- VIA: `rtl/vectrex.vhd` (Gideon Zweijtzer's 6522, GPL-3) is fed from `cpu_addr`,
  `cpu_do`, `via_write` and `via_read`, with `rising => cpu_e_fall`, `falling => cpu_e_rise`.
  `via_write_accept = via_write and cpu_e_rise` also feeds the T2 frame-boundary
  detector. Plan: introduce `bus_addr/bus_do/bus_rw` = player when (pitrex and BA)
  else CPU, and use them for all the decodes (`via_cs`, `ram_cs`, `rom_cs`, `cart`, T2
  detector). The player changes its outputs on the same `cpu_e_fall` edge the CPU uses, one
  command per E period.
- Inputs the ARM needs (the input block): `pot_x_1/y_1/x_2/y_2` (signed, as the comparator
  uses them; mux = `via_pb_o(2:1)`, `compare = pot > signed(via_pa_o)`) and the PSG
  port A input byte (buttons, active low) that `jt49_bus` sees on IOA.
- DDR: `rtl/videodr0me_fb/vfb_ddr_arbiter.sv`, fixed priority readout > flush > fill >
  composition > artwork upload > artwork readout. Add the PiTrex reader **between
  composition and artwork upload**. The layout (`vfb_layout_pkg.sv`, 64-bit word addresses):
  framebuffer 0x06000000-0x0654FFFF, artwork 0x06550000-0x065CFFFF, which is byte
  0x30000000-0x32E7FFFF. Proposed PiTrex base: **byte 0x34000000 = word 0x06800000**
  (to confirm with Videodr0me and against Main's own shmem use).
- OSD: add `P3O[xx],PiTrex,Off,On;` to the System page using a free status bit (used
  bits: 0-3, 7-12, 16-35, 39, 40, 47-50, 63, 64, 66-68, 116-123).

## Core side (to build in a fork of the core)
- OSD System menu: "PiTrex: Off/On". When on: hold the 6809 in HALT, reset VIA/PSG, and start
  the ring reader.
- Ring reader: a new client on the core's existing DDR arbiter, priority just above the artwork
  loader. It fetches 256-byte bursts (32 commands) into a small FIFO (2 × 32 entries, refill when
  half empty), counts down `delta` against the E-clock enable, then drives the VIA write port
  exactly as a 6809 write cycle would. Load estimate: ~30k commands a frame is ~1,000 bursts a
  frame, which is negligible.
- Write the status block and the input block (from the pads it already reads) once per frame.

## Open questions
1. VIA write port vs feeding the ramp logic directly: start with the VIA (exact and simplest);
   the direct pipeline is an optimisation for later.
2. The DDR map: read it from the source on Sunday (framebuffer + 4 overlay planes are taken).
3. Sound: PSG writes go through VIA port A/B handshakes, so they are covered by WRITE. Check
   that the PSG BDIR/BC1 sequencing timing is preserved.
4. The PiTrex Linux build vs a bare-metal target on CPU1: start with Linux userspace pinned
   to CPU1 (`taskset`).

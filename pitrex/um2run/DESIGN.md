# um2run: how it works

An overview for developers. Everything here was worked out by running cartridge images and
reading the public UVMC2 SDK source (github.com/tullulah/uvmc2-sdk), which documents the
runtime inside the images. No image bytes, firmware or ROM data are part of this repository.

## The problem

A UVMC2 cartridge image (`.um2`) is a RAM image for the cartridge's RP2350 (two Cortex-M33
cores). It draws by driving the Vectrex bus itself: a PIO state machine plus DMA put one bus
word on the pins per E period. The MiSTer's DE10-Nano has a Cortex-A9 (ARMv7-A) running Linux, and
the Vectrex is an FPGA core that plays a command ring (see `../src/ptx_*.c` and `../PROTOCOL.md`).
um2run lets the unmodified image run on the A9 and turns what it does to the bus into ring
commands, so the cartridge and the core never need to know about each other.

## Pieces

| File | Job |
|---|---|
| `um2run.c` | Loader and CPU side. Maps the image at 0x20000000 (and PSRAM at 0x11000000) in a Linux process, starts it in Thumb state, catches what an A9 cannot do (signals), and runs core 1 as a second thread. Also the loading screen, pacing, and the diagnostics. |
| `um2bus.c` / `.h` | Device model for the RP2350 pieces the images use: SIO (GPIO, FIFO, spinlocks, core id), PIO0 with a small interpreter, 16 DMA channels, the SPI0 controller for the SD card, TIMER0 alarms, the QMI/PSRAM ID handshake, the NVIC, and the PIO2 I2S audio path. |
| `um2sd.c` / `.h` | An SD card in SPI mode (SDHC, block addressed) backed by a disk image file. |
| `um2aud.c` / `.h` | Cart audio sink: resamples the I2S stream to the 32 kHz mono PCM channel of the core, with a jitter buffer. |
| `../src/ptx_*.c` | The PiTrex stream layer the other programs use: shadow VIA/PSG, the ring, the MiSTer backend (takeover handshake with the core). |

## CPU side

- **svc.** The image's system calls are `svc #n`. The loader patches the `svc` halfwords to
  `udf` at load (the MiSTer kernel has no seccomp), performs ARMv8-M exception entry (stack frame,
  EXC_RETURN) and runs the image's own SVCall handler; the exception return is caught when the
  handler's return reaches the magic EXC_RETURN address.
- **M33-only instructions** (SDIV/UDIV, VSEL, VRINT, VMAXNM/VMINNM, LDA/STL/LDAEX/STLEX, `mrs/msr PRIMASK`, TT, ...) fault
  or are UNDEFINED on the A9. The first trap emulates the instruction; the site is then rewritten
  to `b.w stub`, where the stub does the same without a trap (a trap costs about 50 us on the
  A9). A patch is only made with the other core parked, so it can never run half-written.
  `selftest/` has synthetic guests for each of these paths.
- **RP2350 DCP (coprocessor p4).** The trap decodes the pico-sdk's canned arithmetic,
  comparison, classification and conversion sequences, and computes results with the A9's VFP.
  X, Y, E and the in-progress sequence are kept per guest core. The pico-sdk checks DCP state
  with PCMP and saves/restores X, Y and E when nested code uses the DCP, including an interrupt
  handler that uses the SDK routines; um2run keeps up to 8 nested saves (deeper ones are logged
  and not kept, and their restores don't pop a kept one). This is a model of the sequences, not
  of the DCP's datapath: each operation's result is computed whole when the sequence reads it,
  and the intermediate mantissa reads return 0. Results are the A9 VFP's (round to nearest,
  ties to even). Edge cases (subnormals, NaN payloads and the NaN produced by invalid operations,
  out-of-range conversions) follow the A9 and have **not** been checked against a real RP2350.
  `selftest/gen_dcp.py` writes the DCP instructions as mnemonics so the assembler encodes them;
  its expected values restate the same rules, so it checks the decoding and the sequences, not
  the hardware's edge cases. Every miss is logged (up to 32). There are no native stubs yet, so
  each p4 instruction traps (about 50 us): a double add costs about 0.3 ms.
- **Peripheral access** (loads and stores to 0x4xxxxxxx / 0xdxxxxxxx) faults the same way and is
  handed to the device model; hot sites are patched to a short stub too.
- **XIP cache maintenance.** Some images flush the RP2350's flash cache one line at a time with
  byte stores into its maintenance window (0x18000000-0x19ffffff; the low address bits pick the
  operation). um2run has no flash cache to keep in step, so the 32 MB window is mapped as plain
  memory that nobody reads, and those stores cost nothing. Before 1.3 each one trapped: Dragon's
  Lair ran at about 3 frames/s and M.A.C.H. 3 never reached its first frame. `UM2_NOXIPMAINT=1`
  turns the mapping off (the stores trap again).
- **Boot ROM reads.** Centipede 0.3a reads its noise table through a pointer it never sets, so from
  the first 16 KB of the boot ROM (readable on the real chip). ARM Linux maps nothing below
  0x2000, so the area cannot simply be mapped: a read-only copy (zero, plus the canned header
  words) sits at 0x80000000, and a hot `ldrb rt,[rn,rm]` site that reads the boot ROM area becomes
  `b.w stub`, where addresses below 0x8000 are moved onto the copy. Centipede went from 1-5 to
  50 frames/s; nothing else reads there. `UM2_NOROMPATCH=1` turns it off. The area reads as zero
  unless `--bootrom FILE` gives the real boot ROM (`bootrom-combined.bin` from
  github.com/raspberrypi/pico-bootrom-rp2350, not shipped here); then the copy and the trapped
  reads both return its bytes (the header words stay the canned ones), and Centipede's noise
  sounds. `mister_run.sh` passes `rp2350_bootrom.bin` from beside itself when it is there.
- **Core 1** is a thread. The two guest cores are placed on the A9's two CPUs; the Main program
  of the MiSTer (OSD, controllers) lives on CPU1 as well, so the guest is deliberately
  throttled where it only spins (timer polls, E-clock polls).
- **Interrupts.** Alarm, DMA and spare IRQs are delivered by a per-core NVIC model: pending and
  enable bits, priorities, tail chaining, round robin among equal priorities.
- **Clock.** The MiSTer kernel has no vDSO, so every `clock_gettime` is a real system call
  (1.3 us on the A9), and the guest TIMER and DWT reads, the audio thread and the ring backend
  read the clock a lot. `um2clock.c` reads the Cortex-A9 global timer through `/dev/mem` instead
  (63 ns). That saved about 4% of core 1's CPU. It plays the cart's crystal: its rate is measured
  against CLOCK_MONOTONIC once at start and never changed, and every time in um2run (TIMER,
  alarms, audio pacing, intervals) comes from it. It drifts away from CLOCK_MONOTONIC (about
  180 ppm here), so no absolute deadline made from it goes to the kernel: sleeps are relative
  (the irq thread's futex wait), or the kernel's own deadline is formed just before the call
  (`um2_kernel_deadline`, for `pthread_cond_timedwait`). Until 18 Nov 2026 it was slewed back
  onto CLOCK_MONOTONIC every heartbeat instead, a second clock mixed into the first. Where the timer cannot be mapped (the PC) it falls back to `clock_gettime`;
  `UM2_SLOWCLOCK=1` forces that.

## Bus side

- **Bulk path.** All drawing goes through PIO0 SM0 fed by DMA. The image uploads a 25-instruction
  program; when it is the standard "one write or park per E period" program, each word is decoded
  directly (write word: one E period and a latched VIA write; park word: N idle periods)
  instead of being interpreted, which is what makes it fast enough for the A9. Other programs
  fall back to the PIO interpreter.
- **Single accesses.** Between frames the image switches the pads to SIO and bit-bangs the bus
  (controllers, PSG): the model tracks GPIO output/enable, toggles the synthetic E clock on
  each read, and turns a falling edge into a VIA read or write.
- **Stream.** A latched VIA write at E index i becomes `ptx_write(addr, data)` after the idle
  cycles in between. A frame starts at the image's frame mark (enabling the PIO state
  machine, or the first controller read of the between-frames window) and ends with the T2
  reload the core treats as a frame boundary. Since 20 Nov 2026 the stream is **not padded**: an SDK
  image paces itself (its `UVM2_HZ` filler pads each list inside the list, to 30000 E cycles for 50 Hz,
  25000 for 60, nothing for 0 = free-running), so the old padding to 30000 was a second, conflicting
  pacer: Demon Attack (an Atari 2600 ROM, built for 60 Hz, lists 25000) ran at 50 instead of ~58 fps.
  `--frame-pad N` pads to N: a stand-in for the idle time a free-running image (Punch-Out) leaves
  between its lists on a real cart, which um2run does not model yet. With no new list the cart's core 1
  pushes nothing: it only reads the controllers and services audio once per frame period, and the beam
  stays parked (SDK doc 05, "The underrun is the idle state"; confirmed by the SDK's author, 20 Nov 2026).
  A free-running image's frame rate on a real cart is set by how fast core 0 builds the next list,
  which um2run does not model either, so such an image can run faster under um2run than on a cart. `--frame-cycles` (default 30000) now only sets the frame-mark fallbacks and
  um2run's own screens. The exit stats list each image's list lengths per frame. `--hold` hands the whole frame to the ring at once.
- **Pacing.** `ptx_frame()` paces on the core's read index, so the guest never runs far ahead
  of what the core has played.
- **List pickup (experiment, off by default: `--pickup` or `UM2_PICKUP=1`).** Core 1 hands each
  frame's whole bus list to one function: the SDK's `uvm2_exec(list, n)` (3-byte entries) or the
  push function of Ralf Kornmann's v2 engine (BattleZone 0.3a, Centipede 0.3a). At load um2run
  looks for that function by exact instruction templates (addresses, literal-pool words and the
  pin constant free; the SDK helpers it calls are checked too) and, with exactly one match,
  patches its entry to a stub that builds the same PIO0 TXF0 words natively and hands them to
  the model in the same order (`um2bus_pio0_push`): the stream is unchanged, the guest's
  per-batch DMA register stores (SDK) or per-word FIFO stores (v2) are gone. The SDK function's
  SIO-to-PIO switch (27 pin FUNCSEL stores and PIO0 CTRL = 1) goes through the normal MMIO path,
  and its park-count maximum (read by the image's bus-release wait) is kept. `UM2_PKDIAG=file`
  writes per-list and per-frame hashes for equivalence tests (and observes the function with
  pickup off).
- **Spin wait (experiment, off by default: `UM2_SPINWAIT=1`).** The SDK's core 0 waits for
  core 1 in one 12-byte loop (`uvm2_frame_end` / `uvm2_frame_begin`: spin until
  `uvm2_frame_done` reaches the frame it needs, counting rounds). On the RP2350 that spin costs
  nothing; here core 0 is a real-time thread on CPU0 and spins at 100%. At load um2run looks for
  the loop's exact instructions and, with exactly one match, patches its first 4 bytes to a stub
  that runs the same five instructions and, only when the loop would go round again, sleeps 50 us
  (a Linux nanosleep straight from the stub, so guest interrupts are still delivered there). The
  registers and flags at the exit are the loop's own; the image's round count
  (`uvm2_stats.wait_spins`, statistics only in the SDK) is smaller. Timing does change: on 19 Nov
  2026 the frame rate was the same and core 0 used about 60% less CPU, but the audio thread
  (which shares CPU0) kept better time and Pac-Man's stale cart audio got 2 to 4 times worse
  (research/experiments_audio_spin_2026-11-19.md). Not for a release.

## Storage

- **SD card.** SPI0 is a PL022 model; the card is `um2sd.c` over an image file given with
  `--sd`. The image's own FatFs mounts it, so the file must be a FAT image the cartridge
  understands (see `../UVMC2_GAMES.md`, "Card images"). um2run holds an exclusive flock on
  `<image>.lock` until it exits. The card builder takes the same lock before reading saves
  or replacing an image. The lock file stays in place, even after a crash. Every card byte is four accesses
  (status poll, data write, status poll, data read), so the stubs take SPI0's data and status
  words straight to the model (`um2bus_spi_load` / `um2bus_spi_dr_store`), and the status read
  skips the bus lock. VecDoom, which streams its WAD in play, went from about 47 to 50 fps in
  attract (its ceiling). `UM2_NOSPIFAST=1` puts them back on the generic path.
  The card has a lock of its own (`g_spim`, taken inside the bus lock where both are
  needed), so core 0's byte loop no longer waits behind core 1's PIO and DMA work for the
  bus lock (`UM2_SPIBUSLOCK=1`: the old single lock). The byte path (`um2bus_spi_*`,
  `um2sd.c`) uses no VFP register and no libc call but the mutex and `pread` / `pwrite`
  (table CRC16, plain copy loops), so the stubs of SPI0 sites call it through a thunk that
  saves no VFP state; any other address in such a stub still goes through the full thunk
  (`UM2_NOSPILEAN=1`: the full thunk for all). The card lock is a spin lock that falls back
  to 20 us sleeps after about 2000 spins, so a holder preempted on the same CPU cannot
  deadlock it (`UM2_SPIMUTEX=1`: a pthread mutex). `UM2_SRFAST=1` (experimental, off by
  default: no gain measured beyond the noise) sends SR loads at SPI0 sites straight to a
  frameless leaf (`um2_srfast`) without the thunk's stack switch.
- **Bit-banged SD card (19 Nov 2026; `UM2_NOSDBB=1`: off).** Images built with the SDK's old
  transport (`uvm2_sd.c` with `-DUVM2_SD_BITBANG`: Contrex, Glint, Jukebox 2 and 3, QUARTER,
  Ship It, Summit in the 16 Nov survey) drive the card through SIO GPIO32..47 with SPI0 off:
  per bit, MOSI to `GPIO_HI_OUT_SET` / `_CLR`, a delay, SCK set, MISO from `GPIO_HI_IN`, a
  delay, SCK clear. The SDK documents that at about 300 KB/s on the cart; with each of the 32
  accesses a byte going through a stub or a trap, um2run managed about 45 KB/s, so Contrex's
  load frames took 45 to 50 ms and its cart audio refills fell behind the SDK's 50 ms lead
  (the rough sound for the first seconds). Compiled, the byte routine `xfer()` is the same
  112 bytes in every such image (one or two copies). At load its entry becomes `b.w stub`;
  the stub calls `um2bus_sdbb_xfer` through the full thunk, which makes the same 32 accesses
  in the same order on the same model under one bus lock and returns the byte. The card sees
  the same bytes (the `um2sd` MOSI / MISO hashes at exit are equal with and without, checked
  on Contrex); only the delay loops, which just burn time, are not run. Contrex on the MiSTer,
  3 runs each way: the image takes the ring at 1.7 s instead of 2.4 s, its load frames are at
  most about 35 ms apart instead of about 100, and the cart audio has no waits and no stale
  frames during the load (without: 9 to 88 waits and 200 to 1,800 stale frames a second).
- **PSRAM.** Images that stage a ROM set in PSRAM get 8 MB of RAM at 0x11000000 (and the
  uncached alias at 0x15000000, the same memory). The QMI direct-mode ID handshake that
  proves the PSRAM is present is modelled.

## Cart audio

The images produce audio as 16-bit I2S from PIO2, fed by a DMA channel (an endless ring, or
short ping-pong blocks refilled in a DMA interrupt). A DMA channel whose target is a PIO TX FIFO is
treated as a paced transfer instead of a synchronous one: a helper thread consumes the words
at the frame rate derived from the image's clock setup and PIO divider, publishes the
channel's read address a little ahead of the true position (the ring readers poll it to find
their fill level), and hands the samples to `um2aud`, which resamples to 32 kHz mono (windowed
sinc), removes the DC offset of the AC-coupled DAC, and writes into the core's PCM queue behind
a ~60 ms jitter buffer. `UM2_NOAUDIO=1` turns all of this off.

The helper thread runs on wall-clock time, the game on whatever CPU it gets. When a game refills
its ring late, a plain reader catches up with the write index and plays the previous lap again:
an echo exactly one ring later (Pac-Man: 64 ms). So an endless ring keeps a copy of what each
position held when it was last played, and the reader waits at a block of 32 frames that is still
unchanged (and not flat, so silence never holds it up). It plays what it owes as soon as the game
has written it, and after `UM2_AUDHOLD` ms (default 30, 0 = off) it plays on regardless. On the
MiSTer this cut Pac-Man's echo from about 2.8% of the loud 8 ms windows to about 0.7%
(`tools/audio_echo.py`); longer limits make it worse, because the reader then stays behind for
good. The echo that is left comes where the game itself drops to about half speed.

## Black-screen guard and CPU placement

Right after a core load, Main is still setting the video mode on CPU1. `mister_run.sh` therefore
waits (`--settle`) and keeps core 1 of the guest on CPU0 for the first seconds (`--cpu1-after`);
once the core has been up for 15 s the guard shrinks to 1 s of settle, and core 1 has its own CPU
from the start.

## What is deliberately not modelled

USB and SDIO (the images tested so far do not use them), the PWM block, PLL/clock hardware beyond
what the clock calibration reads back, and GPIO panel inputs beyond the controller pads. Reads of
unmapped peripherals return canned values (`--no-canned` returns 0 instead). When a new image
misbehaves, run it without `--bus` for a census of everything it touches that is not modelled.

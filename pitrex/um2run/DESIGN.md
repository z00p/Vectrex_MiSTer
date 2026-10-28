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
- **M33-only instructions** (SDIV/UDIV, VSEL, LDA/STL/LDAEX/STLEX, `mrs/msr PRIMASK`, TT, ...) fault
  or are UNDEFINED on the A9. The first trap emulates the instruction; the site is then rewritten
  to `b.w stub`, where the stub does the same without a trap (a trap costs about 50 us on the
  A9). A patch is only made with the other core parked, so it can never run half-written.
  `selftest/` has synthetic guests for each of these paths.
- **Peripheral access** (loads and stores to 0x4xxxxxxx / 0xdxxxxxxx) faults the same way and is
  handed to the device model; hot sites are patched to a short stub too.
- **Core 1** is a thread. The two guest cores are placed on the A9's two CPUs; the Main program
  of the MiSTer (OSD, controllers) lives on CPU1 as well, so the guest is deliberately
  throttled where it only spins (timer polls, E-clock polls).
- **Interrupts.** Alarm, DMA and spare IRQs are delivered by a per-core NVIC model: pending and
  enable bits, priorities, tail chaining, round robin among equal priorities.

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
  machine, or the first controller read of the between-frames window); the stream is padded
  to a fixed frame length (`--frame-cycles`, default 30000 E cycles = 50 Hz) and ended with the
  T2 reload the core treats as a frame boundary. `--hold` hands the whole frame to the ring at once.
- **Pacing.** `ptx_frame()` paces on the core's read index, so the guest never runs far ahead
  of what the core has played.

## Storage

- **SD card.** SPI0 is a PL022 model; the card is `um2sd.c` over an image file given with
  `--sd`. The image's own FatFs mounts it, so the file must be a FAT image the cartridge
  understands (see `../UVMC2_GAMES.md`, "Card images").
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

## Black-screen guard and CPU placement

Right after a core load, Main is still setting the video mode on CPU1. `mister_run.sh` therefore
waits (`--settle`) and keeps core 1 of the guest on CPU0 for the first seconds (`--cpu1-after`);
once the core has been up for 15 s the guard shrinks to 1 s / 2 s.

## What is deliberately not modelled

USB and SDIO (the images tested so far do not use them), the PWM block, PLL/clock hardware beyond
what the clock calibration reads back, and GPIO panel inputs beyond the controller pads. Reads of
unmapped peripherals return canned values (`--no-canned` returns 0 instead). When a new image
misbehaves, run it without `--bus` for a census of everything it touches that is not modelled.

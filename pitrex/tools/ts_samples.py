#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tac/Scan sound for UVMC2 mode: MAME's tacscan samples zip -> TACSCAN.PCM, on the MiSTer.

    python3 ts_samples.py <samples tacscan.zip> <out TACSCAN.PCM>

Writes the "KSFX" bundle the UVMC2 Tac/Scan port reads for its audio-jack path (the same
layout as the starter kit's wav_to_pcm.py): magic "KSFX", u16 count, u16 0, then per sound
(u32 offset, u32 samples, u32 rate, u16 format 0 = s16le, u16 0), then the data, each
sound 4-byte aligned. Sounds are in the port's sample order; each is mono 16-bit at
32 kHz, DC removed and normalised to its own peak, with the ship roar (01-03) at half
level so the shots stand out. Python 3 standard library only (wave, zipfile, audioop).
"""
import audioop
import io
import struct
import sys
import wave
import zipfile

RATE = 32000
ORDER = ["01.wav", "02.wav", "03.wav", "plaser.wav", "pexpl.wav", "pship.wav",
         "tunnelh.wav", "sthrust.wav", "slaser.wav", "sexpl.wav", "eshot.wav", "eexpl.wav",
         "tunnelw.wav", "flight1.wav", "flight2.wav", "flight3.wav", "flight4.wav",
         "flight5.wav", "formatn.wav", "warp.wav", "credit.wav", "1up.wav"]
GAIN = {"01.wav": 0.5, "02.wav": 0.5, "03.wav": 0.5}


def decode(data):
    """WAV bytes -> mono s16 samples at RATE."""
    with wave.open(io.BytesIO(data)) as w:
        ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        frames = w.readframes(w.getnframes())
    if width == 1:                       # 8-bit WAV is unsigned
        frames = audioop.bias(frames, 1, -128)
    if width != 2:
        frames = audioop.lin2lin(frames, width, 2)
    if ch == 2:
        frames = audioop.tomono(frames, 2, 0.5, 0.5)
    if rate != RATE:
        frames, _ = audioop.ratecv(frames, 2, 1, rate, RATE, None)
    return list(struct.unpack("<%dh" % (len(frames) // 2), frames))


def level(pcm, gain):
    if not pcm:
        return pcm
    dc = sum(pcm) / len(pcm)
    pcm = [s - dc for s in pcm]
    peak = max(abs(s) for s in pcm) or 1
    k = 32767.0 / peak * gain
    return [max(-32768, min(32767, int(round(s * k)))) for s in pcm]


def main(src, out):
    z = zipfile.ZipFile(src)
    names = {n.lower().rsplit("/", 1)[-1]: n for n in z.namelist()}
    head = 8 + 16 * len(ORDER)
    recs, payload, found = [], bytearray(), 0
    for name in ORDER:
        if name not in names:
            recs.append((0, 0, 0))
            continue
        pcm = level(decode(z.read(names[name])), GAIN.get(name, 1.0))
        while (head + len(payload)) % 4:
            payload += b"\0"
        recs.append((head + len(payload), len(pcm), RATE))
        payload += struct.pack("<%dh" % len(pcm), *pcm)
        found += 1
    if not found:
        sys.exit("ts_samples: no Tac/Scan samples in " + src)
    blob = bytearray(b"KSFX") + struct.pack("<HH", len(ORDER), 0)
    for off, n, hz in recs:
        blob += struct.pack("<IIIHH", off, n, hz, 0, 0)
    blob += payload
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    import os
    os.replace(tmp, out)
    print("ts_samples: %d of %d sounds -> %s (%d bytes)" % (found, len(ORDER), out, len(blob)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])

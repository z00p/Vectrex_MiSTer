#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Look for a ring-buffer echo in a recording of a UVMC2 game's sound.

    python3 audio_echo.py REC.wav [--from S] [--to S] [--lag MS ...] [--stereo]

When um2run's cart audio DMA laps the game's writes into its ring, the old lap plays again: the
sound repeats exactly one ring length later (Pac-Man: 2048 samples, 64 ms at 32 kHz). For each
lag the script cuts the active part of the recording into 8 ms windows and counts the windows
that are near-identical (correlation > 0.98) to the sound one lag earlier.

Measured 3 Nov 2026 (Pac-Man, in play): um2run on the MiSTer (UM2_WAV) 15.7% at 64 ms, 0% at
60 and 70 ms; the reference, Vectrex_EMU's headless harness (tools/vemu_ref.sh), 0% at all lags.
--stereo: the data is interleaved stereo whatever the header says (Vectrex_EMU v1.5.4's vtest
writes stereo under a mono header).
"""
import argparse
import wave

import numpy as np


def load(path, stereo):
    with wave.open(path) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    if stereo or ch == 2:
        a = a[: len(a) // 2 * 2].reshape(-1, 2).mean(1)
    return a, rate


def echo(a, rate, t0, t1, ms, win_ms=8.0, quiet=1500):
    lag = ms / 1000 * rate
    li, fr = int(lag), lag - int(lag)
    w = int(win_ms / 1000 * rate)
    hit = tot = 0
    for i in range(max(int(t0 * rate), li + 1), int(t1 * rate) - w, w):
        x = a[i:i + w]
        if np.abs(x).max() < quiet:
            continue
        y = (1 - fr) * a[i - li:i - li + w] + fr * a[i - li - 1:i - li - 1 + w]
        xc, yc = x - x.mean(), y - y.mean()
        c = (xc @ yc) / (np.sqrt((xc @ xc) * (yc @ yc)) + 1e-9)
        tot += 1
        hit += c > 0.98
    return hit, tot


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("wav")
    p.add_argument("--from", dest="t0", type=float, default=0.0)
    p.add_argument("--to", dest="t1", type=float, default=0.0, help="seconds (0 = the end)")
    p.add_argument("--lag", type=float, nargs="*", default=[64.0, 60.0, 70.0, 128.0], help="ms")
    p.add_argument("--stereo", action="store_true")
    o = p.parse_args()
    a, rate = load(o.wav, o.stereo)
    t1 = o.t1 or len(a) / rate
    print(f"{o.wav}: {len(a) / rate:.1f} s at {rate} Hz, checking {o.t0:.0f} to {t1:.0f} s")
    for ms in o.lag:
        h, t = echo(a, rate, o.t0, t1, ms)
        print(f"  lag {ms:6.1f} ms: {h}/{t} windows near-identical ({100 * h / max(t, 1):.1f}%)")


if __name__ == "__main__":
    main()

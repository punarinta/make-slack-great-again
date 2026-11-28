#!/usr/bin/env python3
"""Synthesize the bundled notification chime (sfx/notify.wav).

We generate our own sound rather than ship a third-party file so the asset is
unambiguously license-free and tiny. Pure stdlib (math + wave), no numpy.

Output: a short, bright two-note rising chime (C6 -> G6, a cheerful perfect
fifth) with a soft bell-like timbre (a couple of decaying harmonics) and a fast
attack / exponential decay envelope. ~0.4 s, 16 kHz, 16-bit mono (~13 KB).

16 kHz is plenty: the brightest partial is G6's 3rd harmonic (~4.7 kHz),
well under the 8 kHz Nyquist limit. In a 22.05 kHz render everything above
8 kHz sits at -82 dB (just the attack transients), so the lower rate drops
nothing audible. It stays plain PCM WAV because Windows PlaySound and aplay
can play nothing else (src/util/sound_player_*.cpp).

Usage: python3 scripts/gen-notify-sound.py
"""
import math
import os
import struct
import wave

SAMPLE_RATE = 16000
AMPLITUDE = 0.72  # peak, leaves a little headroom

# (frequency Hz, start time s, duration s)
NOTES = [
    (1046.50, 0.00, 0.22),  # C6
    (1567.98, 0.13, 0.27),  # G6 (overlaps the tail of C6 for a smooth roll)
]

# Relative weights of fundamental + harmonics -> soft bell timbre.
HARMONICS = [(1.0, 1.0), (2.0, 0.32), (3.0, 0.12)]


def envelope(t, dur):
    """Fast 6 ms attack, exponential decay over the note duration."""
    attack = 0.006
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) * (5.5 / dur))


def main():
    # End exactly at the last note's tail: the exponential decay is already at
    # ~-48 dB there, so there's no audible click and no point padding silence.
    total = max(start + dur for _, start, dur in NOTES)
    n = int(total * SAMPLE_RATE)
    samples = [0.0] * n

    for freq, start, dur in NOTES:
        s0 = int(start * SAMPLE_RATE)
        ns = int(dur * SAMPLE_RATE)
        for i in range(ns):
            idx = s0 + i
            if idx >= n:
                break
            t = i / SAMPLE_RATE
            env = envelope(t, dur)
            val = sum(w * math.sin(2 * math.pi * freq * mult * t)
                      for mult, w in HARMONICS)
            samples[idx] += env * val

    peak = max(abs(s) for s in samples) or 1.0
    scale = AMPLITUDE / peak

    out_dir = os.path.join(os.path.dirname(__file__), os.pardir, "sfx")
    out_path = os.path.normpath(os.path.join(out_dir, "notify.wav"))
    with wave.open(out_path, "w") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for s in samples:
            v = int(max(-1.0, min(1.0, s * scale)) * 32767)
            frames += struct.pack("<h", v)
        wf.writeframes(bytes(frames))

    print(f"wrote {out_path} ({os.path.getsize(out_path)} bytes, {total:.2f}s)")


if __name__ == "__main__":
    main()

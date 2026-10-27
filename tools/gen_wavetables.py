#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate src/wavetables.h: WAVESCAN's own wavetables, in the classic 8-bit format.

Each table is 64 single-cycle waves of 128 8-bit samples. Waves 0..59 sweep from one sound to another; waves
60..63 are, in every table, triangle, pulse, square and saw (as on the 1980s wavetable synths). These tables are
original (computed here from simple recipes), not copies of any instrument's ROM.

Every wave is also stored band-limited at three smaller sizes (mip levels), for higher notes:
  level 0: 128 samples, harmonics 1..63      level 1: 64 samples, harmonics 1..31
  level 2: 32 samples, harmonics 1..15       level 3: 16 samples, harmonics 1..7
The oscillators pick the level by pitch, so high notes do not fold back into noise.

  python3 tools/gen_wavetables.py           (writes src/wavetables.h, and build/wavetables.png with --png)
"""

import sys
from pathlib import Path

import numpy as np

N = 128
W = 64
SWEEP = 60
OUT = Path(__file__).resolve().parents[1] / "src" / "wavetables.h"
x = np.arange(N) / N  # phase 0..1


def additive(amps, phases=None):
  """a wave from harmonic amplitudes (index 0 = fundamental)"""
  w = np.zeros(N)
  for k, a in enumerate(amps):
    if a and k + 1 < N // 2:
      p = phases[k] if phases is not None else 0.0
      w += a * np.sin(2 * np.pi * (k + 1) * x + p)
  return w


def saw_amps(n):
  return [1.0 / k for k in range(1, n + 1)]


def tri():
  return additive([((-1) ** ((k - 1) // 2)) / (k * k) if k % 2 else 0 for k in range(1, 64)])


def pulse(width):
  # band-limited pulse: Fourier series
  return additive([2 * np.sin(np.pi * k * width) / (np.pi * k) for k in range(1, 64)], [np.pi / 2] * 63)


def square():
  return additive([1.0 / k if k % 2 else 0 for k in range(1, 64)])


def saw():
  return additive(saw_amps(63))


def formant(f0_harm, width, base_n=40, tilt=1.0):
  """a saw-like spectrum with a resonant bump around harmonic f0_harm"""
  amps = []
  for k in range(1, base_n + 1):
    a = 1.0 / (k**tilt)
    a *= 1 + 6 * np.exp(-((k - f0_harm) ** 2) / (2 * width**2))
    amps.append(a)
  return additive(amps)


VOWELS = [(3.0, 9.0), (5.5, 13.0), (2.4, 18.0), (2.2, 7.0), (2.0, 5.5)]  # a e i o u: (F1, F2) in harmonics of ~110 Hz


def vowel(t, pitch_harm=1.0):
  # move through a e i o u
  p = t * (len(VOWELS) - 1)
  i = min(int(p), len(VOWELS) - 2)
  f = p - i
  f1 = VOWELS[i][0] * (1 - f) + VOWELS[i + 1][0] * f
  f2 = VOWELS[i][1] * (1 - f) + VOWELS[i + 1][1] * f
  amps = []
  for k in range(1, 48):
    a = 1.0 / k**0.6
    a *= np.exp(-((k - f1) ** 2) / 3) * 4 + np.exp(-((k - f2) ** 2) / 8) * 2.5 + 0.05
    amps.append(a)
  return additive(amps)


def sync(ratio):
  ph = (x * ratio) % 1.0
  return 2 * ph - 1


def fold(amount):
  s = np.sin(2 * np.pi * x) * (1 + 7 * amount)
  return np.sin(s)


def phase_dist(amount):
  d = 0.5 - 0.48 * amount
  ph = np.where(x < d, x / d * 0.5, 0.5 + (x - d) / (1 - d) * 0.5)
  return np.cos(2 * np.pi * ph)


def fm(index, ratio=2.0):
  return np.sin(2 * np.pi * x + index * np.sin(2 * np.pi * ratio * x))


def organ(t):
  # drawbars fading in one after the other: 16' 8' 5 1/3' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'
  harms = [1, 2, 3, 4, 6, 8, 10, 12, 16]
  amps = np.zeros(32)
  for j, h in enumerate(harms):
    lvl = np.clip(t * len(harms) - j + 1, 0, 1)
    amps[h - 1] += lvl * (1.0 if j < 2 else 0.7)
  return additive(list(amps))


rng = np.random.default_rng(1984)
DIG_KEYS = [rng.random(48) ** 3 * (1 / np.arange(1, 49) ** 0.3) for _ in range(6)]


def digital(t):
  p = t * (len(DIG_KEYS) - 1)
  i = min(int(p), len(DIG_KEYS) - 2)
  f = p - i
  # stepped, not smooth: digital keyframes with a hard change half-way
  f = 0.0 if f < 0.5 else 1.0
  return additive(list(DIG_KEYS[i] * (1 - f) + DIG_KEYS[i + 1] * f))


def bell(t):
  amps = np.zeros(63)
  for h, a in [(1, 1.0), (3, 0.6), (5, 0.25), (7, 0.5 * t), (11, 0.4 * t), (13, 0.35 * t), (19, 0.3 * t), (23, 0.2 * t), (29, 0.25 * t), (37, 0.15 * t)]:
    amps[h - 1] = a
  return additive(list(amps))


def harmonic_add(t):
  n = 1 + int(t * 40)
  return additive([1.0 / k**0.5 for k in range(1, n + 1)])


def odd_add(t):
  n = 1 + int(t * 30)
  return additive([1.0 / k if k % 2 else 0 for k in range(1, 2 * n + 1)])


def brass(t):
  cut = 1.5 + t * 30
  return additive([1.0 / k * (1 / (1 + (k / cut) ** 4)) for k in range(1, 64)])


def resonant(t):
  return formant(2 + t * 34, 1.2)


def octaves(t):
  # sub octaves under a fixed top: 1 -> the fundamental is reached from the 8th
  amps = np.zeros(32)
  for j, h in enumerate([16, 8, 4, 2, 1]):
    amps[h - 1] = np.clip(t * 5 - j + 1, 0, 1) * (0.6 if h > 1 else 1.0)
  return additive(list(amps))


def choir(t):
  return vowel(t) + 0.15 * formant(4, 3, 30, 1.5)


def strings(t):
  return additive([(1.0 / k) * (0.6 + 0.4 * np.cos(k * (0.3 + t * 2.0))) ** 2 for k in range(1, 50)])


def hollow(t):
  # a pulse's width: hollow (50 %) to thin
  return pulse(0.5 - 0.46 * t)


def grit(t):
  # a sine crushed to fewer and fewer levels
  levels = int(2 + (1 - t) ** 2 * 30)
  return np.round(np.sin(2 * np.pi * x) * levels) / levels


def mixed(t):
  # saw to square to sine, smooth
  if t < 0.5:
    return saw() * (1 - 2 * t) + square() * 2 * t
  return square() * (2 - 2 * t) + np.sin(2 * np.pi * x) * (2 * t - 1)


TABLES = [
  ("RESONANT", resonant),
  ("HARMONICS", harmonic_add),
  ("PULSE WIDTH", hollow),
  ("SYNC SWEEP", lambda t: sync(1 + t * 7)),
  ("ORGAN", organ),
  ("VOX", vowel),
  ("BELLS", bell),
  ("DIGITAL", digital),
  ("OCTAVES", octaves),
  ("ODD HARM", odd_add),
  ("CHOIR", choir),
  ("BRASS", brass),
  ("FOLD", fold),
  ("PHASE DIST", phase_dist),
  ("FM", lambda t: fm(t * 6)),
  ("FM 3:1", lambda t: fm(t * 4, 3.0)),
  ("STRINGS", strings),
  ("GRIT", grit),
  ("SAW>SINE", mixed),
  ("BASICS", None),  # the plain waves, as an "upper" table
]


def band_limit(w, harms, size):
  spec = np.fft.rfft(w)
  spec[harms + 1 :] = 0
  spec[0] = 0  # no DC
  full = np.fft.irfft(spec, N)
  # resample to size by taking the spectrum's first harmonics
  s2 = np.zeros(size // 2 + 1, dtype=complex)
  s2[: harms + 1] = spec[: harms + 1] * (size / N)
  return np.fft.irfft(s2, size), full


def basics_wave(i):
  # 0..59: triangle -> saw -> square -> narrow pulse
  t = i / 59
  if t < 1 / 3:
    f = t * 3
    return tri() * (1 - f) + saw() * f
  if t < 2 / 3:
    f = t * 3 - 1
    return saw() * (1 - f) + square() * f
  return pulse(0.5 - 0.44 * (t * 3 - 2))


def norm(w):
  w = w - w.mean()
  m = np.max(np.abs(w))
  return w / m if m > 0 else w


def build():
  fixed = [norm(tri()), norm(pulse(0.25)), norm(square()), norm(saw())]
  tables = []
  for name, fn in TABLES:
    waves = []
    for i in range(SWEEP):
      w = basics_wave(i) if fn is None else fn(i / (SWEEP - 1))
      waves.append(norm(np.asarray(w, dtype=float)))
    waves += fixed
    tables.append((name, waves))
  return tables


def q8(w):
  return np.clip(np.round(w * 127), -127, 127).astype(int)


def main():
  tables = build()
  levels = [(63, 128), (31, 64), (15, 32), (7, 16)]
  out = ["/* SPDX-License-Identifier: GPL-3.0-only */", "/* generated by tools/gen_wavetables.py: do not edit */",
         "#ifndef WAVESCAN_WAVETABLES_H", "#define WAVESCAN_WAVETABLES_H", "#include <stdint.h>", "",
         f"#define WT_TABLES {len(tables)}", f"#define WT_WAVES {W}", ""]
  out.append("static const char *const WT_NAMES[WT_TABLES] = {" + ", ".join(f'"{n}"' for n, _ in tables) + "};")
  out.append("")
  for lv, (harms, size) in enumerate(levels):
    out.append(f"/* level {lv}: {size} samples a wave, harmonics up to {harms} */")
    out.append(f"static const int8_t WT{lv}[WT_TABLES][WT_WAVES][{size}] = {{")
    for name, waves in tables:
      out.append(f"  {{ /* {name} */")
      for w in waves:
        bl, _ = band_limit(w, harms, size)
        bl = bl / max(1e-9, np.max(np.abs(bl))) * min(1.0, np.max(np.abs(w)) * 1.0)
        out.append("    {" + ",".join(str(v) for v in q8(bl)) + "},")
      out.append("  },")
    out.append("};")
    out.append("")
  out.append("#endif")
  OUT.write_text("\n".join(out) + "\n")
  print(f"wrote {OUT} ({len(tables)} tables)")
  if "--png" in sys.argv:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axs = plt.subplots(5, 4, figsize=(16, 14))
    for ax, (name, waves) in zip(axs.flat, tables):
      for i in range(0, 64, 4):
        ax.plot(np.arange(N) + i * 2, waves[i] * 20 + i * 3, lw=0.6)
      ax.set_title(name, fontsize=9)
      ax.axis("off")
    Path("build").mkdir(exist_ok=True)
    plt.savefig("build/wavetables.png", dpi=60)


if __name__ == "__main__":
  main()

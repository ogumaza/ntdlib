#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Fits the low-passes that stand in for the consoles' resampling filters in src/sequence_export/resampling.cpp.

usage: resampling_filters.py

AX on the Wii and the Wii U resamples a voice with one of three sets of 4-tap filters. The sets are in the DSP's
coefficient ROM (include/ntdlib/dsp/coef_rom.h): 128 phases of 4 taps each, at 0x1000 (8 kHz), 0x1200 (12 kHz) and
0x1400 (16 kHz). The Switch's audio renderer holds the same three sets. The 3DS's DSP has three polyphase kernels. Their
responses below were measured on the DSP's model: a wave of stepped sines at a sample rate of 32728 Hz, played at rates
on each side of 1 and 4/3. Each tone was measured at its output frequency and compared with the same tone at a rate of
exactly 1, which plays unfiltered.

A filter's response is fixed in cycles per wave sample (nu). The script averages each set's 128 phases into the
response at the tone's frequency. The synths lose highs too: FluidSynth and sfizz interpolate with a Catmull-Rom spline
by default. The target of each fit is the console's response minus the spline's, in dB, relative to nu = 0.005.

A note at a rate r plays its wave at r times the console's mixing rate. At an output frequency f, the console's filter
acts at nu = f / (r * mixing rate). A low-pass's cutoff is a fraction of the rate at which the note plays its wave. The
synths run their low-passes as digital filters at their output rate. Near that rate's Nyquist frequency, a digital
filter differs from its analog prototype. These cutoffs lie at 9 kHz and up. Each fit therefore uses the synth's digital
response at the synth's default rate: FluidSynth's at 44.1 kHz for the SoundFont, and sfizz's at 48 kHz for the SFZ
files.

Each fit minimizes the RMS error in dB over the top two octaves of the console's output: 160 log-spaced frequencies from
an eighth of the mixing rate to half of it, and no higher than 20 kHz. A low-pass after the synth's interpolation can
match the game only below the console's Nyquist frequency. The error is averaged over 12 log-spaced rates at which the
console plays through the filter: from 1 to 4/3 for the 12 kHz set and kernel 1, and from 4/3 to 3 for the 8 kHz set and
kernel 0. Half of the second range lies above 2. Rates there count 0.4 times as much. Both responses are floored at
-30 dB. The 16 kHz set and kernel 2 lose about as much as the spline, and take no low-pass.

The filters:
- SoundFont: FluidSynth's cookbook low-pass at a resonance of 0 (Q = 10^(-3.01 / 20)), with the cutoff held to 0.45 of
  the rate. A higher resonance would change the voice's level. FluidSynth divides the gain by the square root of Q.
- SFZ: sfizz's lpf_1p, a one-pole low-pass with its pole at exp(-2 pi cutoff / rate). sfizz holds the cutoff to
  20 kHz.

AX's sets are fitted at the mixing rate of the Wii and the Wii U, 32 kHz, and again at the Switch's 48 kHz. The 3DS's
kernels are fitted at the DSP's 32728 Hz. The script prints each fit's RMS error and, for a few points, the target and
the low-passes' responses.

Requires Python 3 and numpy.
"""
import re
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("resampling_filters.py requires numpy (pip install numpy)")

ROM = Path(__file__).resolve().parents[2] / "include" / "ntdlib" / "dsp" / "coef_rom.h"
FLOOR_DB = -30.0
CUTOFFS = np.geomspace(0.05, 1.0, 1200)  # the cutoffs tried, as fractions of the played rate

# The synths' default rates, and the highest cutoffs that they run.
FLUIDSYNTH_RATE = 44100.0
FLUIDSYNTH_Q = 10.0 ** (-3.01 / 20.0)
FLUIDSYNTH_HIGHEST = 0.45 * FLUIDSYNTH_RATE
SFIZZ_RATE = 48000.0
SFIZZ_HIGHEST = 20000.0

# The fits' points.
FREQUENCIES = 160
HIGHEST_FREQUENCY = 20000.0
RATES = 12
HIGH_RATE = 2.0
HIGH_RATE_WEIGHT = 0.4  # the weight of rates above HIGH_RATE

# The 3DS's kernels 0, 1 and 2: dB at nu = i / 64, relative to a rate of 1.
CTR_KERNELS = [
    [+0.03, +0.01, -0.04, -0.12, -0.24, -0.39, -0.58, -0.80, -1.06, -1.35, -1.68, -2.05, -2.45, -2.89, -3.37, -3.89,
     -4.45, -5.05, -5.70, -6.39, -7.12, -7.91, -8.74, -9.63, -10.57, -11.57, -12.63, -13.75, -14.94, -16.21, -17.56,
     -18.99, -20.52],
    [+0.03, +0.03, +0.00, -0.04, -0.10, -0.18, -0.27, -0.38, -0.51, -0.66, -0.82, -1.00, -1.21, -1.43, -1.67, -1.94,
     -2.22, -2.53, -2.86, -3.22, -3.60, -4.00, -4.43, -4.89, -5.37, -5.88, -6.43, -7.00, -7.61, -8.25, -8.93, -9.64,
     -10.38],
    [+0.04, +0.04, +0.03, +0.03, +0.01, -0.00, -0.02, -0.04, -0.08, -0.11, -0.16, -0.22, -0.29, -0.37, -0.46, -0.57,
     -0.70, -0.84, -1.01, -1.19, -1.40, -1.62, -1.88, -2.16, -2.46, -2.80, -3.16, -3.55, -3.98, -4.44, -4.93, -5.46,
     -6.03],
]


def phase_average(taps, nu):
    """Returns the response at each `nu` of a resampler whose rows of 4 taps (oldest first) serve the phases 0 to 1 in
    equal steps. The output lies between the second and third tap, at the row's phase."""
    nu = np.asarray(nu, dtype=float)
    flat = nu.reshape(-1)
    phases = (np.arange(taps.shape[0]) + 0.5) / taps.shape[0]
    offsets = np.arange(taps.shape[1]) - 1.0
    response = np.empty(flat.shape, dtype=complex)
    for start in range(0, flat.size, 128):
        chunk = flat[start:start + 128]
        arg = 2j * np.pi * chunk[:, None, None] * (offsets[None, None, :] - phases[None, :, None])
        response[start:start + 128] = np.mean(np.sum(taps[None, :, :] * np.exp(arg), axis=2), axis=1)
    return response.reshape(nu.shape)


def db(h):
    return 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))


def ax_sets():
    """The three sets of the coefficient ROM, as 128 x 4 arrays of taps."""
    text = ROM.read_text(encoding="utf-8")
    body = re.search(r"kCoefficientRom = \{(.*?)\};", text, re.S).group(1)
    words = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body)]
    if len(words) != 2048:
        sys.exit(f"{ROM} holds {len(words)} words, not 2048")
    values = np.array([w - 65536 if w >= 32768 else w for w in words], dtype=float) / 32768.0
    return [values[f * 512:(f + 1) * 512].reshape(128, 4) for f in range(3)]


def catmull_rom():
    """The Catmull-Rom spline's taps at 1024 phases."""
    x = (np.arange(1024) + 0.5) / 1024.0
    return np.stack([x * (-0.5 + x * (1.0 - 0.5 * x)), 1.0 + x * x * (1.5 * x - 2.5), x * (0.5 + x * (2.0 - 1.5 * x)),
                     0.5 * x * x * (x - 1.0)], axis=1)


def cookbook_db(f, cutoff, q, rate):
    """The response in dB at `f` of the cookbook low-pass at `cutoff` (both in Hz) with `q`, at `rate`."""
    w0 = 2.0 * np.pi * cutoff / rate
    cos0, alpha = np.cos(w0), np.sin(w0) / (2.0 * q)
    z = np.exp(-2j * np.pi * f / rate)
    numerator = (1.0 - cos0) / 2.0 * (1.0 + z) ** 2
    return db(numerator / ((1.0 + alpha) - 2.0 * cos0 * z + (1.0 - alpha) * z * z))


def one_pole_db(f, cutoff, rate):
    """The response in dB at `f` of sfizz's one-pole low-pass at `cutoff` (both in Hz), at `rate`."""
    p = np.exp(-2.0 * np.pi * cutoff / rate)
    return 10.0 * np.log10((1.0 - p) ** 2 / (1.0 - 2.0 * p * np.cos(2.0 * np.pi * f / rate) + p * p))


class Points:
    """The points of a fit for a console filter with `response` (dB at nu), mixed at `mixing_rate`, at `rates`: the
    output frequencies (1 x frequencies), the rates at which the notes play their waves in Hz (rates x 1), the weights
    and the floored target in dB (rates x frequencies)."""

    def __init__(self, response, rates, mixing_rate, synth_rate):
        top = min(mixing_rate / 2.0, HIGHEST_FREQUENCY, synth_rate / 2.0 * 0.999)
        self.f = np.geomspace(mixing_rate / 8.0, top, FREQUENCIES)[None, :]
        self.played = rates[:, None] * mixing_rate
        nu = self.f / self.played
        spline = catmull_rom()
        reference = response(np.array([0.005])) - db(phase_average(spline, np.array([0.005])))
        self.target = np.maximum(response(nu) - db(phase_average(spline, nu)) - reference, FLOOR_DB)
        weights = np.where(self.played > HIGH_RATE * mixing_rate, HIGH_RATE_WEIGHT, 1.0) * np.ones_like(self.f)
        self.weights = weights / weights.sum()

    def errors(self, synth_db):
        """The weighted RMS error in dB of each candidate's response (candidates x rates x frequencies)."""
        return np.sqrt(np.sum(self.weights * (np.maximum(synth_db, FLOOR_DB) - self.target) ** 2, axis=(-2, -1)))


def fit(name, above, response, rate_range, mixing_rate):
    """Fits the SoundFont and SFZ low-passes to `response`, a function of nu in dB, for the rates in `rate_range` of a
    console that mixes at `mixing_rate`. `above` is the threshold's C++ expression. Returns the ResamplingFilter's
    initializer."""
    rates = np.geomspace(rate_range[0] * 1.0001, rate_range[1], RATES)
    cutoffs = CUTOFFS[:, None, None]

    fluidsynth = Points(response, rates, mixing_rate, FLUIDSYNTH_RATE)
    errors = fluidsynth.errors(cookbook_db(fluidsynth.f, np.minimum(cutoffs * fluidsynth.played, FLUIDSYNTH_HIGHEST),
                                           FLUIDSYNTH_Q, FLUIDSYNTH_RATE))
    sf2_cutoff, sf2_error = CUTOFFS[np.argmin(errors)], errors.min()

    sfizz = Points(response, rates, mixing_rate, SFIZZ_RATE)
    errors = sfizz.errors(one_pole_db(sfizz.f, np.minimum(cutoffs * sfizz.played, SFIZZ_HIGHEST), SFIZZ_RATE))
    sfz_cutoff, sfz_error = CUTOFFS[np.argmin(errors)], errors.min()
    print(f"// {name} at {mixing_rate:.0f} Hz: SoundFont RMS {sf2_error:.2f} dB at {sf2_cutoff:.4f}, "
          f"lpf_1p RMS {sfz_error:.2f} dB at {sfz_cutoff:.4f}", file=sys.stderr)
    for rate in (rates[0], rates[RATES // 2], rates[-1]):
        row = int(np.argmin(np.abs(rates - rate)))
        played = fluidsynth.played[row, 0]
        for f in (mixing_rate / 4.0, mixing_rate / 2.0 * 0.9):
            column = int(np.argmin(np.abs(fluidsynth.f[0] - f)))
            f = fluidsynth.f[0, column]
            sf2 = cookbook_db(f, min(sf2_cutoff * played, FLUIDSYNTH_HIGHEST), FLUIDSYNTH_Q, FLUIDSYNTH_RATE)
            sfz = one_pole_db(f, min(sfz_cutoff * played, SFIZZ_HIGHEST), SFIZZ_RATE)
            print(f"//   rate {rate:.3f}, {f:5.0f} Hz: target {fluidsynth.target[row, column]:6.2f} dB, "
                  f"SoundFont {sf2:6.2f} dB, SFZ {sfz:6.2f} dB", file=sys.stderr)
    values = [above, f"{sf2_cutoff:.4f}", f"{sfz_cutoff:.4f}"]
    return "{" + ", ".join(values) + "}"


def table(name, comment, filters):
    """Formats a C++ array of ResamplingFilter."""
    return (f"// {comment}\nconstexpr std::array<ResamplingFilter, {len(filters)}> {name} = {{{{\n" +
            "".join(f"    {f},\n" for f in filters) + "}};\n")


def main():
    if len(sys.argv) != 1:
        sys.exit(__doc__.split("\n\n")[1])

    sets = ax_sets()
    ax = {rate: [fit(f"AX's {cutoff} set", above, lambda nu, s=sets[i]: db(phase_average(s, nu)), rates, rate)
                 for i, cutoff, above, rates in ((0, "8 kHz", "4.0 / 3.0", (4.0 / 3.0, 3.0)),
                                                 (1, "12 kHz", "1.0", (1.0, 4.0 / 3.0)))]
          for rate in (32000.0, 48000.0)}
    grid = np.arange(33) / 64.0
    ctr = [fit(f"the 3DS's kernel {k}", above, lambda nu, k=k: np.interp(nu, grid, CTR_KERNELS[k]), rates, 32728.0)
           for k, above, rates in ((0, "4.0 / 3.0", (4.0 / 3.0, 3.0)), (1, "1.0", (1.0, 4.0 / 3.0)))]

    print(table("kAxFilters", "AX's 8 kHz and 12 kHz sets, on the Wii and the Wii U.", ax[32000.0]))
    print(table("kSwitchFilters", "The same sets on the Switch.", ax[48000.0]))
    print(table("kCtrKernels", "The 3DS's kernels 0 and 1.", ctr))


if __name__ == "__main__":
    main()

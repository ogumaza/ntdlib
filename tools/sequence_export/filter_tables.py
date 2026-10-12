#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Generates the SoundFont and SFZ filter tables for src/sequence_export/filter_tables_<console>.cpp.

usage: filter_tables.py <console> [executable]
       filter_tables.py --poles <console>

<console> is ctr, revolution, cafe or switch. For ctr and revolution, the script reads the reference game's executable,
supplied locally. ntdlib doesn't distribute it.

- ctr (the 3DS): Pokemon X's code.bin.
- revolution (the Wii): Wii Music's sys/main.dol, from the US release.

For cafe (the Wii U) and switch, the script reads the game's tables in include/ntdlib/nwsnd/tables.h. That header
holds Animal Crossing: amiibo Festival's and WarioWare: Get It Together!'s values, with their addresses.

--poles prints the poles of AX's one-pole low-pass that the filters of revolution and cafe are fitted to.
tests/ax_test.cpp keeps them and checks ax::ComputeLpfCoefs against them.

The track low-pass (0xd8) is a one-pole filter. Each console's class below says how the game computes it. The biquad
presets are Robert Bristow-Johnson cookbook filters in Q14: low/high-pass at about -0.5 dB gain and band-pass at 0 dB
peak gain. Each coefficient set reduces to a cutoff or centre frequency, Q and gain.

SFZ: sfizz's lpf_1p has pole exp(-2 pi f / rate); lpf_2p, hpf_2p and bpf_2p use cookbook filters with Q expressed in
dB. The SFZ tables match the game's coefficients at the console's rate.

SoundFont: FluidSynth supplies one cookbook low-pass (fluid_iir_filter.c), with Q = 10^((resonance / 10 - 3.01) / 20).
That filter matches the biquad low-pass. The one-pole is approximated by fitting the cutoff at 44100 Hz against the
game's response at the console's rate. The fit minimizes the RMS error in dB, relative to 20 Hz, over 400 log-spaced
frequencies from 20 Hz to 14 kHz. Both responses are floored at -30 dB.

Requires Python 3 and numpy.
"""
import argparse
import math
import re
import struct
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("filter_tables.py requires numpy (pip install numpy)")

OUT_RATE = 44100.0  # Hz, the rate FluidSynth usually plays at
FREQS = np.geomspace(20.0, 14000.0, 400)
FLOOR_DB = -30.0
CUTOFFS = np.arange(1500, 13501, 10)  # the SoundFont cutoffs tried, in absolute cents
SFZ_LOW_PASS_BASE = 20000.0  # Hz: reference cutoff for the SFZ low-pass table (filter open)
SFZ_BIQUAD_BASE = 1000.0  # Hz: reference cutoff for the SFZ biquad table
TABLES = Path(__file__).resolve().parents[2] / "include" / "ntdlib" / "nwsnd" / "tables.h"
f32 = np.float32


def nwsnd_table(name):
    """Returns the values of the integer table `name` in include/ntdlib/nwsnd/tables.h, row after row. Comments in the
    table's body, such as an address, aren't values."""
    text = TABLES.read_text(encoding="utf-8")
    match = re.search(rf"inline constexpr \w+ {name}(?:\[\d+\])+ = \{{(.*?)\}};", text, re.S)
    if not match:
        sys.exit(f"{TABLES} has no table {name}")
    body = re.sub(r"//[^\n]*|/\*.*?\*/", "", match.group(1), flags=re.S)
    return [int(v) for v in re.findall(r"-?\d+", body)]


class Executable:
    """A console whose tables the script reads from the reference game's executable."""

    def __init__(self, path):
        self.path = path

    def biquad_rows(self, kind):
        """Returns the Q14 coefficients of each step of biquad preset `kind`."""
        address, sets, _ = self.biquads[kind]
        return [self.read(self.byte_order + "5h", address + 10 * i) for i in range(sets)]


class NwsndTables:
    """A console whose tables the script reads from include/ntdlib/nwsnd/tables.h."""

    executable = None

    def biquad_rows(self, kind):
        """Returns the Q14 coefficients of each step of biquad preset `kind`."""
        table, sets, _ = self.biquads[kind]
        values = nwsnd_table(table)
        return [tuple(values[5 * i:5 * i + 5]) for i in range(sets)]


class Ctr(Executable):
    """The 3DS. The class extracts kLpfFreq (0x56e358), nn::math's cosine table (0x560ac0), and the five biquad preset
    tables in Q14 (from 0x574e7c).

    The track low-pass (0xd8) is a one-pole filter, y = b0 x + a1 y1. CalcMonoFilterCoefficients (0x17f210) computes the
    coefficients with the table's cosine. The lowest cutoffs then come out above those of the exact cosine."""

    executable = "Pokemon X's code.bin"
    rate = 32728.0  # Hz, the DSP's output rate
    byte_order = "<"
    per_line = 10
    biquads = {  # type: (address, sets, name)
        1: (0x574e7c, 112, "low-pass"),
        2: (0x5752dc, 97, "high-pass"),
        3: (0x5756a6, 122, "band-pass 512"),
        4: (0x575b6a, 93, "band-pass 1024"),
        5: (0x575f0c, 93, "band-pass 2048"),
    }
    CODE_BASE = 0x100000  # code.bin's load address
    LPF_FREQ = 0x56e358  # kLpfFreq: 24 uint16 cutoffs in Hz. The last (16000) switches the filter off.
    COS_TABLE = 0x560ac0  # nn::math: 256 entries of sine, cosine and their differences to the next entry, as floats

    def __init__(self, path):
        super().__init__(path)
        self.code = path.read_bytes()

    def read(self, fmt, address):
        return struct.unpack_from(fmt, self.code, address - self.CODE_BASE)

    def one_pole(self, freq):
        """Computes the one-pole b0 and a1 in single precision as nn::snd does. cos(2 pi freq / 32000) comes from linear
        interpolation in nn::math's table (CosFIdx, 0x172d44). Then b = 2 - cos, c = sqrt(b^2 - 1) - b, b0 = (1 + c) *
        32768, a1 = -c * 32768. The coefficients are truncated to integers."""
        x = f32(min(freq, 16000)) * f32(0.008)
        i = int(x)
        _, cos, _, cos_delta = self.read("<4f", self.COS_TABLE + 16 * i)
        w = f32(cos) + (x - f32(i)) * f32(cos_delta)
        b = f32(2.0) - w
        c = f32(np.sqrt(b * b - f32(1.0))) - b
        return int(np.int32(f32((c + f32(1.0)) * f32(32768.0)))), -int(np.int32(f32(c * f32(32768.0))))

    def low_passes(self):
        lpf_freq = self.read("<24H", self.LPF_FREQ)
        if lpf_freq[0] != 80 or lpf_freq[-1] != 16000:
            sys.exit(f"{self.path} isn't Pokemon X's code.bin (kLpfFreq reads {lpf_freq[:3]}...)")
        filters = []
        for freq in lpf_freq[:-1]:
            b0, a1 = self.one_pole(freq)
            filters.append((b0 / 32768.0, a1 / 32768.0))
        return "kLpfFreq[0] to kLpfFreq[22]", [], filters


class Revolution(Executable):
    """The Wii. The class reads the US release of Wii Music and extracts the five biquad preset tables (from
    0x804eaa90).

    A track's low-pass (0xd8) runs on AX's one-pole filter, y = a0 x + b0 y1 in Q15. NW4R SND turns the argument into
    a scale, 1 + (argument - 64) / 64 clamped to 0..1, and the scale into a cutoff of 32000 * 2^(10 (scale - 1)) Hz,
    computed in double precision and truncated (Util::CalcLpfFreq). A cutoff of 16 kHz or more switches the filter
    off. Only arguments 0 to 57 have a filter. AXGetLpfCoefs computes the coefficients in single precision with
    the C library's cosine and square root: c = 2 - cos(2 pi f / 32000), b0 = 32768 (c - sqrt(c^2 - 1)), a0 = 32767
    - b0.

    The biquad presets (0xb4 type, 0xb5 value) come with builds from June 2008."""

    executable = "Wii Music's sys/main.dol"
    rate = 32000.0  # Hz, AX's output rate
    byte_order = ">"
    per_line = 12
    biquads = {  # type: (address in Wii Music's main.dol, sets, name)
        1: (0x804EAA90, 112, "low-pass"),
        2: (0x804EAEF0, 97, "high-pass"),
        3: (0x804EB2BC, 122, "band-pass 512"),
        4: (0x804EB780, 93, "band-pass 1024"),
        5: (0x804EBB24, 93, "band-pass 2048"),
    }
    OFF = 16000  # Hz: AX's filter is off at this cutoff and above

    def __init__(self, path):
        """Reads a DOL's sections as (address, data) pairs: seven text and eleven data sections."""
        super().__init__(path)
        d = path.read_bytes()
        if len(d) < 0x100:
            sys.exit(f"{path} is too short for a DOL")
        offsets = struct.unpack(">18I", d[0x00:0x48])
        addresses = struct.unpack(">18I", d[0x48:0x90])
        sizes = struct.unpack(">18I", d[0x90:0xD8])
        self.sections = [(a, d[o:o + s]) for o, a, s in zip(offsets, addresses, sizes) if s]

    def read(self, fmt, address):
        """Unpacks `fmt` from the DOL's memory at `address`."""
        for start, data in self.sections:
            if start <= address and address + struct.calcsize(fmt) <= start + len(data):
                return struct.unpack_from(fmt, data, address - start)
        sys.exit(f"0x{address:08X} isn't in the DOL's sections")

    @staticmethod
    def cutoff_hz(argument):
        """The cutoff NW4R SND gives AX for a 0xd8 argument (SeqTrack::UpdateChannelParam, Channel::Update and
        Util::CalcLpfFreq)."""
        scale = min(max(f32(1.0) + f32(argument - 64) / f32(64.0), f32(0.0)), f32(1.0))
        return int(32000.0 * 2.0 ** (10.0 * (float(scale) - 1.0)))

    @classmethod
    def one_pole(cls, freq):
        """AXGetLpfCoefs: a0 and b0 in Q15, for a cutoff in Hz."""
        angle = f32(f32(6.2831855) * f32(freq)) / f32(cls.rate)
        c = f32(2.0) - f32(math.cos(float(angle)))
        root = f32(math.sqrt(float(c * c - f32(1.0))))
        b0 = int(np.int32(f32(32768.0) * -(root - c))) & 0xffff
        return 0x7fff - b0, b0

    @classmethod
    def arguments(cls):
        """The 0xd8 arguments that have a filter."""
        return [a for a in range(64) if cls.cutoff_hz(a) < cls.OFF]

    @classmethod
    def poles(cls):
        """Each argument's cutoff in Hz, and AX's b0 for it."""
        return [(cls.cutoff_hz(a), cls.one_pole(cls.cutoff_hz(a))[1]) for a in cls.arguments()]

    def low_passes(self):
        first = self.read(">5h", self.biquads[1][0])
        if first[0] != first[2] or first[1] != 2 * first[0]:
            sys.exit(f"{self.path} isn't Wii Music's main.dol (the biquad low-pass's first row reads {first})")
        arguments = self.arguments()
        label = f"arguments 0 to {arguments[-1]}"
        hz = ("uint16_t", "kLowPassHz", f"AX's cutoffs in Hz, {label}.", [self.cutoff_hz(a) for a in arguments])
        filters = []
        for argument in arguments:
            a0, b0 = self.one_pole(self.cutoff_hz(argument))
            filters.append((a0 / 32768.0, b0 / 32768.0))
        return label, [hz], filters


class Cafe(NwsndTables):
    """The Wii U, with kLpfFreq and the five biquad preset tables in Q14 from nwsnd/tables.h.

    The track low-pass (0xd8) is a one-pole filter, y = a0 x + b0 y1. AX runs it on the voice at 32 kHz.
    AXComputeLpfCoefs computes b0 = (k - sqrt(k^2 - 1)) * 32768 in single precision, with k = 2 - cos(2 pi freq /
    32000), and a0 = 32767 - b0."""

    rate = 32000.0  # Hz, AX's rate for voices
    per_line = 10
    biquads = {  # type: (table in nwsnd/tables.h, sets, name)
        1: ("kBiquadLpf", 112, "low-pass"),
        2: ("kBiquadHpf", 97, "high-pass"),
        3: ("kBiquadBpf512", 122, "band-pass 512"),
        4: ("kBiquadBpf1024", 93, "band-pass 1024"),
        5: ("kBiquadBpf2048", 93, "band-pass 2048"),
    }

    @classmethod
    def one_pole(cls, freq):
        """Computes the pole b0 in Q15 as AXComputeLpfCoefs does in single precision: k = 2 - cos(w), b0 = (k -
        sqrt(k^2 - 1)) * 32768, truncated, with k^2 - 1 in one fused step."""
        w = f32(6.2831855) * f32(freq) / f32(cls.rate)
        k = f32(2.0) - f32(math.cos(float(w)))
        k2 = f32(float(k) * float(k) - 1.0)  # exact in double, then rounded once like a fused multiply-add
        return int(np.int32((k - f32(np.sqrt(k2))) * f32(32768.0)))

    @classmethod
    def poles(cls):
        """Each cutoff of kLpfFreq below 16 kHz, and AX's b0 for it."""
        return [(freq, cls.one_pole(freq)) for freq in nwsnd_table("kLpfFreq")[:-1]]

    def low_passes(self):
        filters = [((32767 - b0) / 32768.0, b0 / 32768.0) for _, b0 in self.poles()]
        return "kLpfFreq[0] to kLpfFreq[22]", [], filters


class Switch(NwsndTables):
    """The Switch, with kLpfFreq, the low-pass's one-pole coefficients for a 48 kHz renderer (kLowPass48k), the five
    biquad preset tables the Wii U's library has and the five made for 48 kHz, all in Q14, from nwsnd/tables.h.

    The track low-pass (0xd8) is a one-pole filter. nn::atk gives it to the Switch's audio renderer as a biquad from a
    table for the renderer's rate: y = b0 x + a1 y1, with b0 = 16384 - a1. The renderer runs that biquad on the voice at
    48 kHz. Types 1 to 5 are the Wii U's tables for its 32 kHz renderer. The Switch's renderer runs them at 48 kHz.
    Types 6 to 10 are made for 48 kHz."""

    rate = 48000.0  # Hz, the Switch's audio renderer's rate
    per_line = 10
    biquads = {  # type: (table in nwsnd/tables.h, sets, name)
        1: ("kBiquadLpf", 112, "low-pass"),
        2: ("kBiquadHpf", 97, "high-pass"),
        3: ("kBiquadBpf512", 122, "band-pass 512"),
        4: ("kBiquadBpf1024", 93, "band-pass 1024"),
        5: ("kBiquadBpf2048", 93, "band-pass 2048"),
        6: ("kBiquadLpf48k", 112, "low-pass made for 48 kHz"),
        7: ("kBiquadHpf48k", 97, "high-pass made for 48 kHz"),
        8: ("kBiquadBpf512_48k", 122, "band-pass 512 made for 48 kHz"),
        9: ("kBiquadBpf1024_48k", 93, "band-pass 1024 made for 48 kHz"),
        10: ("kBiquadBpf2048_48k", 93, "band-pass 2048 made for 48 kHz"),
    }

    def low_passes(self):
        lpf_freq = nwsnd_table("kLpfFreq")
        low_pass = nwsnd_table("kLowPass48k")
        filters = []
        for i in range(len(lpf_freq) - 1):
            b0, b1, b2, a1, a2 = low_pass[5 * i:5 * i + 5]
            if b1 or b2 or a2 or b0 + a1 != 16384:
                sys.exit(f"the low-pass at {lpf_freq[i]} Hz isn't a one-pole: {(b0, b1, b2, a1, a2)}")
            filters.append((b0 / 16384.0, a1 / 16384.0))
        return "kLpfFreq[0] to kLpfFreq[22]", [], filters


CONSOLES = {"ctr": Ctr, "revolution": Revolution, "cafe": Cafe, "switch": Switch}


def response_db(b, a, rate, freqs=FREQS):
    """Computes the response of y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 at `rate`. Returns dB relative to the first
    frequency, clamped to the floor."""
    z = np.exp(-2j * np.pi * freqs / rate)
    h = (b[0] + b[1] * z + b[2] * z * z) / (1.0 + a[0] * z + a[1] * z * z)
    db = 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))
    return np.maximum(db - db[0], FLOOR_DB)


def fluidsynth_db(cents, resonance):
    """Computes FluidSynth filter coefficients for cutoff in absolute cents and resonance in cB."""
    fres = min(max(8.176 * 2.0 ** (cents / 1200.0), 5.0), 0.45 * OUT_RATE)
    q = 10.0 ** ((min(max(resonance / 10.0, 0.0), 96.0) - 3.01) / 20.0)
    w = 2.0 * np.pi * fres / OUT_RATE
    alpha = np.sin(w) / (2.0 * q)
    b1 = (1.0 - np.cos(w)) / (1.0 + alpha)
    return response_db((b1 / 2.0, b1, b1 / 2.0), (-2.0 * np.cos(w) / (1.0 + alpha), (1.0 - alpha) / (1.0 + alpha)),
                       OUT_RATE)


def best_cutoff(target):
    """Finds the zero-resonance SoundFont cutoff closest to `target`. Returns the cutoff in absolute cents and the RMS
    error in dB."""
    errors = [np.sqrt(np.mean((fluidsynth_db(c, 0) - target) ** 2)) for c in CUTOFFS]
    i = int(np.argmin(errors))
    return int(CUTOFFS[i]), float(errors[i])


def decode_biquad(kind, coefs, rate):
    """Decodes a game biquad's cutoff/centre (Hz at `rate`), Q and gain (dB) from Q14 coefficients in y = b0 x + b1 x1 +
    b2 x2 + a1 y1 + a2 y2. The gain is measured at cookbook unity: DC for low-pass, Nyquist for high-pass, centre for
    band-pass."""
    b0, b1, b2, a1, a2 = (v / 16384.0 for v in coefs)
    alpha = (1.0 + a2) / (1.0 - a2)  # a2 = -(1 - alpha) / (1 + alpha)
    cos_w0 = a1 * (1.0 + alpha) / 2.0  # a1 = 2 cos w0 / (1 + alpha)
    w0 = math.acos(max(-1.0, min(1.0, cos_w0)))
    q = math.sin(w0) / (2.0 * alpha)
    if kind == 1:
        z = 1.0
    elif kind == 2:
        z = -1.0
    else:
        z = complex(math.cos(w0), -math.sin(w0))
    gain = abs((b0 + b1 * z + b2 * z * z) / (1.0 - a1 * z - a2 * z * z))
    return w0 * rate / (2.0 * math.pi), q, 20.0 * math.log10(gain)


def cents(ratio):
    return int(round(1200.0 * math.log2(ratio)))


def table(ctype, name, comment, values, per_line=10):
    """Formats a C++ table with `per_line` values per line, followed by a blank line."""
    lines = [f"// {comment}", f"constexpr {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="Generate the filter tables for src/sequence_export/filter_tables_<console>.cpp.")
    ap.add_argument("console", choices=CONSOLES, help="the console whose tables to generate")
    ap.add_argument("executable", type=Path, nargs="?",
                    help="; ".join(f"{name}: {console.executable}" for name, console in CONSOLES.items()
                                   if console.executable))
    ap.add_argument("--poles", action="store_true",
                    help="print the poles of AX's low-pass for revolution or cafe (tests/ax_test.cpp keeps them)")
    args = ap.parse_args()
    console_type = CONSOLES[args.console]
    if args.poles:
        if not hasattr(console_type, "poles"):
            ap.error("--poles is for the consoles whose low-pass AX computes: revolution and cafe")
        if args.executable:
            ap.error("--poles takes no executable")
        pairs = [f"{{{hz}, {b0}}}" for hz, b0 in console_type.poles()]
        print(table("Pole", f"k{console_type.__name__}Poles",
                    f"The poles of AX's low-pass that filter_tables.py {args.console} fits the filters to: "
                    "{cutoff in Hz, b0}.", pairs, 6))
        return
    if console_type.executable and not args.executable:
        ap.error(f"{args.console} requires {console_type.executable}")
    if not console_type.executable and args.executable:
        ap.error(f"{args.console} takes its tables from include/ntdlib/nwsnd/tables.h, not from an executable")
    console = console_type(args.executable) if console_type.executable else console_type()

    # The track low-pass's poles, and the SoundFont cutoffs fitted to them.
    label, first, filters = console.low_passes()
    for ctype, name, comment, values in first:
        print(table(ctype, name, comment, values, console.per_line))
    low_pass_sf2, low_pass_sfz, sf2_errors = [], [], []
    for gain, pole in filters:
        cutoff, error = best_cutoff(response_db((gain, 0.0, 0.0), (-pole, 0.0), console.rate))
        low_pass_sf2.append(cutoff)
        sf2_errors.append(error)
        low_pass_sfz.append(cents(-console.rate * math.log(pole) / (2.0 * math.pi) / SFZ_LOW_PASS_BASE))

    print(table("uint16_t", "kLowPassCutoff", f"The SoundFont cutoffs, {label}.", low_pass_sf2, console.per_line))
    print(table("int16_t", "kSfzLowPassCutoff", f"The SFZ cutoffs, {label}.", low_pass_sfz, console.per_line))

    # The biquad presets: cutoff (cents from SFZ_BIQUAD_BASE), Q (0.01 dB) and gain (0.01 dB) of each step. On the
    # consoles that have types 6 to 10, those types have the shapes of types 1 to 5.
    for kind, (_, sets, name) in console.biquads.items():
        shape = (kind - 1) % 5 + 1
        steps = [decode_biquad(shape, row, console.rate) for row in console.biquad_rows(kind)]
        values = []
        for freq, q, gain in steps:
            cutoff, resonance = cents(freq / SFZ_BIQUAD_BASE), round(2000.0 * math.log10(q))
            values.append(f"{{{cutoff}, {resonance}, {round(100.0 * gain)}}}")
        print(table("SfzBiquad", f"kSfzBiquad{kind}", f"Type {kind}, {name}: steps 0 to {sets - 1}.", values, 5))
        if shape == 1:
            sf2 = [f"{{{min(cents(freq / 8.176), 13500)}, {round(10.0 * (20.0 * math.log10(q) + 3.01))}}}"
                   for freq, q, _ in steps]
            name = "kBiquadLowPass" if kind == 1 else "kBiquadLowPass48k"
            # A console with two low-pass tables names the type of each.
            of_type = f" of type {kind}" if len(console.biquads) > 5 else ""
            print(table("SoundFontFilter", name, f"The SoundFont filters{of_type}, steps 0 to 111.", sf2, 6))

    print(f"// RMS difference, SoundFont low-pass: {min(sf2_errors):.2f} to {max(sf2_errors):.2f} dB", file=sys.stderr)


if __name__ == "__main__":
    main()

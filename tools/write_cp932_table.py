#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write src/disc/cp932.h, the table of Shift-JIS's double-byte characters in code page 932, from Unicode's CP932.TXT.

usage: write_cp932_table.py CP932.TXT [--output HEADER]

Download CP932.TXT from https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP932.TXT. ntdlib doesn't store
the file. It is Microsoft's table of code page 932, Windows' Shift-JIS. Each of its lines gives a code and the Unicode
code point that the code maps to. A line without a code point marks a lead byte or an unused code. The script checks
that the single-byte codes are the ones that the decoder in src/disc/encodings.cpp handles without the table. Then it
writes the double-byte codes' code points in the order of the codes. The header goes to src/disc/cp932.h unless
--output names another file. Run the script only to remake the header. The header is committed.

The data is under the Unicode License v3. The header gets that licence's SPDX identifier. THIRD-PARTY-NOTICES.md
contains the licence's notice.
"""
import argparse
import re
import sys
import textwrap
from pathlib import Path

HEADER = Path(__file__).resolve().parent.parent / "src" / "disc" / "cp932.h"
SOURCE = "https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP932.TXT"
ROW = 188  # the entries of one lead byte: trail bytes 0x40 to 0x7E and 0x80 to 0xFC
PER_LINE = 8
TRAILS = (range(0x40, 0x7F), range(0x80, 0xFD))


def fail(message):
    sys.exit("error: " + message)


def is_lead(byte):
    """Whether `byte` starts a double-byte code: 0x81 to 0x9F, or 0xE0 to 0xFC."""
    return 0x81 <= byte <= 0x9F or 0xE0 <= byte <= 0xFC


def lead_of(row):
    """The lead byte of the table's row `row`: 0x81 to 0x9F, then 0xE0 onwards."""
    return row + (0x81 if row < 0x1F else 0xC1)


def index_of(code):
    """The table's index of double-byte code `code`, or None if the code isn't one."""
    lead, trail = code >> 8, code & 0xFF
    if not is_lead(lead) or not (0x40 <= trail <= 0x7E or 0x80 <= trail <= 0xFC):
        return None
    return (lead - (0x81 if lead < 0xA0 else 0xC1)) * ROW + trail - (0x40 if trail < 0x80 else 0x41)


def decoder_single(byte):
    """What the decoder gives single byte `byte` without the table: a code point, "lead" or None for an escape."""
    if byte < 0x80:
        return byte
    if 0xA1 <= byte <= 0xDF:
        return 0xFF61 + byte - 0xA1
    return "lead" if is_lead(byte) else None


def read_table(path):
    """The file's date, and the double-byte codes' code points by table index."""
    date = None
    entries = {}
    singles = set()
    with open(path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            line = line.rstrip("\r\n")
            if line.startswith("#") or not line.strip():
                match = re.match(r"#\s*Date:\s*(\d{2})/(\d{2})/(\d{2})\s*$", line)
                if match and date is None:
                    month, day, year = (int(group) for group in match.groups())
                    date = f"{1900 + year if year >= 70 else 2000 + year}-{month:02d}-{day:02d}"
                continue

            fields = line.split("\t")
            try:
                code = int(fields[0], 16)
                unicode = int(fields[1], 16) if len(fields) > 1 and fields[1].strip() else None
            except ValueError:
                fail(f"{path}:{number}: not a code and a code point in hex")

            if code <= 0xFF:
                comment = fields[2] if len(fields) > 2 else ""
                expected = decoder_single(code)
                given = "lead" if unicode is None and "LEAD BYTE" in comment else unicode
                if given != expected:
                    fail(f"{path}:{number}: 0x{code:02X} maps to {given}, but the decoder gives {expected}")
                singles.add(code)
                continue

            index = index_of(code)
            if index is None:
                fail(f"{path}:{number}: 0x{code:04X} isn't a double-byte code")
            if unicode is None:
                fail(f"{path}:{number}: 0x{code:04X} has no code point")
            if not 0 < unicode <= 0xFFFF or 0xD800 <= unicode < 0xE000:
                fail(f"{path}:{number}: U+{unicode:04X} isn't a character in the BMP")
            if index in entries:
                fail(f"{path}:{number}: 0x{code:04X} appears twice")
            entries[index] = unicode

    if len(singles) != 256:
        fail(f"{path} gives {len(singles)} of the 256 single bytes")
    if not entries:
        fail(f"{path} contains no double-byte codes")
    return date, entries


def comment(*paragraphs):
    """The paragraphs as a C++ comment of up to 120 columns, with an empty comment line between them."""
    lines = []
    for paragraph in paragraphs:
        if lines:
            lines.append("//")
        wrapped = textwrap.wrap(paragraph, 117, break_long_words=False, break_on_hyphens=False)
        lines += ["// " + line for line in wrapped]
    return lines


def write_header(date, entries, output):
    rows = max(entries) // ROW + 1
    dated = f", dated {date}" if date else ""
    lines = ["// SPDX-License-Identifier: Unicode-3.0", ""]
    lines += comment(
        "The double-byte characters of code page 932, Windows' Shift-JIS, in Unicode, in the order of their codes. "
        "tools/write_cp932_table.py writes this file from CP932.TXT, Microsoft's table of code page 932 as Unicode "
        f"publishes it{dated} ({SOURCE}). The data is one of Unicode's data files, under the Unicode License v3. "
        "THIRD-PARTY-NOTICES.md contains the licence's notice.",
        f"A double-byte code has a lead byte (0x81 to 0x9F, or 0xE0 to 0x{lead_of(rows - 1):02X}) and a trail byte "
        "(0x40 to 0x7E, or 0x80 to 0xFC). The code's entry is at (lead - (lead < 0xA0 ? 0x81 : 0xC1)) * 188 + trail - "
        "(trail < 0x80 ? 0x40 : 0x41). An entry of 0 maps the code to no character. The comment on each line gives the "
        "code of the line's first entry.")
    lines += [
        "",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace ntdlib::disc::detail",
        "{",
        "",
        "// clang-format off",
        f"inline constexpr std::array<uint16_t, {rows * ROW}> kCp932 = {{",
    ]
    for row in range(rows):
        for trails in TRAILS:
            trails = list(trails)
            for start in range(0, len(trails), PER_LINE):
                chunk = trails[start:start + PER_LINE]
                index = row * ROW + chunk[0] - (0x40 if chunk[0] < 0x80 else 0x41)
                values = " ".join(f"0x{entries.get(index + i, 0):04X}," for i in range(len(chunk)))
                lines.append(f"    {values:<{PER_LINE * 8 - 1}} // 0x{lead_of(row):02X}{chunk[0]:02X}")
    lines += [
        "};",
        "// clang-format on",
        "",
        "} // namespace ntdlib::disc::detail",
    ]
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n".join(lines) + "\n")
    print(f"wrote {output}: {len(entries)} characters in {rows * ROW} entries")


def main():
    parser = argparse.ArgumentParser(description="Write src/disc/cp932.h from Unicode's CP932.TXT.")
    parser.add_argument("table", type=Path, help="CP932.TXT, downloaded from " + SOURCE)
    parser.add_argument("--output", type=Path, default=HEADER, help="the header to write (default: %(default)s)")
    args = parser.parse_args()

    date, entries = read_table(args.table)
    write_header(date, entries, args.output)


if __name__ == "__main__":
    main()

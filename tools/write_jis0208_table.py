#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write src/disc/jis0208.h, the table of Shift-JIS's double-byte characters, from Unicode's JIS0208.TXT.

usage: write_jis0208_table.py JIS0208.TXT [--output HEADER]

Download JIS0208.TXT from https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/JIS0208.TXT. ntdlib doesn't
store the file. Each of its lines gives a character of JIS X 0208 in three columns: the Shift-JIS code, the JIS X 0208
code and the Unicode code point. The script checks that each Shift-JIS code is the one that the JIS X 0208 code gives.
Then it writes the code points in the order of the Shift-JIS codes. The header goes to src/disc/jis0208.h unless
--output names another file. Run the script only to remake the header. The header is committed.

The data is under the Unicode License v3. The header gets that licence's SPDX identifier. THIRD-PARTY-NOTICES.md
contains the licence's notice.
"""
import argparse
import re
import sys
import textwrap
from pathlib import Path

HEADER = Path(__file__).resolve().parent.parent / "src" / "disc" / "jis0208.h"
SOURCE = "https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/JIS0208.TXT"
ROW = 188  # the entries of one lead byte: trail bytes 0x40 to 0x7E and 0x80 to 0xFC
PER_LINE = 8
TRAILS = (range(0x40, 0x7F), range(0x80, 0xFD))


def fail(message):
    sys.exit("error: " + message)


def lead_of(row):
    """The lead byte of the table's row `row`: 0x81 to 0x9F, then 0xE0 onwards."""
    return row + (0x81 if row < 0x1F else 0xC1)


def index_of(code):
    """The table's index of Shift-JIS code `code`, or None if the code isn't a double-byte code."""
    lead, trail = code >> 8, code & 0xFF
    if not (0x81 <= lead <= 0x9F or 0xE0 <= lead <= 0xFC) or not (0x40 <= trail <= 0x7E or 0x80 <= trail <= 0xFC):
        return None
    return (lead - (0x81 if lead < 0xA0 else 0xC1)) * ROW + trail - (0x40 if trail < 0x80 else 0x41)


def shift_jis_of(jis):
    """The Shift-JIS code of JIS X 0208 code `jis`, or None if it isn't one. Rows and cells run from 0x21 to 0x7E."""
    row, cell = jis >> 8, jis & 0xFF
    if not (0x21 <= row <= 0x7E and 0x21 <= cell <= 0x7E):
        return None
    lead = (row + 1) // 2 + (0x70 if row <= 0x5E else 0xB0)
    if row % 2:
        trail = cell + (0x1F if cell <= 0x5F else 0x20)
    else:
        trail = cell + 0x7E
    return lead << 8 | trail


def read_table(path):
    """The file's date, and its code points by table index."""
    date = None
    entries = {}
    with open(path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            line = line.rstrip("\n")
            if line.startswith("#") or not line.strip():
                match = re.match(r"#\s*Date:\s*(\d{4}-\d{2}-\d{2})", line)
                if match and date is None:
                    date = match.group(1)
                continue

            fields = line.split("\t")
            try:
                code, jis, unicode = (int(field, 16) for field in fields[:3])
            except ValueError:
                fail(f"{path}:{number}: not three hex columns")
            index = index_of(code)
            if index is None or shift_jis_of(jis) != code:
                fail(f"{path}:{number}: 0x{code:04X} isn't the Shift-JIS code of JIS X 0208's 0x{jis:04X}")
            if not 0 < unicode <= 0xFFFF or 0xD800 <= unicode < 0xE000:
                fail(f"{path}:{number}: U+{unicode:04X} isn't a character in the BMP")
            if index in entries:
                fail(f"{path}:{number}: 0x{code:04X} appears twice")
            entries[index] = unicode

    if not entries:
        fail(f"{path} contains no mappings")
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
        "JIS X 0208's characters in Unicode, in the order of their Shift-JIS codes. tools/write_jis0208_table.py "
        f"writes this file from Unicode's JIS0208.TXT{dated} ({SOURCE}). Copyright (c) 2015 Unicode, Inc. The data is "
        "under the Unicode License v3. THIRD-PARTY-NOTICES.md contains the licence's notice.",
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
        f"inline constexpr std::array<uint16_t, {rows * ROW}> kJis0208 = {{",
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
    parser = argparse.ArgumentParser(description="Write src/disc/jis0208.h from Unicode's JIS0208.TXT.")
    parser.add_argument("table", type=Path, help="JIS0208.TXT, downloaded from " + SOURCE)
    parser.add_argument("--output", type=Path, default=HEADER, help="the header to write (default: %(default)s)")
    args = parser.parse_args()

    date, entries = read_table(args.table)
    write_header(date, entries, args.output)


if __name__ == "__main__":
    main()

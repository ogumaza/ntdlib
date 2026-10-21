#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a game-mode driver for the GameCube or the Wii (a C file) and write it as a C++ header.

A rip format's game mode loads the driver into the emulated console's memory. The driver calls the game's sound code.
The driver can include ntdlib's shared driver sources (drivers/), such as "jaudio/z2.h".
The script compiles the driver with a clang that has the PowerPC target. LLVM's releases and Homebrew's llvm have that
target. Apple's clang doesn't. The script links the object itself. No PowerPC linker is required. Run the script only
when the driver changes. The generated header is committed. Building the format itself requires no PowerPC toolchain.

usage: build_ppc_driver.py --source DRIVER.c --output HEADER.h --namespace NAMESPACE --product NAME
                           [--command TEXT] [--clang PATH]
"""
import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

# The drivers' shared sources, in ntdlib's drivers/. A driver includes them by path, such as "jaudio/z2.h".
SHARED = Path(__file__).resolve().parent.parent / "drivers"

# Where the driver is loaded: near the top of MEM1, below the disc's file system table and outside the OS's arena. The
# driver's entry is at BASE, and its parameter block is at PARAMS (the format's specification, "Driver").
BASE = 0x81700000
PARAMS = 0x81700010

CFLAGS = [
    "--target=powerpc-unknown-eabi", "-mcpu=750", "-msoft-float", "-Os", "-ffreestanding", "-fno-builtin",
    "-nostdlib", "-fno-pic", "-fno-exceptions", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-Wall",
    "-Wextra", "-Werror",
]

# The relocation types that clang emits for code built without PIC or small data.
R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
R_PPC_REL14 = 11
R_PPC_REL32 = 26

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHF_ALLOC = 2


class Elf:
    """The parts of a 32-bit big-endian ELF object that the linker uses."""

    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
            sys.exit("error: not a 32-bit big-endian ELF object")
        shoff, = struct.unpack_from(">I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x2E)
        self.data = data
        self.sections = []
        for i in range(shnum):
            name, kind, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
                ">IIIIIIIIII", data, shoff + i * shentsize)
            self.sections.append(dict(name=name, kind=kind, flags=flags, offset=offset, size=size, link=link,
                                      info=info, align=max(align, 1)))
        names = self.sections[shstrndx]
        for section in self.sections:
            section["name"] = self.string(names, section["name"])

    def string(self, table, offset):
        start = table["offset"] + offset
        return self.data[start:self.data.index(b"\0", start)].decode()

    def symbols(self):
        """(name, value, section index) for each symbol."""
        table = next(s for s in self.sections if s["kind"] == SHT_SYMTAB)
        strings = self.sections[table["link"]]
        result = []
        for at in range(table["offset"], table["offset"] + table["size"], 16):
            name, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", self.data, at)
            result.append((self.string(strings, name), value, shndx))
        return result


def link(elf):
    """Lays the allocated sections out from BASE: the entry first, the parameters at PARAMS, then the code and data,
    and the BSS last. Applies the relocations. Returns the image, the address of the end of its BSS, and the symbols'
    addresses."""
    def rank(i):
        name = elf.sections[i]["name"]
        return (name != ".text.entry", name != ".params", elf.sections[i]["kind"] == SHT_NOBITS)

    order = sorted((i for i, s in enumerate(elf.sections) if s["flags"] & SHF_ALLOC and s["size"]), key=rank)
    addresses = {}
    at = BASE
    for i in order:
        section = elf.sections[i]
        at = (at + section["align"] - 1) & ~(section["align"] - 1)
        if section["name"] == ".params":
            at = PARAMS
        addresses[i] = at
        at += section["size"]
    end = at
    image = bytearray(max((addresses[i] + elf.sections[i]["size"] for i in order
                           if elf.sections[i]["kind"] != SHT_NOBITS), default=BASE) - BASE)
    for i in order:
        section = elf.sections[i]
        if section["kind"] != SHT_NOBITS:
            start = addresses[i] - BASE
            image[start:start + section["size"]] = elf.data[section["offset"]:section["offset"] + section["size"]]

    symbols = elf.symbols()
    apply_relocations(elf, image, addresses, symbols)
    named = {name: addresses[shndx] + value for name, value, shndx in symbols if name and shndx in addresses}
    return bytes(image), end, named


def apply_relocations(elf, image, addresses, symbols):
    """Applies the relocations of the sections at `addresses` to `image`. The image starts at BASE. `symbols` is the
    list that Elf.symbols returns."""
    def symbol_address(index):
        name, value, shndx = symbols[index]
        if shndx not in addresses:
            sys.exit(f"error: the driver refers to {name or 'a symbol'}. The driver doesn't define it.")
        return addresses[shndx] + value

    for rela in (s for s in elf.sections if s["kind"] == SHT_RELA):
        target = rela["info"]
        if target not in addresses:
            continue
        for at in range(rela["offset"], rela["offset"] + rela["size"], 12):
            offset, info, addend = struct.unpack_from(">IIi", elf.data, at)
            kind = info & 0xFF
            value = (symbol_address(info >> 8) + addend) & 0xFFFFFFFF
            place = addresses[target] + offset
            where = place - BASE
            if kind == R_PPC_ADDR32:
                struct.pack_into(">I", image, where, value)
            elif kind == R_PPC_REL32:
                struct.pack_into(">I", image, where, (value - place) & 0xFFFFFFFF)
            elif kind == R_PPC_ADDR16_LO:
                struct.pack_into(">H", image, where, value & 0xFFFF)
            elif kind == R_PPC_ADDR16_HI:
                struct.pack_into(">H", image, where, value >> 16)
            elif kind == R_PPC_ADDR16_HA:
                struct.pack_into(">H", image, where, ((value + 0x8000) >> 16) & 0xFFFF)
            elif kind in (R_PPC_REL24, R_PPC_REL14):
                delta = (value - place) & 0xFFFFFFFF
                mask = 0x03FFFFFC if kind == R_PPC_REL24 else 0xFFFC
                word, = struct.unpack_from(">I", image, where)
                struct.pack_into(">I", image, where, (word & ~mask & 0xFFFFFFFF) | (delta & mask))
            else:
                sys.exit(f"error: relocation type {kind} isn't supported")


def render_header(blob, entry, params, end, command, source, product, namespace):
    """Returns the header for a linked driver image, formatted to match clang-format. `command` regenerates the
    header from `source`. `product` is the format's name. The constants go in `namespace`."""
    comment = f"Generated by {command} from {source}; do not edit. The {product} guest driver, loaded at 0x{BASE:08x}."
    lines = [
        "// SPDX-License-Identifier: MIT",
        "",
        *textwrap.wrap(comment, width=120, initial_indent="// ", subsequent_indent="// ", break_long_words=False,
                       break_on_hyphens=False),
        "",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        f"namespace {namespace}",
        "{",
        "",
        f"inline constexpr uint32_t kDriverBase = 0x{BASE:08x};",
        f"inline constexpr uint32_t kDriverEntry = 0x{entry:08x};",
        f"inline constexpr uint32_t kDriverParams = 0x{params:08x};",
        f"inline constexpr uint32_t kDriverEnd = 0x{end:08x}; // the end of the bss",
        "inline constexpr uint8_t kDriverBlob[] = {",
    ]
    for i in range(0, len(blob), 19):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in blob[i:i + 19]) + ",")
    lines += ["};", "", f"}} // namespace {namespace}"]
    return "\n".join(lines) + "\n"


def find_clang(given):
    candidates = [given] if given else ["/opt/homebrew/opt/llvm/bin/clang", "/usr/local/opt/llvm/bin/clang", "clang"]
    for candidate in candidates:
        path = shutil.which(candidate) or (candidate if Path(candidate).exists() else None)
        if path and "PowerPC" in subprocess.run([path, "--print-targets"], capture_output=True, text=True).stdout:
            return path
    sys.exit("error: no clang with the PowerPC target; pass --clang")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", required=True, help="the driver's C file")
    parser.add_argument("--output", required=True, help="the header to write")
    parser.add_argument("--namespace", required=True, help="the C++ namespace of the header's constants")
    parser.add_argument("--product", required=True, help="the format's name, for the header's comment")
    parser.add_argument("--command", default="build_ppc_driver.py",
                        help="the command that regenerates the header, for its comment")
    parser.add_argument("--clang", help="a clang with the PowerPC target")
    args = parser.parse_args()

    clang = find_clang(args.clang)
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "driver.o"
        subprocess.run([clang, *CFLAGS, "-I", str(SHARED), "-c", args.source, "-o", str(obj)], check=True)
        image, end, symbols = link(Elf(obj.read_bytes()))

    entry = symbols["driver_entry"]
    if entry != BASE or symbols["driver_params"] != PARAMS:
        sys.exit("error: driver_entry or driver_params isn't where the format puts it")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    header = render_header(image, entry, symbols["driver_params"], end, command=args.command, source=args.source,
                           product=args.product, namespace=args.namespace)
    output.write_text(header, encoding="utf-8")
    print(f"{args.output}: {len(image)} bytes, params at 0x{symbols['driver_params']:08x}, bss to 0x{end:08x}")


if __name__ == "__main__":
    main()

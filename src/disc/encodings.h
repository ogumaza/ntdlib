// SPDX-License-Identifier: MIT

// The text encodings of GameCube and Wii discs, decoded to UTF-8. The names in a disc's file system are Shift-JIS. The
// title in a disc's header is Shift-JIS on a Japanese disc and Windows-1252 on any other (GameFiles::Title).

#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ntdlib::disc::detail
{

// Decodes Shift-JIS text to UTF-8. Bytes 0x00 to 0x7F are ASCII. Bytes 0xA1 to 0xDF are half-width katakana (U+FF61
// to U+FF9F). A lead byte (0x81 to 0x9F, or 0xE0 to 0xFC) and the byte after it form a double-byte code. Unicode's
// table of JIS X 0208 maps the code to a character (jis0208.h). A byte that doesn't decode becomes %XX, with capital
// hex digits. Such bytes are 0x80, 0xA0, 0xFD to 0xFF, and a lead byte at the end of the text. A double-byte code that
// the table lacks becomes %XX%XX. If its second byte is ASCII, only the lead byte becomes %XX. The ASCII byte then
// decodes as itself.
std::string DecodeShiftJis(std::span<const uint8_t> text);

// Decodes Windows-1252 text to UTF-8. Bytes 0x00 to 0x7F are ASCII. Bytes 0xA0 to 0xFF are Latin-1 (U+00A0 to U+00FF).
// Microsoft's table of code page 1252 maps 0x80 to 0x9F. The five codes that the table leaves undefined become %XX,
// with capital hex digits.
std::string DecodeWindows1252(std::span<const uint8_t> text);

// Appends the character `code` to `out` in UTF-8. `code` is a Unicode code point up to U+10FFFF, and not a surrogate
// (U+D800 to U+DFFF).
void AppendUtf8(std::string& out, uint32_t code);

} // namespace ntdlib::disc::detail

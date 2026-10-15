// SPDX-License-Identifier: MIT

// The text encodings of GameCube and Wii discs (see encodings.h).

#include "disc/encodings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "disc/cp932.h"

namespace ntdlib::disc::detail
{
namespace
{

// Windows-1252's characters at 0x80 to 0x9F, from Microsoft's table of code page 1252. 0 marks the five codes that the
// table leaves undefined: 0x81, 0x8D, 0x8F, 0x90 and 0x9D.
constexpr std::array<uint16_t, 32> kWindows1252 = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000, // 0x88
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178, // 0x98
};

// Appends `byte` as %XX.
void AppendEscape(std::string& out, uint8_t byte)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    out += '%';
    out += kDigits[byte >> 4];
    out += kDigits[byte & 15];
}

// The character of the double-byte code `lead`, `trail` in code page 932, or 0 if there's none. `lead` is 0x81 to 0x9F,
// or 0xE0 to 0xFC.
uint16_t Cp932(uint8_t lead, uint8_t trail)
{
    if (trail < 0x40 || trail == 0x7F || trail > 0xFC)
    {
        return 0;
    }

    const size_t index = size_t{lead - (lead < 0xA0 ? 0x81u : 0xC1u)} * 188 + trail - (trail < 0x80 ? 0x40u : 0x41u);

    return index < kCp932.size() ? kCp932[index] : 0;
}

} // namespace

std::string DecodeShiftJis(std::span<const uint8_t> text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const uint8_t byte = text[i];
        if (byte < 0x80)
        {
            out += static_cast<char>(byte);
            continue;
        }
        if (byte >= 0xA1 && byte <= 0xDF)
        {
            AppendUtf8(out, 0xFF61 + (byte - 0xA1u));
            continue;
        }

        const bool lead = (byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC);
        if (!lead || i + 1 == text.size())
        {
            AppendEscape(out, byte);
            continue;
        }

        // For a code that the table lacks, an ASCII second byte decodes as itself.
        const uint8_t trail = text[i + 1];
        if (const uint16_t code = Cp932(byte, trail))
        {
            AppendUtf8(out, code);
            ++i;
        }
        else
        {
            AppendEscape(out, byte);
            if (trail >= 0x80)
            {
                AppendEscape(out, trail);
                ++i;
            }
        }
    }

    return out;
}

std::string DecodeWindows1252(std::span<const uint8_t> text)
{
    std::string out;
    for (const uint8_t byte : text)
    {
        const bool c1 = byte >= 0x80 && byte < 0xA0;
        const uint32_t code = c1 ? kWindows1252[byte - 0x80u] : byte;
        if (c1 && code == 0)
        {
            AppendEscape(out, byte);
        }
        else
        {
            AppendUtf8(out, code);
        }
    }

    return out;
}

void AppendUtf8(std::string& out, uint32_t code)
{
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | code >> 6);
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | code >> 12);
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | code >> 18);
        out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

} // namespace ntdlib::disc::detail

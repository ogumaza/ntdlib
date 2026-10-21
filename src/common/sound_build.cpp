// SPDX-License-Identifier: MIT

#include "ntdlib/sound_build.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "xsflib/bytes.h"

#include "ntdlib/dol.h"

namespace ntdlib
{
namespace
{

// The sizes of the PBs that AX's microcode reads by DMA, in bytes.
constexpr uint16_t kPbSizeOctober2006 = 0x120;
constexpr uint16_t kPbSize = 0x140;

// Two NOPs, then seven jumps (0x029F) to 0x0Fxx.
bool IsAxVectors(std::span<const uint8_t> words)
{
    if (xsflib::LoadBe32(words.data()) != 0)
    {
        return false;
    }

    for (int i = 0; i < 7; i++)
    {
        const uint8_t* jump = words.data() + 4 + i * 4;
        if (xsflib::LoadBe16(jump) != 0x029f || (xsflib::LoadBe16(jump + 2) & 0xff00) != 0x0f00)
        {
            return false;
        }
    }

    return true;
}

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

} // namespace

std::optional<Microcode> FindMicrocode(std::span<const uint8_t> dol, const MicrocodeOptions& options)
{
    const std::optional<Dol> parsed = Dol::Parse(dol, options.skip_damaged_sections);
    if (!parsed)
    {
        return std::nullopt;
    }

    // The microcode is stored as data in one of the DOL's sections. The search checks only 8-byte boundaries for the
    // microcode's start.
    constexpr std::size_t kBytes = kMicrocodeWords * 2;
    for (const Dol::Section& section : parsed->Sections())
    {
        for (std::size_t at = 0; at + kBytes <= section.data.size(); at += 8)
        {
            if (!IsAxVectors(section.data.subspan(at, 32)))
            {
                continue;
            }

            Microcode microcode;
            microcode.address = section.address + static_cast<uint32_t>(at);
            microcode.words.resize(kMicrocodeWords);
            for (std::size_t i = 0; i < kMicrocodeWords; i++)
            {
                microcode.words[i] = xsflib::LoadBe16(&section.data[at + i * 2]);
            }

            return microcode;
        }
    }

    return std::nullopt;
}

AxBuild IdentifyBuild(const Microcode& microcode)
{
    // The microcode reads each voice's PB by DMA. The instruction SI @DSBL, #length sets the DMA's length (0x16CB, then
    // the length in bytes).
    bool reads_0x120 = false;
    bool reads_0x140 = false;
    for (std::size_t i = 0; i + 1 < microcode.words.size(); i++)
    {
        if (microcode.words[i] == 0x16cb)
        {
            reads_0x120 |= microcode.words[i + 1] == kPbSizeOctober2006;
            reads_0x140 |= microcode.words[i + 1] == kPbSize;
        }
    }

    return reads_0x120 && !reads_0x140 ? AxBuild::kOctober2006 : AxBuild::kDecember2006;
}

SndBuild FindSndBuild(std::span<const uint8_t> dol, const BannerOptions& options)
{
    // The banner, then the build's date in the form of C's __DATE__: "Mar  8 2008".
    const std::string_view text(reinterpret_cast<const char*>(dol.data()), dol.size());
    const std::size_t banner = text.find("<< NW4R    - SND ");
    const std::size_t build = banner == std::string_view::npos ? banner : text.find("build: ", banner);
    if (build == std::string_view::npos || build - banner > 32 || build + 18 > text.size())
    {
        return {};
    }

    static constexpr std::string_view kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const std::string_view date = text.substr(build + 7, 11);
    const std::size_t month = kMonths.find(date.substr(0, 3));
    if (month == std::string_view::npos || month % 3 != 0 || !std::all_of(date.begin() + 7, date.end(), IsDigit))
    {
        return {};
    }

    // The day is a space and a digit before the 10th.
    if (options.check_day &&
        !std::all_of(date.begin() + 4, date.begin() + 6, [](char c) { return c == ' ' || IsDigit(c); }))
    {
        return {};
    }

    // The value of the date's character `i`. The day's space counts as 0.
    const auto digit = [&date](std::size_t i)
    {
        return date[i] == ' ' ? 0u : static_cast<uint32_t>(date[i] - '0');
    };
    const uint32_t year = digit(7) * 1000 + digit(8) * 100 + digit(9) * 10 + digit(10);

    return {year * 10000 + static_cast<uint32_t>(month / 3 + 1) * 100 + digit(4) * 10 + digit(5)};
}

} // namespace ntdlib

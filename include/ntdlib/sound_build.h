// SPDX-License-Identifier: MIT

// The builds of the Wii's sound code that a game's main.dol links. AX's microcode identifies the build of AX, and NW4R
// SND's version banner dates the build of NW4R SND.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib
{

// AX's microcode in a game's main.dol. The microcode is 4096 words long and fills the DSP's instruction RAM. After two
// NOPs at the reset vector, its exception vectors are seven jumps to the top of IRAM (0x0Fxx).
struct Microcode
{
    std::vector<uint16_t> words;
    uint32_t address = 0; // the cached address the game loads it from
};

// The size of the IRAM image of AX's DSP task.
constexpr std::size_t kMicrocodeWords = 0x1000;

// How FindMicrocode reads a damaged main.dol.
struct MicrocodeOptions
{
    // A section that the DOL's header places outside the file is left out, and the search goes on in the others.
    // Without this, such a file holds no microcode. Only a damaged main.dol has such a section.
    bool skip_damaged_sections = false;
};

// Finds AX's microcode in a main.dol. Returns nothing if the DOL has none.
std::optional<Microcode> FindMicrocode(std::span<const uint8_t> dol, const MicrocodeOptions& options = {});

// The builds of AX's CPU side. The microcode is built with the CPU side. The microcode therefore identifies the build
// (IdentifyBuild).
enum class AxBuild
{
    kOctober2006,  // 64 voices, 0x120-byte PBs and other command numbers
    kDecember2006, // and the later builds: they take the same command lists and PBs
};

// The build of AX that `microcode` comes with, by the size of the PBs that it reads: 0x120 bytes in the build of
// October 2006, and 0x140 bytes in the build of December 2006 and the later ones.
AxBuild IdentifyBuild(const Microcode& microcode);

// A build of NW4R SND, by the date in its version banner in main.dol ("<< NW4R    - SND \tfinal   build: Mar  8 2008
// 20:59:14 ..."), as YYYYMMDD. 0 means a build without a banner. The models of NW4R SND follow Wii Sports' build
// (20070608). Where builds from 20080308 (Mario Kart Wii) on differ, the models check SinceMarch2008. Where builds from
// 20080620 (Wii Music) on differ again, the models check SinceJune2008. Builds from 20081120 differ again
// (SinceNovember2008), and builds from 20090924 once more (SinceSeptember2009). A build between two of these counts as
// the older kind.
//
// The last two dates are those of the earliest builds known to have the change. The latest builds known without the
// changes are of 20080919 and 20081120. No build from between those dates is known yet. Such a build may have the
// change or not. It counts as the older kind. Every known build then plays as its code does, and a build from a gap
// plays as the known build before the gap. A build from a gap that turns up later may move the date.
//
// WarioWare: Smooth Moves links a build from 2006 without a banner. That build comes with AX of October 2006. AX's
// microcode identifies the build (IdentifyBuild). Where that build differs, the models check October2006.
struct SndBuild
{
    bool October2006() const
    {
        return october_2006;
    }

    // Builds from March 2008 add time prefixes, the envelope's hold, monophony, note-off types, key groups, sweep_pitch
    // in 64ths and a tempo limit.
    bool SinceMarch2008() const
    {
        return date >= 20080308;
    }

    // Builds from June 2008 add the velocity range, the biquad filter and the envelope reset.
    bool SinceJune2008() const
    {
        return date >= 20080620;
    }

    // Builds from November 2008 take the low-pass filter's cutoff from a table, as the Wii U's library does
    // (nwsnd::RevolutionPolicy::LpfFreq).
    bool SinceNovember2008() const
    {
        return date >= 20081120;
    }

    // Builds from September 2009 move the pitch bend (0xC4) to its value over a time prefix's ticks. They also let a
    // voice's volume and its envelope's gain go above full level: the voice holds them only at 0.
    bool SinceSeptember2009() const
    {
        return date >= 20090924;
    }

    uint32_t date = 0;
    bool october_2006 = false; // the build that comes with AX of October 2006
};

// How FindSndBuild reads the banner's date.
struct BannerOptions
{
    // A day that isn't one or two digits gives no date. Without this check, the day's characters give a date whatever
    // they are. C's __DATE__ writes the day in digits. The rules therefore differ only for a damaged banner.
    bool check_day = false;
};

// The build of NW4R SND that a main.dol links, from its banner. The date is all that the banner shows: AX's microcode
// identifies the build of October 2006.
SndBuild FindSndBuild(std::span<const uint8_t> dol, const BannerOptions& options = {});

} // namespace ntdlib

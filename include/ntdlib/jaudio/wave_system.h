// SPDX-License-Identifier: MIT

// A wave system (WSYS) of a JAudio1 or JAudio2 start-up archive (AAF or BAA): the waves that a bank plays, with the AW
// files that hold their samples.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::jaudio
{

// Sample formats of a wave (JASWaveInfo's format).
enum class WaveFormat : uint8_t
{
    kAdpcm4 = 0, // 4-bit ADPCM, 9 bytes per 16 samples (AFC)
    kAdpcm2 = 1, // 2-bit ADPCM, 5 bytes per 16 samples
    kPcm8 = 2,
    kPcm16 = 3,
};

// A wave of a wave system: its format, the key it plays at its rate, and where its samples are in its AW file.
struct WaveInfo
{
    WaveFormat format = WaveFormat::kAdpcm4;
    uint8_t base_key = 60;
    float sample_rate = 32000.0f;
    uint32_t offset = 0; // in the AW file
    uint32_t size = 0;
    bool loop = false;
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past its last sample
    uint32_t sample_count = 0;

    // An ADPCM decoder's history at the loop start: the samples before it. The DSP program takes them up again at each
    // loop.
    int16_t loop_last = 0;
    int16_t loop_penult = 0;

    std::string archive; // the AW file's name
};

// A group of a wave system: one AW file and the ids of its waves. A game loads the file into ARAM as a whole. A wave
// can be in more than one group. A bank plays a wave from whichever of its groups is loaded.
struct WaveGroup
{
    std::string archive; // the AW file's name
    std::vector<uint16_t> waves;
};

// A wave system (WSYS): the waves a bank plays, by wave id, and its groups.
struct WaveSystem
{
    uint32_t number = 0; // the number that banks name it by
    std::map<uint16_t, WaveInfo> waves;
    std::vector<WaveGroup> groups; // in the order of the system's WINF block
};

// Reads the wave system in `file`. Banks name the system by `number`. A system whose tag is "SYSW" is little-endian.
// Each of its fields is then stored in the width of its type. The Switch's releases of GameCube and Wii games store
// their wave systems that way. Throws FormatError if it's damaged.
WaveSystem ReadWaveSystem(std::span<const uint8_t> file, uint32_t number);

// A wave that a sound plays: the number of its wave system, and its id.
struct WaveUse
{
    auto operator<=>(const WaveUse&) const = default;

    uint16_t system = 0;
    uint16_t id = 0;
};

// The groups that hold `waves`, each as (system << 16) | group, in ascending order. For each system, the first group
// chosen holds the most of the waves. The next holds the most of the rest, and so on. Of two groups that hold as many,
// the earlier one is chosen. Waves that no group holds are left out. A game that loads its groups as its scenes require
// them plays a sound with these groups loaded.
std::vector<uint32_t> WaveGroups(const std::vector<WaveSystem>& systems, const std::vector<WaveUse>& waves);

// Whether one of `groups`, each (system << 16) | group as WaveGroups gives them, holds the wave.
bool GroupsHold(const std::vector<WaveSystem>& systems, std::span<const uint32_t> groups, WaveUse wave);

// How many of `played` the chosen groups lack: waves that a group of their system holds, but none of `groups`, each
// (system << 16) | group as WaveGroups gives them. A wave that no group holds isn't counted. The game has no samples
// for it, and its notes are silent on the console too.
std::size_t MissingWaves(const std::vector<WaveSystem>& systems, std::span<const uint32_t> groups,
                         std::span<const WaveUse> played);

// The AW files that hold `groups`, each (system << 16) | group, each file once, in the order of their names.
std::vector<std::string> GroupArchives(const std::vector<WaveSystem>& systems, std::span<const uint32_t> groups);

} // namespace ntdlib::jaudio

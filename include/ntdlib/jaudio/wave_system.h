// SPDX-License-Identifier: MIT

// A wave system (WSYS) of a JAudio1 or JAudio2 start-up archive (AAF or BAA): the waves that a bank plays, with the AW
// files that hold their samples.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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

// A scene of a wave system (SCNE, in its WBCT chunk), with the values that the system stores. Each of its three lists
// holds a 32-bit word for each of its waves. The word's low half is the wave's id. A list has no value if its offset is
// null or doesn't point at the list's tag. The Switch's release of Pikmin 2 keeps the offsets of C-EX and C-ST that the
// earlier releases hold. They point at other data there. In Pikmin's JAudio, the words of C-DF name the waves of the
// group's archive in the archive's order. With an external mode of 0, the words of C-EX name the archive's next waves.
// With any other mode, the high half of each word of C-EX is the number of another scene of the system. The word then
// names the wave of that scene's archive at the place where that scene's C-DF holds the same id. The game looks for a
// sound's wave in C-DF, then in C-EX, then in each scene of `dependencies` in turn.
struct WaveScene
{
    uint32_t external_mode = 0;
    std::vector<uint32_t> dependencies; // numbers of other scenes of the system
    std::optional<std::vector<uint32_t>> cdf;
    std::optional<std::vector<uint32_t>> cex;
    std::optional<std::vector<uint32_t>> cst;
};

// A group of a wave system: one AW file, the ids of its waves and its scene. A game loads the file into ARAM as a
// whole. A wave can be in more than one group. A bank plays a wave from whichever of its groups is loaded.
struct WaveGroup
{
    std::string archive; // the AW file's name
    std::vector<uint16_t> waves;

    // Every wave of the archive's record (in WINF), in the record's order. `waves` holds the ids that the scene's C-DF
    // gives the first of them.
    std::vector<WaveInfo> archive_waves;

    WaveScene scene; // the scene at the group's place in WBCT
};

// A wave system (WSYS): the waves a bank plays, by wave id, and its groups.
struct WaveSystem
{
    uint32_t number = 0; // the number that banks name it by
    std::map<uint16_t, WaveInfo> waves;
    std::vector<WaveGroup> groups; // in the order of the system's WINF block
};

// Where a wave system's record of an archive (in WINF) holds the number of the archive's waves. The record starts with
// the archive's name in 0x70 bytes. A 32-bit offset of each wave's information follows the number. The Switch's
// release of Pikmin 2 has wide records: 0x1c more bytes come before the number (the wave system's parser, 0x3d3130 in
// the release's main). So does the Switch's release of Pikmin (0x273150 in its main).
inline constexpr std::size_t kWaveCountAt = 0x70;
inline constexpr std::size_t kWideWaveCountAt = 0x8c;

// Reads the wave system in `file`. Banks name the system by `number`. Each record of an archive holds the number of its
// waves at `wave_count_at`. A system whose tag is "SYSW" is little-endian. Each of its fields is then stored in the
// width of its type. The Switch's releases of GameCube and Wii games store their wave systems that way. The Switch's
// release of Pikmin also widens each offset to 64 bits. Such a system's tag and size keep the big-endian order, and
// its WINF chunk's tag is reversed. Its other fields are little-endian. Throws FormatError if it's damaged.
WaveSystem ReadWaveSystem(std::span<const uint8_t> file, uint32_t number, std::size_t wave_count_at = kWaveCountAt);

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

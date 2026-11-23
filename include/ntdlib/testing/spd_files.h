// SPDX-License-Identifier: MIT

// Made-up blocks of Nintendo SPD's sound data for tests, laid out as ntdlib/spd/song.h describes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/rounding.h"
#include "ntdlib/spd/song.h"

namespace ntdlib::testing
{

struct SpdInstrument
{
    uint32_t sample = 0; // an index into SpdBlock::samples for a sample instrument
    uint32_t envelope = 0;
    uint32_t settings = 0;
};

struct SpdSample
{
    uint8_t control = 0x08; // PCM8, repeat mode 1
    uint8_t root_key = 60;
    uint16_t timer = 761;
    uint16_t loop_start = 1;
    uint16_t loop_length = 2;
};

struct SpdSound
{
    uint32_t flags = 0x7F0; // player 0, volume 127
    std::vector<std::vector<uint16_t>> tracks;
};

struct SpdBlock
{
    // Each program's instruments, and its map: entry n of `program_maps[p]` names an instrument of `programs[p]`.
    std::vector<std::vector<SpdInstrument>> programs;
    std::vector<std::vector<uint8_t>> program_maps;
    // Each kit's instruments, and its map from keys 36 on.
    std::vector<std::vector<SpdInstrument>> kits;
    std::vector<std::vector<uint8_t>> kit_maps;
    std::vector<SpdSample> samples;
    std::vector<SpdSound> sounds;
    std::vector<uint32_t> modulations;
};

inline void PutLe32(std::vector<uint8_t>& out, std::size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

inline void PutLe16(std::vector<uint8_t>& out, std::size_t at, uint16_t value)
{
    out[at] = static_cast<uint8_t>(value);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
}

// Lays out `block`: the header and the fixed tables, the program, kit and sound tables, the instrument lists, the
// samples (each with zeros for data), then the sounds and their tracks.
inline std::vector<uint8_t> SpdBlockBytes(const SpdBlock& block)
{
    std::vector<uint8_t> out(ntdlib::spd::kBlockHeaderEnd, 0);
    for (std::size_t i = ntdlib::spd::kProgramMapsOffset; i < ntdlib::spd::kEndActionsOffset; ++i)
    {
        out[i] = 0xFF;
    }
    for (std::size_t p = 0; p < block.program_maps.size(); ++p)
    {
        for (std::size_t n = 0; n < block.program_maps[p].size(); ++n)
        {
            out[ntdlib::spd::kProgramMapsOffset + p * ntdlib::spd::kProgramMapSize + n] = block.program_maps[p][n];
        }
    }
    for (std::size_t k = 0; k < block.kit_maps.size(); ++k)
    {
        for (std::size_t n = 0; n < block.kit_maps[k].size(); ++n)
        {
            out[ntdlib::spd::kKitMapsOffset + k * ntdlib::spd::kKitKeys + n] = block.kit_maps[k][n];
        }
    }
    for (std::size_t m = 0; m < block.modulations.size(); ++m)
    {
        PutLe32(out, ntdlib::spd::kModulationsOffset + m * 4, block.modulations[m]);
    }
    const std::size_t program_table = out.size();
    const std::size_t kit_table = program_table + block.programs.size() * 4;
    const std::size_t sound_table = kit_table + block.kits.size() * 4;
    out.resize(sound_table + block.sounds.size() * 8, 0);
    PutLe32(out, 4, static_cast<uint32_t>(program_table));
    PutLe32(out, 8, static_cast<uint32_t>(kit_table));
    PutLe32(out, 12, static_cast<uint32_t>(sound_table));
    PutLe32(out, 16, static_cast<uint32_t>(block.sounds.size()));

    // The samples go after the lists. The lists name them through a fix-up.
    struct Fixup
    {
        std::size_t at;
        uint32_t sample;
    };
    std::vector<Fixup> fixups;
    const auto write_list = [&out, &fixups](const std::vector<SpdInstrument>& list)
    {
        const std::size_t start = out.size();
        for (const SpdInstrument& instrument : list)
        {
            const std::size_t at = out.size();
            out.resize(at + 12, 0);
            if ((instrument.settings & 7) == static_cast<uint32_t>(ntdlib::spd::InstrumentKind::kSample))
            {
                fixups.push_back({at, instrument.sample});
            }
            PutLe32(out, at + 4, instrument.envelope);
            PutLe32(out, at + 8, instrument.settings);
        }
        return static_cast<uint32_t>(start);
    };
    for (std::size_t p = 0; p < block.programs.size(); ++p)
    {
        PutLe32(out, program_table + p * 4, write_list(block.programs[p]));
    }
    for (std::size_t k = 0; k < block.kits.size(); ++k)
    {
        PutLe32(out, kit_table + k * 4, write_list(block.kits[k]));
    }
    std::vector<uint32_t> sample_offsets;
    for (const SpdSample& sample : block.samples)
    {
        const std::size_t at = out.size();
        sample_offsets.push_back(static_cast<uint32_t>(at));
        out.resize(at + 12 + (static_cast<std::size_t>(sample.loop_start) + sample.loop_length) * 4, 0);
        out[at + 1] = sample.control;
        out[at + 2] = sample.root_key;
        PutLe16(out, at + 4, sample.timer);
        PutLe16(out, at + 6, sample.loop_start);
        PutLe16(out, at + 8, sample.loop_length);
    }
    for (const Fixup& fixup : fixups)
    {
        PutLe32(out, fixup.at, sample_offsets.at(fixup.sample));
    }
    for (std::size_t s = 0; s < block.sounds.size(); ++s)
    {
        const SpdSound& sound = block.sounds[s];
        const std::size_t header = out.size();
        out.resize(header + 4 + sound.tracks.size() * 4, 0);
        PutLe32(out, sound_table + s * 8, static_cast<uint32_t>(header));
        PutLe32(out, sound_table + s * 8 + 4, sound.flags);
        PutLe32(out, header, static_cast<uint32_t>(sound.tracks.size()));
        for (std::size_t t = 0; t < sound.tracks.size(); ++t)
        {
            const std::size_t at = out.size();
            PutLe32(out, header + 4 + t * 4, static_cast<uint32_t>(at));
            out.resize(at + sound.tracks[t].size() * 2, 0);
            for (std::size_t e = 0; e < sound.tracks[t].size(); ++e)
            {
                PutLe16(out, at + e * 2, sound.tracks[t][e]);
            }
        }
    }
    PutLe32(out, 0, static_cast<uint32_t>(out.size()));
    return out;
}

// A sound data file: the magic word, the last slot at 8, and the slot table at 0x14. Block n takes slot n, and an
// empty block leaves its slot empty. Each block starts on a multiple of 4 bytes.
inline std::vector<uint8_t> SpdSoundData(const std::vector<std::vector<uint8_t>>& blocks)
{
    std::vector<uint8_t> out = {'S', 'D', 'F', 'D', 'A', 'T', 0, 0};
    const std::size_t table = 0x14;
    out.resize(table + blocks.size() * 8, 0);
    PutLe16(out, 8, static_cast<uint16_t>(blocks.size() - 1));
    for (std::size_t slot = 0; slot < blocks.size(); ++slot)
    {
        if (blocks[slot].empty())
        {
            continue;
        }
        out.resize(ntdlib::AlignUp(out.size(), 4), 0);
        PutLe32(out, table + slot * 8, static_cast<uint32_t>(out.size()));
        PutLe32(out, table + slot * 8 + 4, static_cast<uint32_t>(blocks[slot].size()));
        out.insert(out.end(), blocks[slot].begin(), blocks[slot].end());
    }
    return out;
}

// An event word: a code and its argument.
constexpr uint16_t SpdEvent(uint8_t code, uint8_t argument)
{
    return static_cast<uint16_t>(code | (argument << 8));
}

} // namespace ntdlib::testing

// SPDX-License-Identifier: MIT

// The conversion of Nintendo SPD's sounds (see convert.h): the samples, the SoundFont, the SFZ files and the sound data
// of an image. The performer is in performer.cpp. seqlib writes the MIDI, SoundFont and SFZ files.
//
// The sound unit plays the samples without interpolation. The SFZ regions therefore take sample_quality=0. SoundFont
// has no such setting: its players interpolate.

#include "ntdlib/sequence_export/spd/convert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/psg_samples.h"
#include "ntdlib/sequence_export/sfz_files.h"
#include "ntdlib/spd/driver.h"
#include "ntdlib/spd/player.h"
#include "sequence_export/ntr/sample_preset.h"
#include "sources.h"

namespace ntdlib::sequence_export::spd
{

namespace sp = ntdlib::spd;
namespace sf2 = seqlib::sf2;

namespace
{

// The rate of the sound unit's timers: half the system clock.
constexpr double kTimerHz = ntdlib::ntr::kSystemClockHz / 2.0;

// The timer's period at which a sample of kPsgSampleRate plays the sound unit's square wave and noise (psg_samples.h).
constexpr double kPsgPeriod = 8006.0;

// The name of a source in the SoundFont and of its WAV file.
std::string SourceName(const Source& source)
{
    char name[32];
    if (source.header == 0)
    {
        std::snprintf(name, sizeof(name), source.noise ? "noise" : "square_%d", source.control & 7);
    }
    else
    {
        std::snprintf(name, sizeof(name), "spd_%08X", source.header);
    }

    return name;
}

std::vector<std::string> PresetNames(const std::vector<Source>& presets)
{
    std::vector<std::string> names;
    for (const Source& source : presets)
    {
        names.push_back(SourceName(source));
    }

    return names;
}

// The SoundFont of `perf`: an instrument and a preset for each source that it plays.
sf2::SoundFont BuildSoundFont(const Performance& perf, const Memory& memory, const std::vector<std::string>& names)
{
    sf2::SoundFont sf;
    for (std::size_t i = 0; i < perf.presets.size(); i++)
    {
        const SourceWave wave = WaveOf(memory, perf.presets[i]);
        detail::AddSamplePreset(sf, static_cast<uint32_t>(i), names[i], names[i], wave.pcm, wave.rate,
                                static_cast<uint8_t>(wave.root));
    }

    return sf;
}

// The SFZ files of `perf`: one for each MIDI track, with a region for each source that it plays, and the WAV files.
seqlib::sfz::Instruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, const Memory& memory,
                                  const std::string& title, const std::vector<std::string>& names,
                                  const std::string& program)
{
    // Each source gets a WAV file once.
    seqlib::sfz::Instruments out;
    std::map<Source, std::string> regions;
    const auto region = [&](std::size_t preset)
    {
        const Source& source = perf.presets.at(preset);
        if (const auto found = regions.find(source); found != regions.end())
        {
            return found->second;
        }

        const SourceWave wave = WaveOf(memory, source);
        const std::string line =
            detail::AddSampleRegion(out.samples, names.at(preset) + ".wav", wave.pcm, wave.rate, wave.root);
        return regions.emplace(source, line).first->second;
    };
    const auto preset = [&](uint32_t p)
    {
        return "<group>\n" + region(p);
    };
    out.files = SfzPresetFiles(perf, midi, title, names, program, preset);

    return out;
}

} // namespace

Memory::Memory(const SoundData& sound_data) : data(sound_data), layout(sp::GameLayout(sound_data.header))
{
}

uint8_t Memory::Read8(uint32_t address) const
{
    const auto read = [address](const sp::Block* block, uint32_t base) -> int
    {
        if (block == nullptr || address < base || address - base >= block->Bytes().size())
        {
            return -1;
        }

        return block->Bytes()[address - base];
    };

    int value = read(data.set_a, layout.set_a);
    if (value < 0)
    {
        value = read(data.set_b, layout.set_b);
    }

    return value < 0 ? 0 : static_cast<uint8_t>(value);
}

uint16_t Memory::Read16(uint32_t address) const
{
    return static_cast<uint16_t>(Read8(address) | Read8(address + 1) << 8);
}

SourceWave WaveOf(const Memory& memory, const Source& source)
{
    SourceWave wave;
    if (source.header == 0)
    {
        wave.pcm.channels.push_back(source.noise ? NoiseSamples() : PsgSamples(source.control & 7));
        wave.pcm.loop = true;
        wave.pcm.loop_start = 0;
        wave.pcm.loop_end = static_cast<uint32_t>(wave.pcm.channels[0].size());
        wave.rate = static_cast<uint32_t>(std::lround(kPsgSampleRate * kPsgPeriod / kToneTimer));
        wave.root = kToneKey;

        return wave;
    }

    // The hardware ignores the address's low 2 bits.
    const uint32_t start = (source.header + kHeaderToData) & ~3u;
    const uint32_t words = static_cast<uint32_t>(source.loop) + source.length;
    std::vector<uint8_t> bytes(std::size_t{words} * 4);
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = memory.Read8(start + static_cast<uint32_t>(i));
    }
    const int format = (source.control >> 5) & 3;
    wave.pcm.channels.push_back(
        ntdlib::sdat::DecodeWords(bytes, words, static_cast<ntdlib::sdat::WaveEncoding>(std::min(format, 2))));

    const auto length = static_cast<uint32_t>(wave.pcm.channels[0].size());
    const uint32_t loop_start =
        format == 2 ? (source.loop > 0 ? (source.loop - 1u) * 8 : 0) : source.loop * (4u >> format);
    if (((source.control >> 3) & 3) == 1 && loop_start < length)
    {
        wave.pcm.loop = true;
        wave.pcm.loop_start = loop_start;
        wave.pcm.loop_end = length;
    }
    const uint16_t base = memory.Read16(source.header + 4);
    wave.rate = static_cast<uint32_t>(std::lround(kTimerHz / std::max<uint16_t>(base, 1)));
    wave.root = memory.Read8(source.header + 2);

    return wave;
}

Conversion ConvertSequence(const SoundData& data, int slot, int sound, const std::string& name,
                           const PerformOptions& options, const std::string& program)
{
    Conversion c;
    c.performance = Perform(data, slot, sound, options);
    const seqlib::midi::MidiReport midi = WriteMidiFile(c, name);

    const Memory memory(data);
    const std::vector<std::string> names = PresetNames(c.performance.presets);
    sf2::SoundFont sf = BuildSoundFont(c.performance, memory, names);
    std::vector<std::string> warnings;
    for (const uint32_t index : midi.channel_10_presets)
    {
        sf2::AddChannel10Copy(sf, index, warnings);
    }
    WriteSoundFont(c, sf, name, "Converted by " + program + " from sound " + name + ".", program);
    c.sfz = BuildSfz(c.performance, midi, memory, name, names, program);
    CollectWarnings(c, midi, warnings, {});

    return c;
}

std::vector<SongSet> FindSongs(const ntdlib::ntr::Rom& rom, std::vector<std::string>& warnings)
{
    std::vector<SongSet> sets;
    const std::optional<sp::DriverInfo> info = sp::FindDriver(rom);
    if (!info)
    {
        return sets;
    }

    try
    {
        SongSet set;
        set.info = *info;
        set.data = sp::SoundData(rom, set.info);
        set.blocks = sp::ReadBlockData(set.data, set.info);
        sets.push_back(std::move(set));
    }
    catch (const std::exception& e)
    {
        warnings.push_back(info->path + ": " + e.what());
    }

    return sets;
}

SoundData SoundDataOf(const SongSet& set, int song)
{
    const sp::SongEntry& entry = set.info.songs.at(static_cast<std::size_t>(song));
    SoundData data;
    data.header = set.data;
    data.set_a = &set.blocks.at(0);
    if (entry.block != 0)
    {
        data.set_b = &set.blocks.at(static_cast<std::size_t>(entry.block));
        data.set_b_slot = set.info.blocks.at(static_cast<std::size_t>(entry.block)).slot;
    }

    return data;
}

} // namespace ntdlib::sequence_export::spd

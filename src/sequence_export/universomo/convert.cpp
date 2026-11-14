// SPDX-License-Identifier: MIT

// The conversion of Universomo's songs (see convert.h): the SoundFont, the SFZ files and the songs of an image. The
// performer is in performer.cpp. seqlib writes the MIDI, SoundFont and SFZ files.
//
// The sound unit plays the samples without interpolation. The SFZ regions therefore take sample_quality=0. SoundFont
// has no such setting: its players interpolate.

#include "ntdlib/sequence_export/universomo/convert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/presets.h"
#include "seqlib/sf2/samples.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/track_file.h"
#include "seqlib/wav.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/universomo/formats.h"

namespace ntdlib::sequence_export::universomo
{
namespace
{

namespace sf2 = seqlib::sf2;
namespace uv = ntdlib::universomo;

// The title screen's song in Beat City (game code BEYE, the US release).
constexpr uint32_t kTitleSong = 105;

// The bytes that the sound unit plays of `sample`: the data up to the loop's end, or all of it, in words of 4 bytes, as
// 16-bit PCM. The loop's start and end are in samples.
seqlib::Pcm PcmOf(const uv::Sample& sample)
{
    const uint32_t bytes_per_sample = sample.bits == 16 ? 2 : 1;
    const auto size = static_cast<uint32_t>(sample.data.size());
    const uint32_t loop_start = (sample.loop_start >> 2) * 4;
    const uint32_t loop_end = loop_start + (sample.loop_length >> 2) * 4;
    const bool loop = sample.looped && loop_end > loop_start && loop_end <= size;
    const uint32_t bytes = loop ? loop_end : size / 4 * 4;

    seqlib::Pcm pcm;
    pcm.channels.emplace_back();
    std::vector<int16_t>& out = pcm.channels[0];
    for (uint32_t i = 0; i + bytes_per_sample <= bytes; i += bytes_per_sample)
    {
        out.push_back(sample.bits == 16 ? static_cast<int16_t>(sample.data[i] | sample.data[i + 1] << 8)
                                        : static_cast<int16_t>(static_cast<int8_t>(sample.data[i]) * 256));
    }
    if (loop)
    {
        pcm.loop = true;
        pcm.loop_start = loop_start / bytes_per_sample;
        pcm.loop_end = loop_end / bytes_per_sample;
    }

    return pcm;
}

// The name of the bank's sample `number` in the SoundFont and of its WAV file.
std::string SampleName(uint8_t number)
{
    return "usb_" + std::to_string(number);
}

// The presets' names: the bank's sample of each.
std::vector<std::string> PresetNames(const std::vector<uint8_t>& presets)
{
    std::vector<std::string> names;
    for (const uint8_t sample : presets)
    {
        names.push_back("Sample " + std::to_string(sample));
    }

    return names;
}

// The SoundFont of `perf`: an instrument and a preset for each sample that it plays.
sf2::SoundFont BuildSoundFont(const Performance& perf, const uv::SampleBank& bank,
                              const std::vector<std::string>& names)
{
    sf2::SoundFont sf;
    for (std::size_t i = 0; i < perf.presets.size(); i++)
    {
        const seqlib::Pcm pcm = PcmOf(bank.samples.at(perf.presets[i]));
        const std::vector<uint16_t> ids =
            sf2::AddSamples(sf, seqlib::PrepareForSoundFont(pcm), SampleName(perf.presets[i]), kRootRate, 60);
        sf2::Instrument instrument;
        instrument.name = names[i];
        sf2::Zone global;
        global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
        instrument.zones.push_back(std::move(global));
        sf2::ZoneValues values;
        values.loop = pcm.loop;
        values.sample = ids.at(0);
        instrument.zones.push_back(sf2::RegionZone(values));
        sf2::AddInstrumentPreset(sf, static_cast<uint32_t>(i), names[i], std::move(instrument));
    }

    return sf;
}

// The SFZ files of `perf`: one for each MIDI track, with a region for each sample that it plays, and the WAV files.
seqlib::sfz::Instruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi,
                                  const uv::SampleBank& bank, const std::string& title,
                                  const std::vector<std::string>& names, const std::string& program)
{
    seqlib::sfz::TrackFileOptions options;
    options.title = title;
    options.program = program;
    options.preset_names = names;
    options.presets = perf.presets.size();
    options.level_cut_db = perf.level_cut_db;
    options.track_names = true;

    // Each sample gets a WAV file once.
    seqlib::sfz::Instruments out;
    std::map<uint8_t, std::string> regions;
    const auto region = [&](uint8_t number)
    {
        if (const auto found = regions.find(number); found != regions.end())
        {
            return found->second;
        }

        const seqlib::Pcm pcm = PcmOf(bank.samples.at(number));
        const uint32_t crossfade = (kRootRate * 4 + 999) / 1000; // 4 ms
        const seqlib::PreparedWave prepared = seqlib::PrepareForSfz(pcm, crossfade);
        const std::string name = SampleName(number) + ".wav";
        out.samples.push_back({name, seqlib::WavFile(prepared.channels[0], kRootRate)});
        const std::string line =
            "<region> sample=" + name + " lokey=0 hikey=127 pitch_keycenter=60 sample_quality=0" +
            seqlib::sfz::LoopOpcodes(prepared.loop, prepared.loop_start, prepared.loop ? prepared.loop_end - 1 : 0) +
            "\n";
        return regions.emplace(number, line).first->second;
    };
    const auto text = [&](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = [&](uint32_t p)
        {
            return "<group>\n" + region(perf.presets.at(p));
        };

        return added;
    };
    out.files = seqlib::sfz::TrackFiles(perf, midi, options, text);

    return out;
}

// The file named `name` in `folder` of `rom`'s file system, ignoring case, or null. `folder` ends with '/' or is empty.
const ntdlib::ntr::RomFile* FindInFolder(const ntdlib::ntr::Rom& rom, const std::string& folder,
                                         const std::string& name)
{
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        if (file.path.size() == folder.size() + name.size() && file.path.starts_with(folder) &&
            xsflib::AsciiLower(file.path.substr(folder.size())) == name)
        {
            return &file;
        }
    }

    return nullptr;
}

// The song's number in the name of a song file, ignoring case, or nothing.
std::optional<uint32_t> SongNumber(const std::string& file_name)
{
    const std::string lower = xsflib::AsciiLower(file_name);
    if (lower.size() < 7 || !lower.starts_with("au") || !lower.ends_with(".ums"))
    {
        return std::nullopt;
    }

    const std::string digits = lower.substr(2, lower.size() - 6);
    if (digits.empty() || digits.size() > 5 ||
        !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(std::stoul(digits));
}

} // namespace

Conversion ConvertSequence(const uv::Song& song, const uv::SampleBank& bank, const std::string& name,
                           const PerformOptions& options, const std::string& program)
{
    Conversion c;
    c.performance = Perform(song, bank, options);
    const seqlib::midi::MidiReport midi = WriteMidiFile(c, name);

    const std::vector<std::string> names = PresetNames(c.performance.presets);
    sf2::SoundFont sf = BuildSoundFont(c.performance, bank, names);
    std::vector<std::string> warnings;
    for (const uint32_t index : midi.channel_10_presets)
    {
        sf2::AddChannel10Copy(sf, index, warnings);
    }
    WriteSoundFont(c, sf, name, "Converted by " + program + " from song " + name + ".", program);
    c.sfz = BuildSfz(c.performance, midi, bank, name, names, program);
    CollectWarnings(c, midi, warnings, {});

    return c;
}

std::vector<SongSet> FindSongs(const ntdlib::ntr::Rom& rom, std::vector<std::string>& warnings)
{
    // The folders that hold a bank.
    std::vector<SongSet> sets;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        const std::string::size_type slash = file.path.rfind('/');
        const std::string folder = slash == std::string::npos ? "" : file.path.substr(0, slash + 1);
        if (xsflib::AsciiLower(file.path.substr(folder.size())) != "samplebank.usb")
        {
            continue;
        }

        try
        {
            sets.push_back({folder, uv::ReadSampleBank(rom.FileData(file.id)), {}});
        }
        catch (const std::exception& e)
        {
            warnings.push_back(file.path + ": " + e.what());
        }
    }

    const bool beat_city = rom.Header().game_code == "BEYE";
    for (SongSet& set : sets)
    {
        // The table of sounds gives which songs loop.
        std::vector<uv::Sound> sounds;
        if (const ntdlib::ntr::RomFile* table = FindInFolder(rom, set.folder, "audio"))
        {
            try
            {
                sounds = uv::ReadSoundTable(rom.FileData(table->id));
            }
            catch (const std::exception& e)
            {
                warnings.push_back(table->path + ": " + e.what());
            }
        }

        for (const ntdlib::ntr::RomFile& file : rom.Files())
        {
            if (!file.path.starts_with(set.folder) || file.path.find('/', set.folder.size()) != std::string::npos)
            {
                continue;
            }

            const std::optional<uint32_t> number = SongNumber(file.path.substr(set.folder.size()));
            if (!number)
            {
                continue;
            }

            try
            {
                SongInput input{*number, "au" + std::to_string(*number), true, uv::ReadSong(rom.FileData(file.id))};
                if (*number < sounds.size())
                {
                    input.loop = sounds[*number].loops;
                }
                if (beat_city && *number == kTitleSong)
                {
                    input.name += "_title_screen";
                }
                set.songs.push_back(std::move(input));
            }
            catch (const std::exception& e)
            {
                warnings.push_back(file.path + ": " + e.what());
            }
        }
        std::ranges::sort(set.songs, {}, &SongInput::number);
    }

    std::erase_if(sets, [](const SongSet& set) { return set.songs.empty(); });

    return sets;
}

} // namespace ntdlib::sequence_export::universomo

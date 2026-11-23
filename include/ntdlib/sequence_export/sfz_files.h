// SPDX-License-Identifier: MIT

// What the SFZ files of every model share: the WAV sample of each wave, made once, and the options of a sequence's
// track files. The header requires only seqlib. A library of models that doesn't link sequence_export can therefore use
// it.

#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sfz/track_file.h"
#include "seqlib/wav.h"

namespace ntdlib::sequence_export
{

// The samples of a sequence's SFZ files: a mono WAV file for each wave, made once, on first use. A wave goes by its
// name. No two waves of a sequence share a name. `P` is a decoded wave: a seqlib::Pcm with its `sample_rate` in Hz,
// as a uint32_t or a float.
template <typename P>
class SfzSamples
{
public:
    // A sample's file name, rate and loop. SFZ's loop end is the loop's last sample. A wave that can't be read has no
    // name.
    struct Wave
    {
        std::string name;
        uint32_t rate = 0;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
    };

    // The sample of the wave named `name`, in the file <name>.wav. On first use, `pcm` gives the wave, or nullptr for
    // a wave that can't be read. A rate in floating point is rounded to the nearest whole rate.
    const Wave& Get(const std::string& name, const std::function<const P*()>& pcm)
    {
        if (const auto it = waves_.find(name); it != waves_.end())
        {
            return it->second;
        }

        Wave wave;
        if (const P* decoded = pcm())
        {
            const seqlib::PreparedWave prepared =
                seqlib::PrepareForSfz(*decoded, seqlib::SfzLoopCrossfade(decoded->sample_rate));
            wave.name = name + ".wav";
            if constexpr (std::is_floating_point_v<decltype(decoded->sample_rate)>)
            {
                wave.rate = static_cast<uint32_t>(std::lround(decoded->sample_rate));
            }
            else
            {
                wave.rate = decoded->sample_rate;
            }
            files_.push_back({wave.name, seqlib::WavFile(prepared.channels[0], wave.rate)});
            wave.loop = prepared.loop;
            if (prepared.loop)
            {
                wave.loop_start = prepared.loop_start;
                wave.loop_end = prepared.loop_end - 1;
            }
        }

        return waves_.emplace(name, std::move(wave)).first->second;
    }

    // Takes the WAV files made. The collection is left empty.
    std::vector<seqlib::sfz::Sample> Take()
    {
        return std::move(files_);
    }

private:
    std::map<std::string, Wave> waves_;
    std::vector<seqlib::sfz::Sample> files_;
};

// The options of the SFZ track files of `perf`, a performance with its `presets` and its `level_cut_db`
// (seqlib::sfz::TrackFiles). The first comment of each file names the sequence `title` and `program`, the program
// that wrote it. A comment names each preset from `preset_names`. Each file takes its MIDI track's name where it has
// one, and is "Track <n>" otherwise.
template <typename Performance>
seqlib::sfz::TrackFileOptions SfzTrackFileOptions(const Performance& perf, const std::string& title,
                                                  const std::vector<std::string>& preset_names,
                                                  const std::string& program)
{
    seqlib::sfz::TrackFileOptions options;
    options.title = title;
    options.program = program;
    options.preset_names = preset_names;
    options.presets = perf.presets.size();
    options.level_cut_db = perf.level_cut_db;
    options.track_names = true;

    return options;
}

// The SFZ file of each MIDI track of `perf` that has a channel in `midi`, with the options of SfzTrackFileOptions.
// `preset(p)` gives the text after the <master> line of preset `p`: its opcodes, and its groups with their regions.
template <typename Performance>
std::vector<seqlib::sfz::File> SfzPresetFiles(const Performance& perf, const seqlib::midi::MidiReport& midi,
                                              const std::string& title, const std::vector<std::string>& preset_names,
                                              const std::string& program,
                                              const std::function<std::string(uint32_t)>& preset)
{
    const auto text = [&preset](int)
    {
        seqlib::sfz::TrackText added;
        added.preset = preset;

        return added;
    };

    return seqlib::sfz::TrackFiles(perf, midi, SfzTrackFileOptions(perf, title, preset_names, program), text);
}

} // namespace ntdlib::sequence_export

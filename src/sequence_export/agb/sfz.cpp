// SPDX-License-Identifier: MIT

// The SFZ files of a GBA conversion (see sfz.h).

#include "ntdlib/sequence_export/agb/sfz.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/sfz/track_file.h"
#include "seqlib/wav.h"

namespace ntdlib::sequence_export::agb
{
namespace
{

namespace sfz = seqlib::sfz;

// The custom curve of CC10 to the pan. seqlib's track files give it this number, and sfizz reserves curves 0 to 6.
constexpr int kPanCurve = 7;

// The bend range of a preset's file by itself, in semitones: the range that a GBA conversion's MIDI tracks start with.
constexpr int kStartingBendRange = 2;

// The release of a zone without a volume envelope: SoundFont's default, the shortest time, in seconds.
const double kRelease = std::exp2(kInstant / 1200.0);

// A region's values, from its zone's generators.
struct RegionValues
{
    int sample = -1;
    int low_key = 0;
    int high_key = 127;
    int root_key = -1; // -1: the sample's key
    int cents = 0;
    bool loop = false;
};

// The values of `zone`. Throws std::invalid_argument for a generator that a region doesn't take, and for modulators.
RegionValues ValuesOf(const sf2::Zone& zone)
{
    if (!zone.modulators.empty())
    {
        throw std::invalid_argument("A zone of the SoundFont holds modulators. The SFZ files can't hold them.");
    }

    RegionValues values;
    for (const sf2::Generator& generator : zone.generators)
    {
        const int amount = static_cast<int16_t>(generator.amount);
        switch (generator.oper)
        {
        case sf2::kKeyRange:
            values.low_key = generator.amount & 0xFF;
            values.high_key = generator.amount >> 8;
            break;
        case sf2::kOverridingRootKey:
            values.root_key = amount;
            break;
        case sf2::kCoarseTune:
            values.cents += 100 * amount;
            break;
        case sf2::kFineTune:
            values.cents += amount;
            break;
        case sf2::kSampleModes:
            values.loop = (amount & 1) != 0;
            break;
        case sf2::kSampleId:
            values.sample = generator.amount;
            break;
        default:
            throw std::invalid_argument("A zone of the SoundFont holds generator " + std::to_string(generator.oper) +
                                        ". The SFZ files can't hold it.");
        }
    }

    return values;
}

// The WAV files of the SoundFont's samples, made once, on first use. A file's name comes from the sample's: spaces
// become underscores, and percent signs go. A name that another sample's file took gets the sample's index after it.
class SfzSamples
{
public:
    explicit SfzSamples(const Sf2File& sf) : sf_(sf)
    {
    }

    // The file name of sample `index`.
    const std::string& NameOf(int index)
    {
        if (const auto found = names_.find(index); found != names_.end())
        {
            return found->second;
        }

        const sf2::Sample& sample = sf_.samples.at(static_cast<std::size_t>(index));
        std::string stem;
        for (const char c : sample.name)
        {
            if (c == ' ')
            {
                stem += '_';
            }
            else if (c != '%')
            {
                stem += c;
            }
        }
        stem = seqlib::SafeFileName(stem);
        std::string name = stem + ".wav";
        if (taken_.contains(name))
        {
            name = stem + "_" + std::to_string(index) + ".wav";
        }
        taken_.insert(name);
        files_.push_back({name, seqlib::WavFile(sample.data, sample.sample_rate)});

        return names_.emplace(index, std::move(name)).first->second;
    }

    // Takes the WAV files made. The collection is left empty.
    std::vector<sfz::Sample> Take()
    {
        return std::move(files_);
    }

private:
    const Sf2File& sf_;
    std::map<int, std::string> names_;
    std::set<std::string> taken_;
    std::vector<sfz::Sample> files_;
};

// The region of the zone whose values are `values`: its sample, keys, root key, tuning and loop. A looping zone loops
// the sample's loop, or the whole sample where the sample has none. The SoundFont plays it the same way.
std::string RegionLine(const RegionValues& values, const sf2::Sample& sample, const std::string& name)
{
    const int root = values.root_key >= 0 ? values.root_key : sample.original_key;
    std::string s = "<region> sample=" + name + " lokey=" + std::to_string(values.low_key) +
                    " hikey=" + std::to_string(values.high_key) + " pitch_keycenter=" + std::to_string(root) +
                    " sample_quality=0";
    s += sfz::TuneOpcodes(values.cents);
    s += sfz::AmpEnvelopeOpcodes({.release = kRelease});

    const bool loop = values.loop && !sample.data.empty();
    uint32_t start = 0;
    uint32_t end = static_cast<uint32_t>(sample.data.size());
    if (Looped(sample))
    {
        start = sample.loop_start;
        end = sample.loop_end;
    }
    s += sfz::LoopOpcodes(loop, start, loop ? end - 1 : 0);

    return s + "\n";
}

// The regions of preset `preset` of `sf`, after a <group> header. A zone of the instrument without a sample is its
// global zone. The GBA conversions give it only seqlib's global modulators. The frame of each file stands in for
// them. Throws std::invalid_argument as ValuesOf does, and for a global zone or a preset zone with other values.
std::string PresetRegions(const Sf2File& sf, std::size_t preset, SfzSamples& samples)
{
    const sf2::Preset& p = sf.presets.at(preset);
    std::string s = "<group>\n";
    for (const sf2::Zone& preset_zone : p.zones)
    {
        if (preset_zone.generators.size() != 1 || preset_zone.generators[0].oper != sf2::kInstrument ||
            !preset_zone.modulators.empty())
        {
            throw std::invalid_argument("A preset zone of the SoundFont holds values other than its instrument.");
        }

        const sf2::Instrument& instrument = sf.instruments.at(preset_zone.generators[0].amount);
        for (const sf2::Zone& zone : instrument.zones)
        {
            const bool global =
                std::ranges::none_of(zone.generators, [](const sf2::Generator& g) { return g.oper == sf2::kSampleId; });
            if (global)
            {
                if (!zone.generators.empty())
                {
                    throw std::invalid_argument(
                        "An instrument's global zone holds generators. The SFZ files can't hold them.");
                }
                continue;
            }

            const RegionValues values = ValuesOf(zone);
            const sf2::Sample& sample = sf.samples.at(static_cast<std::size_t>(values.sample));
            s += RegionLine(values, sample, samples.NameOf(values.sample));
        }
    }

    return s;
}

// The start of a file: its first comment, `comment`, then the samples' folder, CC10's pan curve, the bend range of
// `bend_range` semitones, and CC7, CC10 and CC11 without smoothing. Curve 4 squares CC7 and CC11: General MIDI's volume
// curve, 40 log10(value / 127) dB. The SoundFont's default modulators give the same.
std::string Frame(const std::string& comment, int bend_range)
{
    std::string s = "// " + comment + "\n\n";
    s += "<control>\ndefault_path=../" + sfz::kSamplesFolder + "/\nhint_ram_based=1\n\n";
    s += sfz::Curve(kPanCurve, "CC10 to pan", [](int v) { return (v - 64) / 64.0; });
    const std::string bend = std::to_string(bend_range * 100);
    s += "<global>\nbend_up=" + bend + " bend_down=-" + bend + "\n";
    s += "pan_oncc10=100 pan_curvecc10=" + std::to_string(kPanCurve) + " pan_smoothcc10=0\n";
    s += "amplitude_oncc7=100 amplitude_curvecc7=4 amplitude_smoothcc7=0 amplitude_oncc11=100 amplitude_curvecc11=4 "
         "amplitude_smoothcc11=0\n";

    return s;
}

// Whether the presets of `sf` are in more than one bank, the drum bank's copies aside. A <master> then selects its
// bank by CC0.
bool BanksSelected(const Sf2File& sf)
{
    return std::ranges::any_of(sf.presets, [](const sf2::Preset& p) { return p.bank != 0 && p.bank != kDrumBank; });
}

} // namespace

seqlib::sfz::Instruments TrackSfz(const Sf2File& sf, std::span<const SfzTrack> tracks, const std::string& title,
                                  const std::string& program)
{
    SfzSamples samples(sf);
    sfz::Instruments out;
    const bool banks = BanksSelected(sf);
    for (const SfzTrack& track : tracks)
    {
        if (track.presets.empty())
        {
            continue;
        }

        const std::string name = sfz::CommentText(track.name);
        std::string s = Frame(name + " of " + sfz::CommentText(title) + ", converted by " + program +
                                  ". Load for MIDI track \"" + name + "\".",
                              track.bend_range);
        std::set<std::size_t> presets(track.presets.begin(), track.presets.end());
        for (const std::size_t p : presets)
        {
            if (p >= sf.presets.size())
            {
                throw std::invalid_argument("A track selects preset " + std::to_string(p) +
                                            ". The SoundFont has no such preset.");
            }

            const sf2::Preset& preset = sf.presets[p];
            s += "\n// " + sfz::CommentText(preset.name) + "\n<master> loprog=" + std::to_string(preset.program) +
                 " hiprog=" + std::to_string(preset.program);
            if (banks)
            {
                s += " locc0=" + std::to_string(preset.bank & 0x7F) + " hicc0=" + std::to_string(preset.bank & 0x7F);
            }
            s += "\n" + PresetRegions(sf, p, samples);
        }
        out.files.push_back({seqlib::SafeFileName(track.name) + ".sfz", std::move(s)});
    }
    out.samples = samples.Take();

    return out;
}

seqlib::sfz::Instruments PresetSfz(const Sf2File& sf, const std::string& title, const std::string& program)
{
    std::size_t digits = 1;
    for (const sf2::Preset& preset : sf.presets)
    {
        digits = std::max(digits, std::to_string(preset.bank * 128 + preset.program).size());
    }

    SfzSamples samples(sf);
    sfz::Instruments out;
    for (std::size_t p = 0; p < sf.presets.size(); ++p)
    {
        const sf2::Preset& preset = sf.presets[p];
        std::string s =
            Frame(sfz::CommentText(preset.name) + " of " + sfz::CommentText(title) + ", converted by " + program + ".",
                  kStartingBendRange);
        s += "\n" + PresetRegions(sf, p, samples);

        std::string number = std::to_string(preset.bank * 128 + preset.program);
        number.insert(0, digits - number.size(), '0');
        out.files.push_back({seqlib::SafeFileName(title) + "_" + number + ".sfz", std::move(s)});
    }
    out.samples = samples.Take();

    return out;
}

} // namespace ntdlib::sequence_export::agb

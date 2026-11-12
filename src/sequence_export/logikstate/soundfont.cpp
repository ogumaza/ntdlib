// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/logikstate/soundfont.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/player.h"
#include "ntdlib/sequence_export/agb/sf2.h"

namespace ntdlib::sequence_export::logikstate
{

using ntdlib::logikstate::kModulePeriods;
using ntdlib::logikstate::kPeriodClock;
using ntdlib::logikstate::kStoppedLength;
using ntdlib::sequence_export::agb::AddLoopGuard;
using ntdlib::sequence_export::agb::LengthenLoop;
using ntdlib::sequence_export::agb::Looped;
using ntdlib::sequence_export::agb::MakeInstrument;
using ntdlib::sequence_export::agb::MakePreset;
namespace sf2 = seqlib::sf2;

namespace
{

// The rate of kRootPitch from ProTracker's period for it, for a module whose table has no period there.
constexpr uint32_t kUsualRate = 8287;

// Returns the SoundFont sample of `played`: its points, signed 8-bit, and its loop.
sf2::Sample MakeSample(const Rom& rom, const PlayedSample& played, const char* name)
{
    sf2::Sample s;
    s.name = name;
    s.sample_rate = played.rate;
    s.original_key = uint8_t(kRootKey);
    const auto add = [&](uint32_t from, uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            s.data.push_back(int16_t(rom.S8(played.points + from + i) * 256));
        }
    };
    add(0, played.length);

    if (played.loops)
    {
        s.loop_start = LoopOffset(played);
        if (s.loop_start == played.length)
        {
            add(played.loop_start, played.loop_length);
        }
        s.loop_end = uint32_t(s.data.size());
        LengthenLoop(s);
        AddLoopGuard(s);
    }
    if (s.data.empty())
    {
        s.data.push_back(0);
    }

    return s;
}

} // namespace

bool Loops(uint32_t loop_length)
{
    return loop_length > kStoppedLength;
}

uint32_t LoopOffset(const PlayedSample& sample)
{
    const bool ends_together = sample.loop_start + sample.loop_length == sample.length;
    return ends_together ? sample.loop_start : sample.length;
}

int InstrumentSet::Program(const PlayedSample& sample, int number)
{
    return programs_.try_emplace(sample, Instrument{number, int(programs_.size())}).first->second.program;
}

Sf2File InstrumentSet::Build(const Rom& rom) const
{
    Sf2File f;
    std::map<PlayedSample, int> samples;
    for (const auto& [played, inst] : programs_)
    {
        char name[32];
        std::snprintf(name, sizeof name, "Sample %02d", inst.number);

        // The SoundFont's sample. The instruments of every start point share it.
        PlayedSample whole = played;
        whole.offset = 0;
        auto it = samples.find(whole);
        if (it == samples.end())
        {
            f.samples.push_back(MakeSample(rom, whole, name));
            it = samples.emplace(whole, int(f.samples.size()) - 1).first;
        }

        // A note that starts within the sample starts its zone there.
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
        if (played.offset)
        {
            z.generators.push_back(sf2::Generator::Value(sf2::kStartAddrsOffset, int(played.offset % 32768)));
            if (played.offset >= 32768)
            {
                z.generators.push_back(sf2::Generator::Value(sf2::kStartAddrsCoarseOffset, int(played.offset / 32768)));
            }
            std::snprintf(name, sizeof name, "Sample %02d +%u", inst.number, unsigned(played.offset));
        }
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, Looped(f.samples[size_t(it->second)]) ? 1 : 0));
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, it->second));
        f.instruments.push_back(MakeInstrument(name, {z}));
        f.presets.push_back(MakePreset(name, uint16_t(inst.program / 128), uint16_t(inst.program % 128),
                                       int(f.instruments.size()) - 1));
    }

    return f;
}

uint32_t SampleRate(const Rom& rom, const Module& module, uint16_t fine_tune)
{
    if (module.format == ntdlib::logikstate::Format::kGbaMod1)
    {
        const uint64_t step = rom.U32(module.steps + (kMod1RootPitch + fine_tune) * 4);
        return step ? uint32_t((step * module.rate + 2048) / 4096) : kUsualRate;
    }

    const uint32_t period = rom.U16(module.address + kModulePeriods + (kRootPitch + fine_tune) * 2);
    return period ? uint32_t(kPeriodClock) / (period * 2) : kUsualRate;
}

} // namespace ntdlib::sequence_export::logikstate

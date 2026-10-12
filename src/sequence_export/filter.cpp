// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/filter.h"

#include <algorithm>
#include <type_traits>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"

namespace ntdlib::sequence_export
{
namespace
{

// The biquad types that have presets: 10 on the Switch, 5 on the other consoles.
int BiquadTypes(const Settings& settings)
{
    return settings.filters->sfz_biquads[5].empty() ? 5 : 10;
}

// The row of biquad preset `biquad_type` that 0xb5 argument `biquad_value` selects, or -1 for no biquad. The track
// stores the argument / 127. Zero, or a type with no preset, disables the filter. Each preset picks its table's row
// from the value (nwsnd::BiquadPresetRow). Every console's voice clamps the value to 0-1 (on the Wii,
// Voice::SetBiquadFilter, 0x800cc028 in Wii Music's main.dol). CC104 holds only 0 to 127, and the conversion clamps the
// argument to that range.
template <typename Policy>
int BiquadStep(int biquad_type, int biquad_value)
{
    const float value = std::clamp(Policy::BiquadValueOf(biquad_value), 0.0f, 1.0f);
    return value > 0.0f ? nwsnd::BiquadPresetRow<Policy>(biquad_type, value) : -1;
}

// The low-pass step that 0xd8 argument `low_pass` selects on the Wii U, the Switch and the 3DS, or -1 for off. The
// track stores (argument - 64) / 64 (MmlParser::CommandProc). Channel::Update adds 1 for the voice. The voice takes the
// cutoff from kLpfFreq (nwsnd::LpfFreqIndex) and turns the filter off at 16000 Hz.
template <typename Policy>
int LowPassStep(int low_pass)
{
    const int index = nwsnd::LpfFreqIndex<Policy>(static_cast<float>(low_pass - 0x40) * 0.015625f + 1.0f);
    return nwsnd::tables::kLpfFreq[index] < 16000 ? index : -1;
}

// A track's filters on the console of `Policy`.
template <typename Policy>
GameFilters FiltersOf(int low_pass, int biquad_type, int biquad_value)
{
    GameFilters f;

    // The Wii's low-pass takes the argument as its step.
    if constexpr (std::is_same_v<Policy, nwsnd::RevolutionPolicy>)
    {
        f.low_pass = LowPassIndex(low_pass);
    }
    else
    {
        f.low_pass = LowPassStep<Policy>(low_pass);
    }

    const int step = BiquadStep<Policy>(biquad_type, biquad_value);
    if (step >= 0)
    {
        f.biquad_type = static_cast<uint8_t>(biquad_type);
        f.biquad_step = step;
    }

    return f;
}

} // namespace

uint8_t SfzLowPassValue(int low_pass)
{
    return static_cast<uint8_t>(std::clamp(0x40 - low_pass, 0, 127));
}

uint8_t SfzBiquadValue(int biquad_type, int biquad_value, const Settings& settings)
{
    return BiquadShape(biquad_type, settings) != 0 ? static_cast<uint8_t>(std::clamp(biquad_value, 0, 127)) : 0;
}

int BiquadShape(int biquad_type, const Settings& settings)
{
    return biquad_type >= 1 && biquad_type <= BiquadTypes(settings) ? (biquad_type - 1) % 5 + 1 : 0;
}

int LowPassIndex(int low_pass)
{
    // The track gives (argument - 64) / 64, the channel adds 1, and the voice clamps the sum to 0..1. So an argument of
    // 0 or less gives the lowest cutoff.
    const int last = static_cast<int>(kRevolutionFilters.low_pass_hz.size()) - 1;
    const int index = std::max(low_pass, 0);
    return index <= last ? index : -1;
}

int LowPassHz(int index)
{
    const int last = static_cast<int>(kRevolutionFilters.low_pass_hz.size()) - 1;
    return kRevolutionFilters.low_pass_hz[static_cast<std::size_t>(std::clamp(index, 0, last))];
}

GameFilters TrackFilters(int low_pass, int biquad_type, int biquad_value, const Settings& settings)
{
    switch (settings.engine)
    {
    case Engine::kRevolution:
        return FiltersOf<nwsnd::RevolutionPolicy>(low_pass, biquad_type, biquad_value);
    case Engine::kCtr:
        return FiltersOf<nwsnd::CtrPolicy>(low_pass, biquad_type, biquad_value);
    case Engine::kCafe:
        return FiltersOf<nwsnd::CafePolicy>(low_pass, biquad_type, biquad_value);
    default:
        return FiltersOf<nwsnd::NxPolicy>(low_pass, biquad_type, biquad_value);
    }
}

SoundFontFilter ToSoundFont(const GameFilters& filters, const Settings& settings)
{
    const FilterTables& tables = *settings.filters;
    SoundFontFilter sf;
    if (filters.low_pass >= 0)
    {
        sf.cutoff = tables.low_pass_cutoff[static_cast<std::size_t>(filters.low_pass)];
    }

    // Two active low-passes use the lower cutoff.
    const auto step = static_cast<std::size_t>(filters.biquad_step);
    const SoundFontFilter* biquad = filters.biquad_type == 1 ? &tables.biquad_low_pass[step]
                                    : filters.biquad_type == 6 && !tables.biquad_low_pass_48k.empty()
                                        ? &tables.biquad_low_pass_48k[step]
                                        : nullptr;
    if (biquad != nullptr && biquad->cutoff < sf.cutoff)
    {
        sf = *biquad;
    }

    return sf;
}

uint8_t FilterControllerValue(uint16_t cutoff)
{
    const int steps = (kFilterOpen - static_cast<int>(cutoff)) * 128;
    return static_cast<uint8_t>(std::clamp((steps + kFilterControllerRange / 2) / kFilterControllerRange, 0, 127));
}

int SfzLowPassCutoff(int index, const Settings& settings)
{
    const std::span<const int16_t> table = settings.filters->sfz_low_pass_cutoff;
    const int last = static_cast<int>(table.size()) - 1;
    return table[static_cast<std::size_t>(std::clamp(index, 0, last))];
}

SfzBiquad SfzBiquadStep(int type, int step, const Settings& settings)
{
    // A type with no table takes the last table.
    const int types = BiquadTypes(settings);
    const int table_type = type >= 1 && type <= types ? type : types;
    const std::span<const SfzBiquad> table = settings.filters->sfz_biquads[table_type - 1];
    const int last = static_cast<int>(table.size()) - 1;
    return table[static_cast<std::size_t>(std::clamp(step, 0, last))];
}

} // namespace ntdlib::sequence_export

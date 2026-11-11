// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/effects.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/util.h"

namespace ntdlib::rvl::snd
{
namespace
{

float Word(const EffectParams& params, int index)
{
    return std::bit_cast<float>(params[static_cast<std::size_t>(index)]);
}

// ut::Clamp: the low bound below it, the high bound above it, and the value itself between (and for NaN).
template <typename T>
T Clamp(T value, T low, T high)
{
    return value < low ? low : (high < value ? high : value);
}

// Fills the Exp parameters of a 2008 reverb as the class's SetParam does. The longest pre-delay bounds the pre-delay as
// given, before the game raises a negative one to 0. The game checks each upper bound before the lower one.
template <typename Params>
void FillExpParams(Params& p, const EffectParams& params, const Reverb2008Words& words)
{
    const float max = Word(params, words.pre_delay_time_max);
    const float pre_delay = Word(params, 0);
    const float fused_time = Word(params, 1);
    p.early_mode = params[static_cast<std::size_t>(words.early_mode)];
    p.pre_delay_time_max = max < 0.0f ? 0.0f : max;
    p.pre_delay_time = max < pre_delay ? max : (pre_delay < 0.0f ? 0.0f : pre_delay);
    p.fused_mode = params[static_cast<std::size_t>(words.fused_mode)];
    p.fused_time = fused_time < 0.0f ? 0.0f : fused_time;
    p.coloration = Clamp(Word(params, 2), 0.0f, 1.0f);
    p.damping = Clamp(Word(params, 3), 0.0f, 1.0f);
    p.early_gain = Clamp(Word(params, words.early_gain), 0.0f, 1.0f);
    p.fused_gain = Clamp(Word(params, words.fused_gain), 0.0f, 1.0f);
    p.out_gain = Clamp(Word(params, words.out_gain), 0.0f, 1.0f);
    p.send_gain = 0.0f;
}

} // namespace

axfx::Build AxfxBuild(const SndBuild& build, ax::Build ax_build)
{
    if (ax_build == ax::Build::kOctober2006)
    {
        return axfx::Build::kOctober2006;
    }

    return build.SinceMarch2008() ? axfx::Build::kSeptember2008 : axfx::Build::kDecember2006;
}

axfx::ChorusExp::Params Chorus2009Params(const EffectParams& params)
{
    axfx::ChorusExp::Params p;
    p.delay_time = Clamp(Word(params, 0), 0.1f, 50.0f);
    p.depth = Clamp(Word(params, 1), 0.0f, 1.0f);
    p.rate = Clamp(Word(params, 2), 0.1f, 2.0f);
    p.feedback = Clamp(Word(params, 3), 0.0f, 0.99f);
    p.out_gain = Clamp(Word(params, 4), 0.0f, 1.0f);
    p.send_gain = 0.0f;

    return p;
}

axfx::DelayExp::Params Delay2009Params(const EffectParams& params)
{
    // A NaN passes each bound as the game's comparisons let it through.
    const float delay = Word(params, 0);
    const float max_delay = Word(params, 3);
    axfx::DelayExp::Params p;
    p.max_delay = max_delay < 0.04f ? 0.04f : max_delay;
    p.delay = max_delay < delay ? max_delay : (delay < 0.04f ? 0.04f : delay);
    p.feedback = Clamp(Word(params, 1), 0.0f, 0.99f);
    p.lpf = Clamp(Word(params, 4), 0.0f, 1.0f);
    p.out_gain = Clamp(Word(params, 2), 0.0f, 1.0f);
    p.send_gain = 0.0f;

    return p;
}

Effect::Effect(EffectType type, axfx::Build build) : slot_(MakeSlot(type, build))
{
}

Effect::Slots Effect::MakeSlot(EffectType type, axfx::Build build)
{
    switch (type)
    {
    case EffectType::kReverbHi:
        return Slot<axfx::ReverbHi>{axfx::ReverbHi(build), {}};
    case EffectType::kReverbHi2008:
        return Slot<axfx::ReverbHiExp>{axfx::ReverbHiExp(build), {}};
    case EffectType::kChorus:
        return Slot<axfx::Chorus>{};
    case EffectType::kDelay:
        return Slot<axfx::Delay>{};
    case EffectType::kReverbStd2008:
        return Slot<axfx::ReverbStdExp>{};
    case EffectType::kPhotoMood:
        return Slot<axfx::PhotoMood>{};
    case EffectType::kChorus2009:
        return Slot<axfx::ChorusExp>{axfx::ChorusExp(axfx::Build::kFebruary2009), {}};
    case EffectType::kDelay2009:
        return Slot<axfx::DelayExp>{};
    case EffectType::kReverbStd:
        break;
    }

    return Slot<axfx::ReverbStd>{};
}

bool Effect::SetParam(const EffectParams& params)
{
    if (auto* slot = std::get_if<Slot<axfx::ReverbHi>>(&slot_))
    {
        // FxReverbHi::SetParam (0x8003a84c): preDelayTime, fusedTime, coloration, damping, crossTalk and outGain.
        axfx::ReverbHi::Params& p = slot->params;
        p.pre_delay = Clamp(Word(params, 0), 0.0f, 0.1f);
        p.time = Clamp(Word(params, 1), 0.0f, 10.0f);
        p.coloration = Clamp(Word(params, 2), 0.0f, 1.0f);
        p.damping = Clamp(Word(params, 3), 0.0f, 1.0f);
        p.crosstalk = Clamp(Word(params, 4), 0.0f, 1.0f);
        p.mix = Clamp(Word(params, 5), 0.0f, 1.0f);

        return !active_ || slot->fx.Settings(p);
    }

    if (auto* slot = std::get_if<Slot<axfx::ReverbHiExp>>(&slot_))
    {
        // FxReverbHi::SetParam of 2008 (Animal Crossing: City Folk, 0x80262fe8, and Wii Music, 0x800b73cc) fills the
        // Exp parameters as FxReverbStd's of 2008 does, with crossTalk too. While the reverb runs, the class keeps the
        // delay lines if their longest pre-delay stays the same.
        axfx::ReverbHiExp::Params& p = slot->params;
        const float old_max = p.pre_delay_time_max;
        FillExpParams(p, params, kReverbHi2008Words);
        p.crosstalk = Clamp(Word(params, 4), 0.0f, 1.0f);
        if (!active_)
        {
            return true;
        }

        return p.pre_delay_time_max == old_max ? slot->fx.SettingsUpdate(p) : slot->fx.Settings(p);
    }

    if (auto* slot = std::get_if<Slot<axfx::ReverbStdExp>>(&slot_))
    {
        // FxReverbStd::SetParam of 2008 (Mario Kart Wii, 0x80094d00, and Wii Music, 0x800b813c). While the reverb runs,
        // the class keeps the delay lines if their longest pre-delay stays the same.
        axfx::ReverbStdExp::Params& p = slot->params;
        const float old_max = p.pre_delay_time_max;
        FillExpParams(p, params, kReverbStd2008Words);
        if (!active_)
        {
            return true;
        }

        return p.pre_delay_time_max == old_max ? slot->fx.SettingsUpdate(p) : slot->fx.Settings(p);
    }

    if (auto* slot = std::get_if<Slot<axfx::Chorus>>(&slot_))
    {
        // FxChorus::SetParam (0x80039f54): delayTime in milliseconds, depth and rate in hertz.
        axfx::Chorus::Params& p = slot->params;
        const float delay_time = Word(params, 0);
        p.base_delay = Clamp<uint32_t>(ToUnsigned(delay_time), 1, 50);
        p.variation = Clamp<uint32_t>(ToUnsigned(delay_time * Word(params, 1)), 0, 50);
        p.period = Clamp<uint32_t>(ToUnsigned(1000.0f / Word(params, 2)), 500, 10000);

        return !active_ || slot->fx.Settings(p);
    }

    if (auto* slot = std::get_if<Slot<axfx::Delay>>(&slot_))
    {
        // FxDelay::SetParam (0x8003a44c): delay in milliseconds, feedback and outGain, the same for each channel.
        axfx::Delay::Params& p = slot->params;
        const uint32_t delay = Clamp<uint32_t>(ToUnsigned(Word(params, 0)), 1, 5000);
        const uint32_t feedback = Clamp<uint32_t>(ToUnsigned(100.0f * Word(params, 1)), 0, 99);
        const uint32_t output = Clamp<uint32_t>(ToUnsigned(100.0f * Word(params, 2)), 0, 100);
        p.delay.fill(delay);
        p.feedback.fill(feedback);
        p.output.fill(output);

        return !active_ || slot->fx.Settings(p);
    }

    if (auto* slot = std::get_if<Slot<axfx::ChorusExp>>(&slot_))
    {
        // FxChorus::SetParam of 2009 runs Settings while the chorus runs.
        slot->params = Chorus2009Params(params);

        return !active_ || slot->fx.Settings(slot->params);
    }

    if (auto* slot = std::get_if<Slot<axfx::DelayExp>>(&slot_))
    {
        // FxDelay::SetParam of 2009. While the delay runs, the class keeps the lines if their longest delay stays the
        // same.
        const float old_max = slot->params.max_delay;
        slot->params = Delay2009Params(params);
        if (!active_)
        {
            return true;
        }

        const axfx::DelayExp::Params& p = slot->params;
        return p.max_delay == old_max ? slot->fx.SettingsUpdate(p) : slot->fx.Settings(p);
    }

    if (auto* slot = std::get_if<Slot<axfx::PhotoMood>>(&slot_))
    {
        // The Photo Channel 1.1's SetMode (0x80049820) holds the mode to 0 to 4.
        slot->params = axfx::PhotoMood::SetMode(static_cast<int32_t>(params[0]));

        return !active_ || slot->fx.Settings(slot->params);
    }

    // WarioWare: Smooth Moves' standard reverb (its SetParam, 0x800e33ac): preDelay, time, coloration, damping and mix.
    auto& slot = std::get<Slot<axfx::ReverbStd>>(slot_);
    axfx::ReverbStd::Params& p = slot.params;
    p.pre_delay = Clamp(Word(params, 0), 0.0f, 0.1f);
    p.time = Clamp(Word(params, 1), 0.0f, 10.0f);
    p.coloration = Clamp(Word(params, 2), 0.0f, 1.0f);
    p.damping = Clamp(Word(params, 3), 0.0f, 1.0f);
    p.mix = Clamp(Word(params, 4), 0.0f, 1.0f);

    return !active_ || slot.fx.Settings(p);
}

bool Effect::StartUp()
{
    const bool started = std::visit([](auto& slot) { return slot.fx.Init(slot.params); }, slot_);
    active_ = true;

    return started;
}

void Effect::Shutdown()
{
    if (!active_)
    {
        return;
    }

    active_ = false;
    std::visit([](auto& slot) { slot.fx.Shutdown(); }, slot_);
}

void Effect::UpdateBuffer(axfx::AuxFrame& frame)
{
    std::visit([&frame](auto& slot) { slot.fx.Process(frame); }, slot_);
}

void Effects::Set(const std::vector<EffectSetup>& setups, axfx::Build build, bool follow_variables)
{
    setups_ = setups;
    objects_.clear();
    for (const EffectSetup& setup : setups_)
    {
        objects_.emplace_back(setup.type, build);
    }

    follow_variables_ = follow_variables;
    pending_ = !setups_.empty();
}

void Effects::OnAxFrame(ax::System& ax, const std::array<int16_t, 16>* variables)
{
    if (pending_)
    {
        pending_ = false;
        for (std::size_t i = 0; i < setups_.size(); i++)
        {
            if (setups_[i].presets.empty())
            {
                objects_[i].SetParam(setups_[i].params);
                Append(ax, setups_[i].bus, i);
            }
        }
    }

    // 0x800fee28 reads the music's local variables 0 and 1, and passes each to 0x80100e40.
    if (follow_variables_ && variables != nullptr)
    {
        PickPreset(ax, (*variables)[0]);
        PickPreset(ax, (*variables)[1]);
    }
}

bool Effects::Append(ax::System& ax, int bus, std::size_t object)
{
    // The fade of ClearEffect is never running: the effects are only ever cleared at once.
    ax.SetAuxReturnVolume(bus, ax::kMaxVolume);
    if (!objects_[object].StartUp())
    {
        return false;
    }

    std::vector<std::size_t>& list = lists_[static_cast<std::size_t>(bus)];
    if (list.empty())
    {
        ax.RegisterAuxCallback(bus, [this, bus](axfx::AuxFrame& frame) { AuxCallback(bus, frame); });
        wait_counters_[static_cast<std::size_t>(bus)] = 2;
    }

    list.push_back(object);

    return true;
}

void Effects::Shutdown(ax::System& ax, int bus)
{
    std::vector<std::size_t>& list = lists_[static_cast<std::size_t>(bus)];
    if (list.empty())
    {
        return;
    }

    for (const std::size_t object : list)
    {
        objects_[object].Shutdown();
    }

    list.clear();
    ax.RegisterAuxCallback(bus, nullptr);
}

void Effects::AuxCallback(int bus, axfx::AuxFrame& frame)
{
    int& wait = wait_counters_[static_cast<std::size_t>(bus)];
    const std::vector<std::size_t>& list = lists_[static_cast<std::size_t>(bus)];
    if (wait > 0 || list.empty())
    {
        wait = std::max(wait - 1, 0);
        frame = {};

        return;
    }

    for (const std::size_t object : list)
    {
        objects_[object].UpdateBuffer(frame);
    }
}

void Effects::PickPreset(ax::System& ax, int16_t value)
{
    const auto found = std::find_if(setups_.begin(), setups_.end(), [](const auto& s) { return !s.presets.empty(); });
    if (found == setups_.end())
    {
        return;
    }

    const EffectSetup& setup = *found;
    const auto object = static_cast<std::size_t>(found - setups_.begin());
    if (value == 0)
    {
        // 0x80101158 clears the bus's effects at once (ClearEffect with no fade).
        if (preset_ != 0)
        {
            preset_ = 0;
            Shutdown(ax, setup.bus);
            preset_appended_ = false;
        }

        return;
    }

    if (value < 0 || static_cast<std::size_t>(value) > setup.presets.size() || value == preset_)
    {
        return;
    }

    // 0x80100dd4 sets the preset on the effect object. The object runs Settings while the effect runs. 0x80100f6c then
    // clears the bus if the effect is on it, and appends the effect.
    preset_ = value;
    objects_[object].SetParam(setup.presets[static_cast<std::size_t>(value - 1)]);
    if (preset_appended_)
    {
        Shutdown(ax, setup.bus);
    }

    Append(ax, setup.bus, object);
    preset_appended_ = true;
}

} // namespace ntdlib::rvl::snd

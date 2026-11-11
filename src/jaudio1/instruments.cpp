// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio1/instruments.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/jaudio/bank.h"

namespace ntdlib::jaudio1
{

namespace
{

// JASInstRand's offset of its random value from 0 to 2: -0.99999988 (0xbf7ffffe).
constexpr float kRandomCenter = std::bit_cast<float>(0xbf7ffffeu);

// JASDrumSet::getParam's oscillator for every drum: the volume at 1 throughout, with no tables.
const OscData kDrumOscillator{};

// The instruments that JASBNKParser::findOscPtr searches.
constexpr uint32_t kInstruments = 128;

// The first instrument before program `number` that plays oscillator `osc`, or nullptr.
const ntdlib::jaudio::Instrument* FirstSharing(const ntdlib::jaudio::Bank& bank, uint32_t number, std::size_t osc)
{
    for (const auto& [earlier, other] : bank.programs)
    {
        if (earlier >= number)
        {
            return nullptr;
        }

        const auto* shares = std::get_if<ntdlib::jaudio::Instrument>(&other);
        if (shares != nullptr &&
            std::find(shares->oscillators.begin(), shares->oscillators.end(), osc) != shares->oscillators.end())
        {
            return shares;
        }
    }

    return nullptr;
}

// The parser's draws (findOscPtr), from `random`: for each oscillator of an instrument, in the order of the programs
// and of the instrument's slots, the first earlier instrument that plays the same oscillator gives a note of key 60 at
// velocity 127. Each of its random effects takes a number. The drum sets come after the instruments and draw none.
void DrawLikeTheParser(const ntdlib::jaudio::Bank& bank, RandomFast& random)
{
    for (const auto& [number, program] : bank.programs)
    {
        const auto* inst = std::get_if<ntdlib::jaudio::Instrument>(&program);
        if (number >= kInstruments || inst == nullptr)
        {
            continue;
        }

        for (const std::size_t osc : inst->oscillators)
        {
            const ntdlib::jaudio::Instrument* shares = FirstSharing(bank, number, osc);
            if (shares == nullptr)
            {
                continue;
            }

            for (const ntdlib::jaudio::InstEffect& e : shares->effects)
            {
                if (e.kind == ntdlib::jaudio::InstEffect::Kind::kRandom)
                {
                    random.Next();
                }
            }
        }
    }
}

} // namespace

InstrumentBank::InstrumentBank(const ntdlib::jaudio::Bank& bank, RandomFast& random)
{
    if (bank.version != 0)
    {
        throw xsflib::FormatError("the model of Pikmin 2 reads banks of version 0 only");
    }

    number_ = bank.number;

    // Each oscillator gets its copies of its two tables, as the game's parser makes them.
    const auto copy = [&](const std::vector<ntdlib::jaudio::OscPoint>& points) -> OscTable
    {
        if (points.empty())
        {
            return {};
        }

        const auto& table = tables_.emplace_back(std::make_unique<std::vector<OscPoint>>());
        for (const ntdlib::jaudio::OscPoint& point : points)
        {
            table->push_back({point.mode, point.time, point.value});
        }

        return {table->data(), table->size()};
    };
    std::vector<const OscData*> oscillators;
    oscillators.reserve(bank.oscillators.size());
    for (const ntdlib::jaudio::Oscillator& from : bank.oscillators)
    {
        auto& osc = oscillators_.emplace_back(std::make_unique<OscData>());
        osc->target = from.target;
        osc->rate = from.rate;
        osc->attack = copy(from.attack);
        osc->release = copy(from.release_is_attack ? from.attack : from.release);
        osc->width = from.scale;
        osc->vertex = from.vertex;
        oscillators.push_back(osc.get());
    }

    const auto oscillator = [&oscillators](std::size_t i)
    {
        return oscillators[i];
    };
    jaudio2::CopyPrograms<Programs>(bank, programs_, oscillator);

    DrawLikeTheParser(bank, random);
}

bool InstrumentBank::HasProgram(uint32_t program) const
{
    return program < programs_.size() && !std::holds_alternative<std::monostate>(programs_[program]);
}

void InstrumentBank::ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param,
                                 RandomFast& random)
{
    float y = 0.0f;
    if (effect.random)
    {
        float unit = random.Next();
        unit = unit + unit;
        unit = unit + kRandomCenter;
        y = effect.base + effect.width * unit;
    }
    else
    {
        const int32_t x = effect.trigger == 2   ? static_cast<int32_t>(key)
                          : effect.trigger == 1 ? static_cast<int32_t>(velocity)
                                                : 0;
        y = SenseCurve(effect.low, effect.high, effect.center, static_cast<uint32_t>(x));
    }

    switch (effect.target)
    {
    case 0:
        param.volume = y * param.volume;
        break;
    case 1:
        param.pitch = y * param.pitch;
        break;
    case 2:
        // The game moves the pan in double precision.
        param.pan = static_cast<float>((static_cast<double>(y) + -0.5) + static_cast<double>(param.pan));
        break;
    case 3:
        param.fxmix = y + param.fxmix;
        break;
    case 4:
        param.dolby = y + param.dolby;
        break;
    default:
        break;
    }
}

float SenseCurve(float low, float high, uint32_t center, uint32_t x)
{
    if (center == 0x7f || center == 0)
    {
        return low + ((high - low) * static_cast<float>(x)) / 127.0f;
    }
    if (center > x)
    {
        return low + (static_cast<float>(x) / static_cast<float>(center)) * (1.0f - low);
    }

    const float above =
        static_cast<float>(static_cast<int32_t>(x - center)) / static_cast<float>(static_cast<int32_t>(127 - center));

    return above * (high + -1.0f) + 1.0f;
}

bool InstrumentBank::GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param,
                                  RandomFast& random) const
{
    if (program >= programs_.size())
    {
        return false;
    }

    if (const auto* inst = std::get_if<Instrument>(&programs_[program]))
    {
        // JASBasicInst::getParam: always two oscillators, the second often none.
        param.wave_format = 0;
        param.percussion = false;
        param.oscillators = inst->oscillators;
        param.oscillator_count = 2;
        param.volume = inst->volume;
        param.pitch = inst->pitch;
        for (const Effect& effect : inst->effects)
        {
            ApplyEffect(effect, key, velocity, param, random);
        }

        const KeyRegion* region = jaudio2::FindKeyRegion(inst->keys, key);
        if (region == nullptr)
        {
            return false;
        }

        const VelocityRegion* v = jaudio2::FindVelocity<int64_t>(region->velocities, velocity);
        if (v == nullptr)
        {
            return false;
        }

        param.volume = v->volume * param.volume;
        param.pitch = v->pitch * param.pitch;
        param.wave_id = v->wave_id;

        return true;
    }

    if (const auto* drums = std::get_if<DrumSet>(&programs_[program]))
    {
        // JASDrumSet::getParam. A key without a drum has no regions.
        if (key > 0x7f || !drums->keys[key])
        {
            return false;
        }

        const Drum& drum = *drums->keys[key];
        param.wave_format = 0;
        param.percussion = true;
        param.volume = drum.volume;
        param.pitch = drum.pitch;
        param.pan = drum.pan;
        param.release = drum.release;
        param.oscillators = {&kDrumOscillator, nullptr};
        param.oscillator_count = 1;
        for (const Effect& effect : drum.effects)
        {
            ApplyEffect(effect, key, velocity, param, random);
        }

        const VelocityRegion* v = jaudio2::FindVelocity<int64_t>(drum.velocities, velocity);
        if (v == nullptr)
        {
            return false;
        }

        param.volume = v->volume * param.volume;
        param.pitch = v->pitch * param.pitch;
        param.wave_id = v->wave_id;

        return true;
    }

    return false;
}

} // namespace ntdlib::jaudio1

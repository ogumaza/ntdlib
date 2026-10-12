// SPDX-License-Identifier: MIT

// A NintendoWare sequence's performance (see performance.h).

#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export
{
namespace
{

// The engines whose LFOs keep the bytecode's units: the speed, and the delay in 5 ms steps.
bool LfoInBytecodeUnits(Engine engine)
{
    return engine == Engine::kCtr || engine == Engine::kRevolution;
}

} // namespace

EnvelopeValues OverrideEnvelope(EnvelopeValues env, const PresetKey& key)
{
    if (key.attack <= 127)
    {
        env.attack = key.attack;
    }
    if (key.decay <= 127)
    {
        env.decay = key.decay;
    }
    if (key.sustain <= 127)
    {
        env.sustain = key.sustain;
    }
    if (key.release <= 127)
    {
        env.release = key.release;
    }
    if (key.hold <= 127)
    {
        env.hold = key.hold;
    }

    return env;
}

double LfoHz(const PresetKey& key, const Settings& settings)
{
    if (!LfoInBytecodeUnits(settings.engine))
    {
        return key.lfo_speed;
    }

    // The bytecode's speed is a whole number, and the float holds it exactly. env_time_scale is 1 on the Wii.
    return static_cast<uint8_t>(key.lfo_speed) * 100.0 / 256.0 / settings.env_time_scale;
}

double LfoDelayMs(const PresetKey& key, const Settings& settings)
{
    if (!LfoInBytecodeUnits(settings.engine))
    {
        return key.lfo_delay;
    }

    return static_cast<uint16_t>(key.lfo_delay) * 5 * settings.env_time_scale;
}

} // namespace ntdlib::sequence_export

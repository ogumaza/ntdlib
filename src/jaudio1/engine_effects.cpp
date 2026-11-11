// SPDX-License-Identifier: MIT

// The engine's effects on the effect sequence: their start, their levels in each of the game's frames, and the
// levels that their slot tracks take (see engine.cpp).

#include "ntdlib/jaudio1/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "jaudio1/engine_internal.h"

namespace ntdlib::jaudio1
{

namespace
{

// The flag of a game's effect ID that keeps the effect until it ends. The game sets it for most of its effects. It
// holds the others for as long as they play.
constexpr uint32_t kHeldEffect = 0x800;

// The levels that the effect sequence's slot tracks keep when they ask for them: the sequence's sound info (flag
// 0x1f00 in PSGame's start-up) keeps them all.
constexpr uint8_t kEffectKept = 0x1f;

// The flags of an effect's sound info that its levels read (JAInter's sound flags).
constexpr uint32_t kFlagNoDistanceVolume = 0x2;
constexpr uint32_t kFlagRandomPitch = 0x10;
constexpr uint32_t kFlagPitchSpread = 0xc0;
constexpr uint32_t kFlagNoMapSend = 0x400;

// The bits of the sound table's format for the effects.
constexpr uint8_t kFormatOffset = 1;
constexpr uint8_t kFormatPitch = 2;
constexpr uint8_t kFormatSend = 8;

// checkPlayingSeUpdateMultiplication's product of the lines.
float LineProduct(const Lines& lines, float factor)
{
    float value = 1.0f;
    if (lines[kLineOverride].value == -1.0f)
    {
        for (std::size_t i = 0; i < kLineOverride; i++)
        {
            value *= lines[i].value;
        }
    }
    else
    {
        value = lines[kLineOverride].value;
    }

    return value * factor;
}

// checkPlayingSeUpdateAddition's sum of the lines' offsets from `center`, from 0 to 1.
float LineSum(const Lines& lines, float center)
{
    if (lines[kLineOverride].value != -1.0f)
    {
        return lines[kLineOverride].value;
    }

    float value = 0.0f;
    for (std::size_t i = 0; i < kLineOverride; i++)
    {
        value += lines[i].value - center;
    }
    value += center;
    return std::clamp(value, 0.0f, 1.0f);
}

// JAISe::setSeDistanceParameters with PSM::SeSound's overrides, for an effect with JAI's default actor at the listener.
// The levels move over `time` frames.
void SetDistanceLevels(Effect& e, uint32_t time)
{
    // setSeDistanceVolume. SeSound::setDistanceVolumeCommon also sets a line of the effect send.
    float volume = 1.0f;
    if ((e.info.flags & kFlagNoDistanceVolume) == 0)
    {
        e.fxmixes[kLineDoppler].Set(kEffectSend, 0);
        volume = std::clamp(1.0f - e.distance_offset, 0.0f, 1.0f);
    }
    e.volumes[kLineDistance].Set(volume, time);

    // SeSound::setSeDistancePan.
    e.pans[kLineDistance].Set(kListenerPan, time);

    // setSeDistancePitch. An effect with the random pitch takes a new pitch below 1 in each frame.
    float pitch = 1.0f;
    if ((e.info.flags & kFlagRandomPitch) != 0)
    {
        const auto step = static_cast<uint32_t>(static_cast<int32_t>(e.jai_random.Next() * 16.0f)) & 0xf;
        pitch = 1.0f - static_cast<float>(step) / 192.0f;
    }
    if ((e.info.flags & kFlagPitchSpread) != 0)
    {
        pitch += static_cast<float>(e.pitch_spread) / 192.0f;
    }
    e.pitches[kLineDistance].Set(pitch, time);

    // setSeDistanceFxmix, the map's send, and SeSound::setSeDistanceDolby.
    e.fxmixes[kLineDistance].Set(0.0f, time);
    if ((e.info.flags & kFlagNoMapSend) == 0)
    {
        e.fxmixes[kLineMap].Set(0.0f, 0);
    }
    e.dolbys[kLineDistance].Set(0.0f, time);
}

} // namespace

std::optional<std::string> Engine::StartEffect(uint16_t id, uint8_t volume, uint32_t sound_id)
{
    model_.reset();
    const SoundFiles::SequenceFile* file = files_.FindSequence(id);
    if (file == nullptr)
    {
        return std::string("the sequence archive has no such file");
    }

    const std::optional<SoundInfo> info = (sound_id >> 30) == 0 ? files_.FindSound(sound_id) : std::nullopt;
    if (!info)
    {
        return std::string("the sound table has no such effect");
    }

    // The effect's slot track: its category's first in effect scene 0 (SeMgr's seScene stays 0). The slot tracks
    // count on from one category to the next.
    const uint32_t category = (sound_id >> 12) & 0xff;
    const std::vector<std::vector<uint8_t>>& scenes = files_.EffectScenes();
    if (scenes.empty() || category >= scenes[0].size() || scenes[0][category] == 0)
    {
        return std::string("effect scene 0 gives the effect's category no slot track");
    }

    uint32_t slot = 0;
    for (uint32_t i = 0; i < category; i++)
    {
        slot += scenes[0][i];
    }

    uint32_t slots = 0;
    for (const std::vector<uint8_t>& scene : scenes)
    {
        uint32_t sum = 0;
        for (const uint8_t count : scene)
        {
            sum += count;
        }
        slots = std::max(slots, sum);
    }

    // JAI's lines of the effect sequence's volume: the sequence's volume in the game's list of its music when the
    // sequences' format has the volume bit (SeqBase::init writes it into the sound info), line 3 at 1
    // (SeSeq::setConfigVolume), and line 8, the effects' volume of the options.
    float sequence_volume = 1.0f;
    if ((files_.SequenceFormat() & kFormatVolume) != 0)
    {
        sequence_volume = sequence_volume * (static_cast<float>(volume) / 127.0f);
    }
    // Line 3 multiplies by 1 and changes nothing.
    sequence_volume = sequence_volume * settings_.effect_volume;
    if (auto error = StartSequence(*file, sequence_volume))
    {
        return error;
    }

    // storeSeBuffer and SeSound::initParameter. The game keeps the effect until it ends. The high 4 bits of the
    // effect's flags give the most that its volume drops at random, in fifteenths.
    Model& m = *model_;
    m.piece.kept_levels = kEffectKept;
    m.effect = std::make_unique<Effect>();
    Effect& e = *m.effect;
    e.id = sound_id | kHeldEffect;
    e.info = *info;
    e.slot = slot;
    e.slots = slots;
    e.route = 0x20000000u | ((slot >> 4) & 0xf) | ((slot & 0xf) << 4);
    const uint32_t drop = info->flags >> 28;
    if (drop != 0)
    {
        e.distance_offset = std::clamp(static_cast<float>(drop) / 15.0f * e.jal_random.Next(), 0.0f, 1.0f);
    }

    return std::nullopt;
}

void Engine::EffectFrame()
{
    // processGFrameSe for the one effect: checkNextFrameSe, checkSeMovePara and checkPlayingSe. SeMgr finds the effect
    // sequence playing (SOUNDSTATE_Playing) a frame after the sequence manager does.
    Model& m = *model_;
    Effect& e = *m.effect;
    if (!m.started || e.state == Effect::kInactive)
    {
        return;
    }

    // checkNextFrameSe: the stored effect takes its category's first slot, and loads. A playing effect is ready again.
    if (e.state == Effect::kStored)
    {
        e.state = Effect::kLoaded;
    }
    else if (e.state == Effect::kPlaying)
    {
        e.state = Effect::kReady;
    }

    for (Lines* lines : {&e.volumes, &e.pans, &e.fxmixes, &e.dolbys, &e.pitches})
    {
        for (MoveLevel& line : *lines)
        {
            line.Move();
        }
    }

    // checkPlayingSe reads ports 2 and 0 of the slot track. A missing slot track leaves the game's values unknown.
    uint16_t playing = 0;
    uint16_t start = 0;
    if (!m.tracks.ReadPortApp(m.root, e.route | 0x20000, playing) || !m.tracks.ReadPortApp(m.root, e.route, start))
    {
        m.unmodelled++;
    }

    if (e.state == Effect::kLoaded)
    {
        // The start. An effect with a spread of pitches draws its rise at random. The distance's levels take their
        // values at once, then the sound info's. The slot track takes the levels when it asks for them. The ports take
        // the wait, the FX line, the effect's number and the start.
        const uint32_t spread = e.info.flags & kFlagPitchSpread;
        if (spread != 0)
        {
            const auto draw = static_cast<uint32_t>(static_cast<int32_t>(e.jai_random.Next() * 255.0f));
            const uint32_t mask = spread == 0x40 ? 0xf : spread == 0x80 ? 0x1f : 0x3f;
            e.pitch_spread = static_cast<uint8_t>(draw & mask);
        }
        SetDistanceLevels(e, 0);

        // JAIBasic::setSeExtParameter.
        const uint8_t format = files_.EffectFormat();
        if ((format & kFormatVolume) != 0)
        {
            e.volumes[kLineDoppler].Set(static_cast<float>(e.info.volume) / 127.0f, 0);
        }
        if ((format & kFormatSend) != 0)
        {
            e.fxmixes[kLineDoppler].Set(static_cast<float>(e.info.fxmix) / 127.0f, 0);
        }
        if ((format & kFormatPitch) != 0)
        {
            e.pitches[kLineDoppler].Set(e.info.pitch, 0);
        }
        SendEffectLevels(false);

        // JAIBasic::getSoundOffsetNumberFromID. An effect with the ground's flag adds the ground of map 0, which is 0.
        const uint16_t number = (format & kFormatOffset) != 0 ? e.info.offset : static_cast<uint16_t>(e.id & 0x3ff);
        m.tracks.WritePortApp(m.root, e.route | 0x30000, 0);
        m.tracks.WritePortApp(m.root, e.route | 0x60000, 0);
        m.tracks.WritePortApp(m.root, e.route | 0x40000, number);
        m.tracks.WritePortApp(m.root, e.route, 1);
        e.state = Effect::kPlaying;
    }
    else if (playing == 0 && start != 1)
    {
        // releaseSeRegist: the slot track has finished the effect. Its port 0 drops to 0, and the sequence manager
        // stops the track.
        m.tracks.WritePortApp(m.root, e.route, 0);
        e.stop = true;
        e.state = Effect::kInactive;
    }
    else if (e.state == Effect::kReady)
    {
        SetDistanceLevels(e, kDistanceTime);
        SendEffectLevels(true);
        e.state = Effect::kPlaying;
    }
}

void Engine::SendEffectLevels(bool send)
{
    // sendSeAllParameter: the levels from the lines, each in the order of the code. A level that has changed goes to
    // the slot track by a port command, unless the effect is loading. The category's volume is 1.
    Model& m = *model_;
    Effect& e = *m.effect;
    Model::PortArgs& args = m.ports[e.slot];
    uint8_t flags = 0;
    const auto update = [&](float value, float& level, float& arg, uint8_t flag)
    {
        if (level != value)
        {
            level = value;
            if (send)
            {
                flags |= flag;
                arg = value;
            }
        }
    };
    update(LineProduct(e.volumes, 1.0f), e.volume, args.volume, kPortVolume);
    update(LineSum(e.pans, 0.5f), e.pan, args.pan, kPortPan);
    update(LineProduct(e.pitches, 1.0f), e.pitch, args.pitch, kPortPitch);
    update(LineSum(e.fxmixes, 0.0f), e.fxmix, args.fxmix, kPortFxmix);
    update(LineSum(e.dolbys, kSurroundCenter / 127.0f), e.dolby, args.dolby, kPortDolby);
    if (flags != 0)
    {
        args.flags = flags;
        if (!args.queued)
        {
            args.queued = true;
            m.port_queue.push_back(e.slot);
        }
    }
}

} // namespace ntdlib::jaudio1

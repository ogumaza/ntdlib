// SPDX-License-Identifier: MIT

// The sounds that the tracks start and stop (oneshot's Play_1shot functions): the notes, the percussion, the
// oscillators' sounds, the gates and the releases (see channel.cpp).

#include "ntdlib/jac/channel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "jac/channel_internal.h"
#include "ntdlib/jaudio1/instruments.h"
#include "ntdlib/jaudio2/instruments.h"

namespace ntdlib::jac
{

namespace
{

// polys_table (0x5b7090 in main's ro): the voices of a sound's group by the class in the low 4 bits of its
// instrument's flags.
constexpr std::array<uint8_t, 16> kPolys = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32};

// The envelopes of a drum (PERC_ENV, 0x6b0500 in main's data) and of a DSP oscillator (OSC_ENV, 0x6b0520) with its
// release (OSC_REL, 0x6b0540). A drum's envelope has no tables and holds the note at full volume until its direct
// release.
constexpr std::array<OscPoint, 2> kOscReleasePoints = {{{1, 10, 0}, {kPointStop, 0, 1}}};
constexpr OscData kPercEnvelope{OscData::kVolume, 1.0f, {}, {}, 1.0f, 0.0f};
constexpr OscData kOscEnvelope{
    OscData::kVolume, 1.0f, {}, {kOscReleasePoints.data(), kOscReleasePoints.size()}, 1.0f, 0.0f};

int32_t ClampKey(int32_t key)
{
    return std::min(std::max(key, 0), 127);
}

// The square of a velocity's fraction of 127.
float VelocityVolume(uint32_t velocity)
{
    const float fraction = static_cast<float>(velocity & 0xff) / 127.0f;

    return fraction * fraction;
}

// The pitch of `key` on channel `c`: against its wave's base key, or the key itself for an oscillator's sound or a
// channel without a wave.
float KeyPitchOf(const Channel& c, uint32_t key)
{
    int32_t k = static_cast<int32_t>(key & 0xff);
    if (c.kind != Channel::kOscillator && c.wave != nullptr)
    {
        k = k - c.wave->info.base_key + 60;
    }

    return KeyPitch(static_cast<uint32_t>(ClampKey(k))) * c.base_pitch;
}

// The levels of a sound at its start: its pan `pan`, no effect send and no surround, the effects' levels in the
// middle, and no modifiers.
void StartLevels(Channel& c, float pan)
{
    c.pans[Channel::kPan][Channel::kSound] = pan;
    c.pans[Channel::kPan][Channel::kEffect] = 0.5f;
    c.pans[Channel::kFxmix][Channel::kSound] = 0.0f;
    c.pans[Channel::kFxmix][Channel::kEffect] = 0.0f;
    c.pans[Channel::kDolby][Channel::kSound] = 0.0f;
    c.pans[Channel::kDolby][Channel::kEffect] = 0.0f;
    c.pitch_modifier = 1.0f;
    c.volume_modifier = 1.0f;
}

// Bank_SenseToOfs (0x272420 in main): an effect that follows the key or the velocity `value`.
float SenseToOfs(const InstEffect* effect, uint32_t value)
{
    if (effect == nullptr)
    {
        return 1.0f;
    }

    return jaudio1::SenseCurve(effect->low, effect->high, effect->center, value & 0xff);
}

// Bank_RandToOfs (0x2724d0 in main): a random effect.
float RandToOfs(const InstEffect* effect, Random& random)
{
    if (effect == nullptr)
    {
        return 1.0f;
    }

    return random.Sf32() * effect->width + effect->base;
}

// The effect of a kind in slot `slot` (0 or 1) of an instrument or a drum. ntdlib keeps the effects that the record
// gives, the random ones first. No record of the Switch's bank file leaves its first slot of a kind empty and fills
// the second.
const InstEffect* EffectAt(const std::vector<InstEffect>& effects, InstEffect::Kind kind, std::size_t slot)
{
    std::size_t seen = 0;
    for (const InstEffect& effect : effects)
    {
        if (effect.kind == kind && seen++ == slot)
        {
            return &effect;
        }
    }

    return nullptr;
}

// Returns the velocity region of `inst` for `key` and `velocity` (GetInstVmap), or null. Sets `keymap` to the index of
// the key region.
const ntdlib::jaudio::VelocityRegion* FindRegion(const Instrument* inst, uint32_t key, uint32_t velocity,
                                                 std::size_t& keymap)
{
    keymap = 0;
    while (keymap < inst->keys.size() && inst->keys[keymap].high_key < key)
    {
        keymap++;
    }
    if (keymap == inst->keys.size())
    {
        return nullptr;
    }

    return jaudio2::FindVelocity<int64_t>(inst->keys[keymap].velocities, velocity);
}

// The sound's group for the limit on its voices: the flags in the top byte, the bank's place and the program, and by
// the flags' bits 6 and 7 the key region, all keys, or all of the bank's sounds.
uint32_t VoiceGroup(uint8_t flags, uint32_t place, uint32_t program, std::size_t keymap)
{
    uint32_t group = uint32_t{flags} << 24 | place << 8 | program;
    switch (flags & 0xc0)
    {
    case 0x40:
        group |= static_cast<uint32_t>(keymap) << 16;
        break;

    case 0x80:
        group |= 0xff;
        break;

    case 0xc0:
        group = uint32_t{flags} << 24 | 0xffffff;
        break;

    default:
        break;
    }

    return group;
}

} // namespace

uint32_t Channels::PlayStart(uint32_t manager, uint32_t channel, int32_t time)
{
    // Play_1shot's start (0x279450 in main): the channel waits for a DSP channel, or goes back to the free list if
    // the queue is full or it has no wave.
    Channel& c = channels_[channel];
    c.dsp_channel = kNone;
    c.callback = Channel::kOneShot;
    c.time = time != 0 ? time : -1;
    c.countdown = c.time;
    if (CheckLogical(channel))
    {
        if (AddWait(channel))
        {
            ListAddTail(manager, ChannelManager::kWaiting, channel);
            return channel;
        }
    }

    ListAddTail(manager, ChannelManager::kFree, channel);

    return kNone;
}

uint32_t Channels::Play(uint32_t manager, uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity,
                        int32_t time)
{
    const uint32_t sound = (bank & 0xff) << 24 | (program & 0xff) << 16 | (key & 0xff) << 8 | (velocity & 0xff);
    if ((program & 0xff) >= 0xf0)
    {
        return PlayOsc(manager, sound, time);
    }
    if ((program & 0xff) >= 0xe4)
    {
        return PlayPerc(manager, sound, time);
    }

    return PlayOneShot(manager, sound, time);
}

const WaveHandle* Channels::RegionWave(uint32_t place, const ntdlib::jaudio::VelocityRegion& region) const
{
    // WaveidToWavegroup and GetSoundHandle.
    const uint32_t system = banks_.BankSystem(place);
    return system != Banks::kNone ? banks_.FindSound(system, region.wave_id) : nullptr;
}

uint32_t Channels::PlayOneShot(uint32_t manager, uint32_t sound, int32_t time)
{
    // Play_1shot (0x278d10 in main).
    const uint32_t place = sound >> 24;
    const uint32_t program = (sound >> 16) & 0xff;
    const uint32_t key = (sound >> 8) & 0xff;
    const uint32_t velocity = sound & 0xff;

    // Bank_Get, InstChange, GetInstVmap, WaveidToWavegroup and GetSoundHandle.
    const Instrument* inst = banks_.FindInstrument(place, program);
    if (inst == nullptr)
    {
        return kNone;
    }

    std::size_t keymap = 0;
    const ntdlib::jaudio::VelocityRegion* region = FindRegion(inst, key, velocity, keymap);
    if (region == nullptr)
    {
        return kNone;
    }

    const WaveHandle* handle = RegionWave(place, *region);
    if (handle == nullptr)
    {
        return kNone;
    }

    const uint32_t channel = GetLogical(manager, handle);
    if (channel == kNone)
    {
        return kNone;
    }

    Channel& c = channels_[channel];
    const WaveRecord& record = *c.wave;
    const int32_t k = ClampKey(static_cast<int32_t>(key) - record.info.base_key + 60);
    c.velocity = static_cast<uint8_t>(velocity);
    c.key = static_cast<uint8_t>(key);
    c.base_pitch = ((record.info.sample_rate / kDacRate) * region->pitch) * inst->pitch;
    c.pitch = KeyPitch(static_cast<uint32_t>(k)) * c.base_pitch;
    c.base_volume = region->volume * inst->volume;
    c.volume = VelocityVolume(velocity) * c.base_volume;
    StartLevels(c, 0.5f);

    SetUpEffects(channel, inst, place, sound);

    c.pans[Channel::kPan][Channel::kSound] = ClampUnit(c.pans[Channel::kPan][Channel::kSound]);
    c.pans[Channel::kFxmix][Channel::kSound] = ClampUnit(c.pans[Channel::kFxmix][Channel::kSound]);

    const uint8_t flags = banks_.InstrumentFlags(place, program);
    const uint32_t group = VoiceGroup(flags, place, program, keymap);

    return LimitVoices(manager, channel, group, flags, time);
}

void Channels::SetUpEffects(uint32_t channel, const Instrument* inst, uint32_t place, uint32_t sound)
{
    // The effects in the game's order: for each slot, the effect that follows the key or velocity, the random one and
    // the oscillator. The first slot's key or velocity comes from the sound, the second's from the channel.
    Channel& c = channels_[channel];
    for (std::size_t slot = 0; slot < 2; slot++)
    {
        const InstEffect* sense = EffectAt(inst->effects, InstEffect::Kind::kSense, slot);
        if (sense != nullptr)
        {
            uint32_t value = 0;
            if (sense->trigger == 1)
            {
                value = slot == 0 ? sound : c.velocity;
            }
            else if (sense->trigger == 2)
            {
                value = slot == 0 ? sound >> 8 : c.key;
            }
            DoEffect(channel, sense->target, SenseToOfs(sense, value));
        }

        const InstEffect* random = EffectAt(inst->effects, InstEffect::Kind::kRandom, slot);
        if (random != nullptr)
        {
            DoEffect(channel, random->target, RandToOfs(random, random_));
        }

        if (slot < inst->oscillators.size())
        {
            const OscData* osc = banks_.Oscillator(place, inst->oscillators[slot]);
            c.osc_states[slot].state = OscState::kStart;
            c.oscillators[slot] = osc;
            const float value = OscToOfs(osc, c.osc_states[slot]);
            if (osc != nullptr)
            {
                DoEffectOsc(channel, osc->target, value);
            }
        }
        else
        {
            c.oscillators[slot] = nullptr;
        }
    }
}

uint32_t Channels::LimitVoices(uint32_t manager, uint32_t channel, uint32_t group, uint8_t flags, int32_t time)
{
    Channel& c = channels_[channel];
    c.sound = 0;
    const uint32_t polys_class = (group >> 24) & 0xf;
    if (polys_class == 0)
    {
        c.sound = group;
        return PlayStart(manager, channel, time);
    }

    const uint32_t polys = kPolys[polys_class];
    const ChannelManager& m = managers_[c.manager];
    if ((group & 0x20000000) != 0)
    {
        // A count of the group's voices. Past the limit, the note either takes no channel (bit 4) or starts paused.
        uint32_t count = 0;
        for (const uint8_t list : {ChannelManager::kActive, ChannelManager::kReleasing})
        {
            for (uint32_t o = m.lists[list]; o != kNone; o = channels_[o].next)
            {
                count += channels_[o].sound == group ? 1 : 0;
            }
        }
        c.voices = static_cast<uint8_t>(count);
        if (count >= polys)
        {
            if ((flags & 0x10) != 0)
            {
                ListAddTail(manager, ChannelManager::kFree, channel);
                return kNone;
            }

            c.sound = group;
            const uint32_t started = PlayStart(manager, channel, time);
            if (started != kNone)
            {
                channels_[started].pause = true;
                channels_[started].flush = 1;
            }
            return started;
        }

        c.sound = group;
        return PlayStart(manager, channel, time);
    }

    CountNewVoice(m, group, polys);

    c.voices = 0;
    c.sound = group;

    return PlayStart(manager, channel, time);
}

void Channels::CountNewVoice(const ChannelManager& m, uint32_t group, uint32_t polys)
{
    // Each voice of the group counts one more before it. The one that reaches the limit stops (bit 28) or pauses,
    // and a releasing one stops.
    for (uint32_t o = m.lists[ChannelManager::kActive]; o != kNone; o = channels_[o].next)
    {
        Channel& other = channels_[o];
        if (other.sound != group)
        {
            continue;
        }

        other.voices++;
        if (polys == other.voices)
        {
            if ((group & 0x10000000) != 0)
            {
                ForceStopLogical(o);
            }
            else
            {
                other.pause = true;
                other.flush = 1;
            }
        }
    }
    for (uint32_t o = m.lists[ChannelManager::kReleasing]; o != kNone; o = channels_[o].next)
    {
        Channel& other = channels_[o];
        if (other.sound != group)
        {
            continue;
        }

        other.voices++;
        if (polys == other.voices)
        {
            ForceStopLogical(o);
        }
    }
}

uint32_t Channels::PlayPerc(uint32_t manager, uint32_t sound, int32_t time)
{
    // Play_1shot_Perc (0x279500 in main). The game reads past a drum set's 128 keys for a higher key. The model
    // starts no note there.
    const uint32_t place = sound >> 24;
    const uint32_t program = (sound >> 16) & 0xff;
    const uint32_t key = (sound >> 8) & 0xff;
    const uint32_t velocity = sound & 0xff;
    const ntdlib::jaudio::DrumSet* drums = banks_.FindDrums(place, program);
    if (drums == nullptr || key >= drums->keys.size() || !drums->keys[key])
    {
        return kNone;
    }

    const Drum& drum = *drums->keys[key];
    const ntdlib::jaudio::VelocityRegion* region = jaudio2::FindVelocity<int64_t>(drum.velocities, velocity);
    if (region == nullptr)
    {
        return kNone;
    }

    const WaveHandle* handle = RegionWave(place, *region);
    if (handle == nullptr)
    {
        return kNone;
    }

    const uint32_t channel = GetLogical(manager, handle);
    if (channel == kNone)
    {
        return kNone;
    }

    // A drum plays at its wave's rate, with no key's pitch. A drum set of the second kind (PER2) gives each key a
    // pan and a release. The first kind plays in the middle with a release of 1,000.
    Channel& c = channels_[channel];
    const WaveRecord& record = *c.wave;
    c.velocity = static_cast<uint8_t>(velocity);
    c.key = static_cast<uint8_t>(key);
    c.base_pitch = ((record.info.sample_rate / kDacRate) * region->pitch) * drum.pitch;
    c.pitch = c.base_pitch;
    c.base_volume = region->volume * drum.volume;
    c.volume = VelocityVolume(velocity) * c.base_volume;
    StartLevels(c, drum.pan);

    const InstEffect* first = EffectAt(drum.effects, InstEffect::Kind::kRandom, 0);
    if (first != nullptr)
    {
        DoEffect(channel, first->target, RandToOfs(first, random_));
    }
    c.oscillators[0] = nullptr;
    const InstEffect* second = EffectAt(drum.effects, InstEffect::Kind::kRandom, 1);
    if (second != nullptr)
    {
        DoEffect(channel, second->target, RandToOfs(second, random_));
    }

    c.osc_states[0].state = OscState::kStart;
    c.oscillators[0] = &kPercEnvelope;
    c.oscillators[1] = nullptr;
    OscToOfs(&kPercEnvelope, c.osc_states[0]);
    c.osc_states[0].direct_release = drum.release;

    c.pans[Channel::kPan][Channel::kSound] = ClampUnit(c.pans[Channel::kPan][Channel::kSound]);
    c.pans[Channel::kFxmix][Channel::kSound] = ClampUnit(c.pans[Channel::kFxmix][Channel::kSound]);
    c.sound = 0;

    return PlayStart(manager, channel, time);
}

uint32_t Channels::PlayOsc(uint32_t manager, uint32_t sound, int32_t time)
{
    // Play_1shot_Osc (0x279810 in main): one of the DSP's oscillators by the program's distance from 0xf0.
    const uint32_t channel = GetLogical(manager, nullptr);
    if (channel == kNone)
    {
        return kNone;
    }

    const uint32_t key = (sound >> 8) & 0xff;
    Channel& c = channels_[channel];
    c.kind = Channel::kOscillator;
    c.source = ((sound >> 16) & 0xff) - 0xf0;
    c.velocity = static_cast<uint8_t>(sound);
    c.key = static_cast<uint8_t>(key);
    c.base_pitch = kOscillatorPitch / kDacRate;
    c.pitch = KeyPitch(std::min(key, 0x7fu)) * c.base_pitch;
    c.base_volume = 1.0f;
    c.volume = VelocityVolume(sound);
    StartLevels(c, 0.5f);
    c.oscillators[0] = &kOscEnvelope;
    c.oscillators[1] = nullptr;
    c.osc_states[0].state = OscState::kStart;
    OscToOfs(&kOscEnvelope, c.osc_states[0]);
    c.sound = 0;

    return PlayStart(manager, channel, time);
}

void Channels::StopBody(uint32_t channel)
{
    // The release in Stop_1Shot (0x278410 in main): the first two oscillators release unless they already stop. A
    // channel whose oscillators release moves to the releasing list, and its DSP channel drops to the release's
    // priority. The note then has no time.
    Channel& c = channels_[channel];
    bool released = false;
    for (std::size_t k = 0; k < 2; k++)
    {
        if (c.oscillators[k] != nullptr && (c.osc_states[k].state & 0xfe) != OscState::kForceStop)
        {
            c.osc_states[k].state = OscState::kRelease;
            released = true;
        }
    }

    if (released && ListCut(channel) != -1)
    {
        ListAddTail(c.manager, ChannelManager::kReleasing, channel);
        if (c.dsp_channel != kNone)
        {
            dsp_channels_[c.dsp_channel].priority =
                (c.priority & 0xff00) != 0 ? static_cast<uint8_t>(c.priority >> 8) : uint8_t{1};
        }
    }
    c.time = -1;
}

void Channels::Stop(uint32_t channel)
{
    // Stop_1Shot (0x278410 in main). A channel that waits for a DSP channel leaves the queue.
    if (channels_[channel].dsp_channel == kNone)
    {
        UpdateOneShot(channel, kStoppedWaiting);
        return;
    }

    StopBody(channel);
}

void Channels::StopRelease(uint32_t channel, uint16_t release)
{
    // Stop_1Shot_R (0x2787f0 in main).
    if (channels_[channel].dsp_channel == kNone)
    {
        UpdateOneShot(channel, kStoppedWaiting);
        return;
    }

    channels_[channel].osc_states[0].direct_release = release;
    StopBody(channel);
}

void Channels::AllStop(uint32_t manager)
{
    // AllStop_1Shot (0x2788e0 in main).
    for (uint32_t channel = managers_[manager].lists[ChannelManager::kActive]; channel != kNone;)
    {
        const uint32_t next = channels_[channel].next;
        if (channels_[channel].dsp_channel == kNone)
        {
            UpdateOneShot(channel, kStoppedWaiting);
        }
        else
        {
            StopBody(channel);
        }
        channel = next;
    }
}

void Channels::FlushRelease(uint32_t manager)
{
    // FlushRelease_1Shot (0x278bd0 in main): each releasing channel's first two oscillators stop at once, and the
    // channel moves to the list's end.
    uint32_t count = ListCount(manager, ChannelManager::kReleasing);
    while (count != 0)
    {
        const uint32_t channel = ListGet(manager, ChannelManager::kReleasing);
        if (channel == kNone)
        {
            return;
        }

        Channel& c = channels_[channel];
        for (std::size_t k = 0; k < 2; k++)
        {
            if (c.oscillators[k] != nullptr && (c.osc_states[k].state & 0xfe) != OscState::kForceStop)
            {
                c.osc_states[k].state = OscState::kForceStop;
            }
        }
        ListAddTail(manager, ChannelManager::kReleasing, channel);
        count--;
    }
}

void Channels::StopVoices(uint32_t channel)
{
    // StopMonoPolyCheck (in Jesus1Shot_Update): the group's later voices count one fewer. One that drops below the
    // limit, or to one below it in counting mode (bit 29), plays on.
    Channel& c = channels_[channel];
    const uint32_t group = c.sound;
    c.sound = 0;
    const uint32_t polys_class = (group >> 24) & 0xf;
    if (group == 0 || polys_class == 0)
    {
        return;
    }

    const uint32_t polys = kPolys[polys_class];
    const bool counting = (group & 0x20000000) != 0;
    for (uint32_t o = managers_[c.manager].lists[ChannelManager::kActive]; o != kNone; o = channels_[o].next)
    {
        Channel& other = channels_[o];
        if (other.sound != group || other.voices <= c.voices)
        {
            continue;
        }

        other.voices--;
        if (counting ? polys - 1 == other.voices : polys > other.voices)
        {
            other.pause = false;
            other.flush = 1;
        }
    }
}

void Channels::UpdateOneShot(uint32_t channel, CallbackStatus status)
{
    // Jesus1Shot_Update (0x278500 in main). Status 1 (the wave's end), 2 (the envelope's end) and 6 (a stop while
    // the channel waits): the channel goes back to the free list, or to the global manager's for a channel that the
    // manager took for a note. Status 0 (the note's time is over): the release.
    Channel& c = channels_[channel];
    if (status == kWaveEnd || status == kEnvelopeEnd || status == kStoppedWaiting)
    {
        const uint32_t allocations = managers_[c.manager].allocations;
        if (ListCut(channel) != -1)
        {
            if (allocations != 0)
            {
                managers_[c.manager].allocations--;
                StopVoices(channel);
                FixRelease(channel);
            }
            else
            {
                StopVoices(channel);
                ListAdd(c.manager, ChannelManager::kFree, channel);
            }
        }

        if (status == kStoppedWaiting)
        {
            DeleteWait(channel);
        }
        else
        {
            StopLogical(channel);
        }
        c.key = 0xff;
        c.time = -1;
        c.callback = Channel::kNoCallback;
    }
    else if (status == kTimeOver)
    {
        StopBody(channel);
    }
}

void Channels::RunCallback(uint32_t channel, CallbackStatus status)
{
    if (channels_[channel].callback == Channel::kOneShot)
    {
        UpdateOneShot(channel, status);
    }
}

bool Channels::SweepUpdate(uint32_t channel)
{
    // Extra_Update (0x278a10 in main): a step of the sweep to the target pitch.
    Channel& c = channels_[channel];
    if (c.sweep_steps == 0)
    {
        return false;
    }

    const float distance = c.target_pitch - c.pitch;
    if (kFrameStep >= c.sweep_steps)
    {
        c.sweep_steps = 1;
    }
    const auto steps = static_cast<float>(c.sweep_steps);
    c.sweep_steps = static_cast<uint16_t>(c.sweep_steps - kFrameStep);
    c.pitch = c.pitch + distance / steps;
    if (c.sweep_steps == 0)
    {
        c.sweep = Channel::kNoSweep;
    }

    return true;
}

void Channels::SetKeyTarget(uint32_t channel, uint32_t key, uint32_t steps)
{
    // SetKeyTarget_1Shot (0x278a80 in main).
    Channel& c = channels_[channel];
    const float pitch = KeyPitchOf(c, key);
    if (steps != 0)
    {
        c.target_pitch = pitch;
        c.sweep_steps = static_cast<uint16_t>(steps);
        c.sweep = Channel::kPitchSweep;
    }
    else
    {
        c.pitch = pitch;
        c.sweep = Channel::kNoSweep;
    }
}

void Channels::Gate(uint32_t channel, uint32_t key, uint32_t velocity, int32_t time)
{
    // Gate_1Shot (0x278b00 in main). The game reads a wave channel's record without a check. The model takes the key
    // as it is for a channel without one.
    Channel& c = channels_[channel];
    if (c.time != -1)
    {
        return;
    }

    c.time = time;
    c.countdown = time;
    c.key = static_cast<uint8_t>(key);
    c.pitch = KeyPitchOf(c, key);
    c.velocity = static_cast<uint8_t>(velocity);
    c.volume = VelocityVolume(velocity) * c.base_volume;
}

void Channels::SetPanPower(uint32_t channel, float a, float b, float c)
{
    // UpdatePanPower_1Shot (0x278ba0 in main).
    const float sum = (a + b) + c;
    if (sum == 0.0f)
    {
        return;
    }

    channels_[channel].pans[Channel::kPower] = {a / sum, b / sum, c / sum};
}

void Channels::OverwriteOscillator(uint32_t channel, const OscData* data, uint32_t slot)
{
    // Effecter_Overwrite_1ShotD (0x278360 in main). The game reads the new oscillator's target without a check.
    if (slot > 3)
    {
        return;
    }

    Channel& c = channels_[channel];
    c.osc_states[slot].state = OscState::kStart;
    c.oscillators[slot] = data;
    const float value = OscToOfs(data, c.osc_states[slot]);
    if (data != nullptr)
    {
        DoEffectOsc(channel, data->target, value);
    }
}

uint32_t Channels::CheckInstWave(uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity) const
{
    // One_CheckInstWave (0x278c70 in main). InstChange takes a drum set's program as an instrument. A drum set holds
    // no key regions there. Its program therefore gives 2. A program past the drum sets gives 1: the bank file holds no
    // instrument there up to 0xf2, and the game reads other data above.
    if (banks_.FindDrums(bank, program) != nullptr)
    {
        return 2;
    }

    const Instrument* inst = banks_.FindInstrument(bank, program);
    if (inst == nullptr)
    {
        return 1;
    }

    const ntdlib::jaudio::VelocityRegion* region = nullptr;
    for (const ntdlib::jaudio::KeyRegion& keymap : inst->keys)
    {
        if (keymap.high_key >= (key & 0xff))
        {
            region = jaudio2::FindVelocity<int64_t>(keymap.velocities, velocity & 0xff);
            break;
        }
    }
    if (region == nullptr)
    {
        return 2;
    }

    const uint32_t system = banks_.BankSystem(bank);
    if (system == Banks::kNone)
    {
        return 3;
    }

    return banks_.FindSound(system, region->wave_id) != nullptr ? 0 : 4;
}

} // namespace ntdlib::jac

// SPDX-License-Identifier: MIT

// The voices in the model of the early EAD engine: the voice lists, the allocation of voices to layers, the envelopes
// and vibratos, and the processing of the voices at each update.

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "ead_early_constants.h"
#include "ead_lists.h"
#include "ead_tables.h"
#include "ntdlib/n64/ead_early.h"

namespace ntdlib::n64
{

namespace
{

using ead_early_constants::kGlobalPool;
using ead_early_constants::kMuteSoften;
using ead_early_constants::kMuteStopNotes;
using ead_early_constants::kMuteStopScript;
using ead_early_constants::kPriorityDisabled;
using ead_early_constants::kPriorityMinimum;
using ead_early_constants::kPriorityStopping;
using ead_early_constants::kStateFadeIn;
using ead_early_constants::kStateFadeOut;
using ead_early_constants::kStateFadeTo;
using ead_lists::WriteBe16;

namespace tables = ead_tables;

constexpr uint8_t kActionDecay = 0x20;
constexpr uint8_t kActionRelease = 0x10;

constexpr uint8_t kPolicyLayer = 1;
constexpr uint8_t kPolicyChannel = 2;
constexpr uint8_t kPolicySequence = 4;
constexpr uint8_t kPolicyGlobal = 8;

// A portamento's scale of the frequency, at a place from 0 to 127 in the engine's table of bends.
float PortamentoScale(float extent, float position)
{
    uint32_t index = static_cast<uint32_t>(position);
    if (index >= 127)
    {
        index = 127;
    }

    return 1.0F + extent * (tables::kBendScale[index + 127] - 1.0F);
}

// Steps a vibrato extent or rate toward the channel target. A new target starts a ramp over `delay` updates. A delay
// of 0 sets the target at once.
void RampVibratoValue(uint16_t& value, uint16_t& timer, uint16_t target, uint16_t delay)
{
    if (timer != 0)
    {
        if (timer == 1)
        {
            value = target;
        }
        else
        {
            value = static_cast<uint16_t>(value + (int32_t{target} - value) / int32_t{timer});
        }

        timer--;
    }
    else if (target != value)
    {
        timer = delay;
        if (timer == 0)
        {
            value = target;
        }
    }
}

} // namespace

int EadEarlyPlayer::Head(int pool, ListKind kind) const
{
    return static_cast<int>(voices_.size()) + pool * 4 + kind;
}

int EadEarlyPlayer::PlayerPool(int player) const
{
    return 1 + player;
}

int EadEarlyPlayer::ChannelPool(int channel) const
{
    return 1 + kPlayers + channel;
}

void EadEarlyPlayer::PushBack(int head, int voice)
{
    const auto v = static_cast<std::size_t>(voice);
    if (previous_[v] != -1)
    {
        return;
    }

    const auto h = static_cast<std::size_t>(head);
    previous_[v] = previous_[h];
    next_[v] = head;
    next_[static_cast<std::size_t>(previous_[h])] = voice;
    previous_[h] = voice;
    voices_[v].pool = (head - static_cast<int>(voices_.size())) / 4;
}

void EadEarlyPlayer::PushFront(int head, int voice)
{
    const auto v = static_cast<std::size_t>(voice);
    if (previous_[v] != -1)
    {
        return;
    }

    const auto h = static_cast<std::size_t>(head);
    previous_[v] = head;
    next_[v] = next_[h];
    previous_[static_cast<std::size_t>(next_[h])] = voice;
    next_[h] = voice;
    voices_[v].pool = (head - static_cast<int>(voices_.size())) / 4;
}

int EadEarlyPlayer::PopBack(int head)
{
    const int voice = previous_[static_cast<std::size_t>(head)];
    if (voice == head)
    {
        return -1;
    }

    Remove(voice);

    return voice;
}

void EadEarlyPlayer::Remove(int voice)
{
    const auto v = static_cast<std::size_t>(voice);
    if (previous_[v] == -1)
    {
        return;
    }

    next_[static_cast<std::size_t>(previous_[v])] = next_[v];
    previous_[static_cast<std::size_t>(next_[v])] = previous_[v];
    previous_[v] = -1;
}

void EadEarlyPlayer::ClearPool(int pool)
{
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        const int source = Head(pool, kind);
        const int target = Head(kGlobalPool, kind);
        for (int moved = 0; moved <= static_cast<int>(voices_.size()); moved++)
        {
            const int voice = next_[static_cast<std::size_t>(source)];
            if (voice == source)
            {
                break;
            }

            Remove(voice);
            PushBack(target, voice);
        }
    }
}

void EadEarlyPlayer::FillPool(int pool, int count)
{
    ClearPool(pool);
    int taken = 0;
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        while (taken < count)
        {
            const int voice = PopBack(Head(kGlobalPool, kind));
            if (voice == -1)
            {
                break;
            }

            PushBack(Head(pool, kind), voice);
            taken++;
        }

        if (taken >= count)
        {
            return;
        }
    }
}

int EadEarlyPlayer::PopLowerPriority(int head, int limit)
{
    int current = next_[static_cast<std::size_t>(head)];
    if (current == head)
    {
        return -1;
    }

    int best = current;
    for (; current != head; current = next_[static_cast<std::size_t>(current)])
    {
        if (voices_[static_cast<std::size_t>(best)].priority >= voices_[static_cast<std::size_t>(current)].priority)
        {
            best = current;
        }
    }

    if (limit < voices_[static_cast<std::size_t>(best)].priority)
    {
        return -1;
    }

    Remove(best);

    return best;
}

EadEnvelope::Point EadEarlyPlayer::EnvelopePoint(const EnvelopeRef& envelope, int index) const
{
    if (envelope.bank != nullptr)
    {
        const auto at = static_cast<std::size_t>(index);
        if (at < envelope.bank->points.size())
        {
            return envelope.bank->points[at];
        }

        return {kEnvelopeHang, 0};
    }

    if (envelope.player >= 0)
    {
        const Player& player = players_[static_cast<std::size_t>(envelope.player)];
        const uint32_t place = envelope.offset + static_cast<uint32_t>(index) * 4;
        const auto delay =
            static_cast<int16_t>(static_cast<uint16_t>(PeekByte(player, place) << 8) | PeekByte(player, place + 1));
        const auto target =
            static_cast<int16_t>(static_cast<uint16_t>(PeekByte(player, place + 2) << 8) | PeekByte(player, place + 3));

        return {delay, target};
    }

    return {kEnvelopeHang, 0};
}

void EadEarlyPlayer::ChannelSound(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    float volume = channel.volume * channel.volume_scale * player.fade_volume;
    if (player.muted && (channel.mute_behavior & kMuteSoften) != 0)
    {
        volume *= player.mute_scale;
    }

    const float pan_from_channel = channel.pan * channel.pan_weight;
    const float layer_weight = 1.0F - channel.pan_weight;
    for (const int layer_index : channel.layers)
    {
        if (layer_index == -1)
        {
            continue;
        }

        Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
        if (layer.enabled && layer.voice != -1)
        {
            layer.note_frequency = layer.frequency_scale * channel.frequency_scale;
            layer.note_velocity = layer.velocity_square * volume;
            layer.note_pan = (layer.pan * layer_weight) + pan_from_channel;
        }
    }
}

void EadEarlyPlayer::PlayerSound(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (player.fade_updates != 0)
    {
        player.fade_volume += player.fade_step;
        if (player.fade_volume > 1.0F)
        {
            player.fade_volume = 1.0F;
        }

        if (player.fade_volume < 0.0F)
        {
            player.fade_volume = 0.0F;
        }

        player.fade_updates--;
        if (player.fade_updates == 0)
        {
            switch (player.state)
            {
            case kStateFadeOut:
                DisablePlayer(index);
                return;
            case kStateFadeIn:
            case kStateFadeTo:
                player.state = 0;
                break;
            default:
                break;
            }
        }
    }

    for (const int slot : player.channels)
    {
        if (slot >= 0 && channels_[static_cast<std::size_t>(slot)].enabled)
        {
            ChannelSound(slot);
        }
    }
}

float EadEarlyPlayer::VibratoScale(Vibrato& vibrato)
{
    if (vibrato.delay != 0)
    {
        vibrato.delay--;

        return 1.0F;
    }

    const Channel& channel = channels_[static_cast<std::size_t>(vibrato.channel)];
    RampVibratoValue(vibrato.extent, vibrato.extent_timer, channel.vibrato_extent_target, channel.vibrato_extent_delay);
    RampVibratoValue(vibrato.rate, vibrato.rate_timer, channel.vibrato_rate_target, channel.vibrato_rate_delay);
    if (vibrato.extent == 0)
    {
        return 1.0F;
    }

    // The wave's quarter, read forward and backward, then negated.
    vibrato.time += vibrato.rate;
    int wave_index = static_cast<int>((vibrato.time >> 10) & 0x3F);
    int change = 0;
    switch (wave_index & 0x30)
    {
    case 0x10:
        change = tables::kVibratoCurve[static_cast<std::size_t>(31 - wave_index)];
        break;
    case 0x00:
        change = tables::kVibratoCurve[static_cast<std::size_t>(wave_index)];
        break;
    case 0x20:
        change = -tables::kVibratoCurve[static_cast<std::size_t>(wave_index - 0x20)];
        break;
    default:
        change = -tables::kVibratoCurve[static_cast<std::size_t>(63 - wave_index)];
        break;
    }

    const float extent = static_cast<float>(static_cast<int32_t>(vibrato.extent)) / 4096.0F;

    return 1.0F + extent * (tables::kBendScale[static_cast<std::size_t>(static_cast<int8_t>(change) + 127)] - 1.0F);
}

void EadEarlyPlayer::UpdateVibrato(Voice& voice)
{
    if (voice.vibrato.active)
    {
        if (voice.portamento.mode == 0)
        {
            voice.portamento_scale = 1.0F;
        }
        else
        {
            voice.portamento.position += voice.portamento.speed;
            voice.portamento_scale = PortamentoScale(voice.portamento.extent, voice.portamento.position);
        }

        if (voice.layer != -1)
        {
            voice.vibrato_scale = VibratoScale(voice.vibrato);
        }
    }
}

void EadEarlyPlayer::InitVibrato(int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    voice.vibrato_scale = 1.0F;
    voice.portamento_scale = 1.0F;
    Vibrato& vibrato = voice.vibrato;
    const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    if (channel.vibrato_extent_start == 0 && channel.vibrato_extent_target == 0 && layer.portamento.mode == 0)
    {
        vibrato.active = false;
        return;
    }

    vibrato.active = true;
    vibrato.time = 0;
    vibrato.channel = layer.channel;
    vibrato.extent_timer = channel.vibrato_extent_delay;
    vibrato.extent = vibrato.extent_timer == 0 ? channel.vibrato_extent_target : channel.vibrato_extent_start;
    vibrato.rate_timer = channel.vibrato_rate_delay;
    vibrato.rate = vibrato.rate_timer == 0 ? channel.vibrato_rate_target : channel.vibrato_rate_start;
    vibrato.delay = channel.vibrato_delay;
    voice.portamento = layer.portamento;
}

void EadEarlyPlayer::UpdateAdsr(Voice& voice)
{
    Adsr& adsr = voice.adsr;
    const uint8_t action = adsr.action;
    switch (adsr.phase)
    {
    case AdsrPhase::kDisabled:
        return;
    case AdsrPhase::kInitial:
        adsr.current = 0;
        adsr.target = 0;
        [[fallthrough]];
    case AdsrPhase::kStartLoop:
        adsr.index = 0;
        adsr.current_fine = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(adsr.current)) << 16);
        adsr.phase = AdsrPhase::kLoop;
        [[fallthrough]];
    case AdsrPhase::kLoop:
        ReadEnvelopePoint(adsr);
        if (adsr.phase != AdsrPhase::kFade)
        {
            break;
        }

        [[fallthrough]];
    case AdsrPhase::kFade:
        adsr.current_fine =
            static_cast<int32_t>(static_cast<uint32_t>(adsr.current_fine) + static_cast<uint32_t>(adsr.velocity));
        adsr.current = static_cast<int16_t>(adsr.current_fine >> 16);
        adsr.delay--;
        if (adsr.delay <= 0)
        {
            adsr.phase = AdsrPhase::kLoop;
        }

        break;
    case AdsrPhase::kHang:
        break;
    case AdsrPhase::kDecay:
    case AdsrPhase::kRelease:
        StepDecayOrRelease(adsr);
        break;
    case AdsrPhase::kSustain:
        adsr.delay--;
        if (adsr.delay == 0)
        {
            adsr.phase = AdsrPhase::kRelease;
        }

        break;
    }

    if ((action & kActionDecay) != 0)
    {
        adsr.phase = AdsrPhase::kDecay;
        adsr.action = static_cast<uint8_t>(action & ~kActionDecay);
    }

    if ((action & kActionRelease) != 0)
    {
        adsr.phase = AdsrPhase::kRelease;
        adsr.action = static_cast<uint8_t>(action & ~(kActionRelease | kActionDecay));
    }

    voice.adsr_volume = adsr.current;
}

// Reads the envelope point at the index. A point with a delay starts a fade to its target. A command point disables,
// holds, jumps or restarts the envelope.
void EadEarlyPlayer::ReadEnvelopePoint(Adsr& adsr) const
{
    const EadEnvelope::Point point = EnvelopePoint(adsr.envelope, adsr.index);
    adsr.delay = point.delay;
    switch (adsr.delay)
    {
    case kEnvelopeDisable:
        adsr.phase = AdsrPhase::kDisabled;
        break;
    case kEnvelopeHang:
        adsr.phase = AdsrPhase::kHang;
        break;
    case kEnvelopeGoto:
        adsr.index = point.target;
        break;
    case kEnvelopeRestart:
        adsr.phase = AdsrPhase::kInitial;
        break;
    default:
        adsr.target = point.target;
        adsr.velocity =
            static_cast<int32_t>(static_cast<uint32_t>(int32_t{adsr.target} - adsr.current) << 16) / adsr.delay;
        adsr.phase = AdsrPhase::kFade;
        adsr.index++;
        break;
    }
}

// Lowers the level of a decaying or releasing envelope by its step. A decay with a sustain level stops there. Any other
// fade ends the envelope once the level falls below 100.
void EadEarlyPlayer::StepDecayOrRelease(Adsr& adsr)
{
    adsr.current = static_cast<int16_t>(adsr.current - adsr.fade_step);
    if (adsr.sustain != 0 && adsr.phase == AdsrPhase::kDecay)
    {
        if (adsr.current < adsr.sustain)
        {
            adsr.current = adsr.sustain;
            adsr.delay = static_cast<int16_t>(adsr.sustain / 16);
            adsr.phase = AdsrPhase::kSustain;
        }

        return;
    }

    if (adsr.current < 100)
    {
        adsr.current = 0;
        adsr.phase = AdsrPhase::kDisabled;
    }
}

void EadEarlyPlayer::DecayOrRelease(int layer_index, AdsrPhase target)
{
    if (layer_index < 0)
    {
        return;
    }

    Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    if (layer.voice == -1)
    {
        return;
    }

    const int voice_index = layer.voice;
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    const bool has_channel = layer.channel != -1;
    if (has_channel && channels_[static_cast<std::size_t>(layer.channel)].policy == 0)
    {
        layer.voice = -1;
    }

    if (voice.wanted_layer == layer_index)
    {
        voice.wanted_layer = -1;
    }

    if (voice.layer != layer_index)
    {
        return;
    }

    layer.loaded = false;
    if (voice.adsr.phase != AdsrPhase::kDecay)
    {
        voice.saved_frequency = layer.note_frequency;
        voice.saved_velocity = layer.note_velocity;
        voice.saved_pan = layer.note_pan;
        if (has_channel)
        {
            voice.saved_reverb = channels_[static_cast<std::size_t>(layer.channel)].reverb;
        }

        voice.priority = kPriorityStopping;
        voice.previous_layer = voice.layer;
        voice.layer = -1;
        if (target == AdsrPhase::kRelease)
        {
            voice.adsr.fade_step = static_cast<int16_t>(0x8000 / updates_per_frame_);
            voice.adsr.action |= kActionRelease;
        }
        else
        {
            voice.adsr.action |= kActionDecay;
            const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
            const int release = layer.adsr.release == 0 ? channel.adsr.release : layer.adsr.release;
            voice.adsr.fade_step = static_cast<int16_t>(release * 24);
            voice.adsr.sustain = static_cast<int16_t>((int32_t{voice.adsr.current} * channel.adsr.sustain) / 0x10000);
        }
    }

    if (target == AdsrPhase::kDecay)
    {
        Remove(voice_index);
        PushFront(Head(voice.pool, kDecaying), voice_index);
    }
}

void EadEarlyPlayer::BuildWave(Voice& voice, Layer& layer, AudioMemory* memory)
{
    const uint8_t previous_count = voice.sample_count;
    int step = 0;
    if (layer.frequency_scale < 1.0F)
    {
        voice.sample_count = 64;
        layer.frequency_scale *= 1.0465F;
        step = 1;
    }
    else if (layer.frequency_scale < 2.0F)
    {
        voice.sample_count = 32;
        layer.frequency_scale *= 0.52325F;
        step = 2;
    }
    else if (layer.frequency_scale < 4.0F)
    {
        voice.sample_count = 16;
        layer.frequency_scale *= 0.26263F;
        step = 4;
    }
    else
    {
        voice.sample_count = 8;
        layer.frequency_scale *= 0.13081F;
        step = 8;
    }

    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    if (voice.sample_count == previous_count && channel.program == voice.program)
    {
        return;
    }

    voice.program = channel.program;
    if (memory == nullptr)
    {
        return;
    }

    const auto wave = static_cast<std::size_t>(channel.program - 0x80) & 3;
    for (int i = 0; i < 64; i++)
    {
        // The wave's samples at the step, repeated to fill 64 samples.
        const int source = (i % voice.sample_count) * step;
        WriteBe16(*memory, voice.buffers.samples + static_cast<uint32_t>(i) * 2,
                  tables::kWaves[wave][static_cast<std::size_t>(source)]);
    }
}

void EadEarlyPlayer::InitWave(Voice& voice, Layer& layer)
{
    const int previous_count = voice.sample_count;
    BuildWave(voice, layer, build_memory_);
    if (previous_count != 0)
    {
        voice.position *= voice.sample_count / previous_count;
    }
    else
    {
        voice.position = 0;
    }
}

void EadEarlyPlayer::EnableVoice(Voice& voice)
{
    voice.enabled = true;
    voice.needs_init = true;
    voice.restart = false;
    voice.finished = false;
    voice.strong_right = false;
    voice.strong_left = false;
    voice.headset_effects = false;
    voice.headset_left = 0;
    voice.headset_right = 0;
    voice.previous_headset_right = 0;
    voice.previous_headset_left = 0;
}

void EadEarlyPlayer::DisableVoice(Voice& voice)
{
    if (voice.needs_init)
    {
        voice.needs_init = false;
    }
    else
    {
        SetVelocityPanReverb(voice, 0.0F, 0.5F, 0);
    }

    voice.priority = kPriorityDisabled;
    voice.enabled = false;
    voice.finished = false;
    voice.layer = -1;
    voice.previous_layer = -1;
}

void EadEarlyPlayer::InitVoiceForLayer(int voice_index, int layer_index)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    voice.previous_layer = -1;
    voice.layer = layer_index;
    voice.priority = channel.priority;
    voice.bank = channel.bank;
    voice.stereo_effects = channel.stereo_effects;
    voice.sound = layer.sound;
    layer.loaded = true;
    layer.voice = voice_index;
    if (voice.sound == nullptr)
    {
        BuildWave(voice, layer, build_memory_);
    }

    voice.adsr.action = 0;
    voice.adsr.delay = 0;
    voice.adsr.velocity = 0;
    voice.adsr.envelope = layer.adsr.release == 0 ? channel.adsr.envelope : layer.adsr.envelope;
    voice.adsr.phase = AdsrPhase::kInitial;
    voice.target_left = 0;
    voice.target_right = 0;
    voice.reverb = 0;
    voice.reverb_shifted = 0;
    voice.current_left = 1;
    voice.current_right = 1;
    voice.frequency = 0.0F;
    EnableVoice(voice);
}

void EadEarlyPlayer::ReleaseAndTake(int voice_index, int layer_index)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    voice.wanted_layer = layer_index;
    voice.priority = kPriorityStopping;
    voice.adsr.fade_step = static_cast<int16_t>(0x8000 / updates_per_frame_);
    voice.adsr.action |= kActionRelease;
}

int EadEarlyPlayer::AllocFromDisabled(int pool, int layer)
{
    const int voice = PopBack(Head(pool, kDisabled));
    if (voice != -1)
    {
        InitVoiceForLayer(voice, layer);
        PushFront(Head(pool, kActive), voice);
    }

    return voice;
}

int EadEarlyPlayer::AllocFromDecaying(int pool, int layer)
{
    const int voice = PopBack(Head(pool, kDecaying));
    if (voice != -1)
    {
        ReleaseAndTake(voice, layer);
        PushBack(Head(pool, kReleasing), voice);
    }

    return voice;
}

int EadEarlyPlayer::AllocFromActive(int pool, int layer_index)
{
    const Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    const int priority = channels_[static_cast<std::size_t>(layer.channel)].priority;
    const int voice = PopLowerPriority(Head(pool, kActive), priority);
    if (voice != -1)
    {
        DecayOrRelease(voices_[static_cast<std::size_t>(voice)].layer, AdsrPhase::kRelease);
        voices_[static_cast<std::size_t>(voice)].wanted_layer = layer_index;
        PushBack(Head(pool, kReleasing), voice);
    }

    return voice;
}

// Allocates a voice for a layer from the pools in order: a disabled voice first, then a decaying one, then an active
// one of lower priority. With no voice found, the layer is marked as not loaded.
int EadEarlyPlayer::AllocFromPools(int layer_index, std::initializer_list<int> pools)
{
    for (const int pool : pools)
    {
        const int voice = AllocFromDisabled(pool, layer_index);
        if (voice != -1)
        {
            return voice;
        }
    }

    for (const int pool : pools)
    {
        const int voice = AllocFromDecaying(pool, layer_index);
        if (voice != -1)
        {
            return voice;
        }
    }

    for (const int pool : pools)
    {
        const int voice = AllocFromActive(pool, layer_index);
        if (voice != -1)
        {
            return voice;
        }
    }

    layers_[static_cast<std::size_t>(layer_index)].loaded = false;

    return -1;
}

int EadEarlyPlayer::AllocVoice(int layer_index)
{
    const Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const uint8_t policy = channel.policy;
    const int channel_pool = ChannelPool(layer.channel);
    const int player_pool = PlayerPool(channel.player);
    if ((policy & kPolicyLayer) != 0)
    {
        const int voice = layer.voice;
        if (voice != -1 && voices_[static_cast<std::size_t>(voice)].previous_layer == layer_index)
        {
            ReleaseAndTake(voice, layer_index);
            Remove(voice);
            PushBack(Head(kGlobalPool, kReleasing), voice);

            return voice;
        }
    }

    if ((policy & kPolicyChannel) != 0)
    {
        return AllocFromPools(layer_index, {channel_pool});
    }

    if ((policy & kPolicySequence) != 0)
    {
        return AllocFromPools(layer_index, {channel_pool, player_pool});
    }

    if ((policy & kPolicyGlobal) != 0)
    {
        return AllocFromPools(layer_index, {kGlobalPool});
    }

    return AllocFromPools(layer_index, {channel_pool, player_pool, kGlobalPool});
}

void EadEarlyPlayer::ReclaimVoices()
{
    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        Voice& voice = voices_[i];
        if (voice.layer == -1)
        {
            continue;
        }

        const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
        bool release = false;
        if (!layer.enabled && voice.priority >= kPriorityMinimum)
        {
            release = true;
        }
        else if (layer.channel == -1)
        {
            DisableLayer(voice.layer);
            voice.priority = kPriorityStopping;
        }
        else if (channels_[static_cast<std::size_t>(layer.channel)].player == -1)
        {
            DisableChannel(layer.channel);
            voice.priority = kPriorityStopping;
        }
        else if (players_[static_cast<std::size_t>(channels_[static_cast<std::size_t>(layer.channel)].player)].muted)
        {
            const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
            if ((channel.mute_behavior & (kMuteStopScript | kMuteStopNotes)) != 0)
            {
                release = true;
            }
        }

        if (release)
        {
            DecayOrRelease(voice.layer, AdsrPhase::kRelease);
            Remove(static_cast<int>(i));
            PushFront(Head(voice.pool, kDisabled), static_cast<int>(i));
            voice.priority = kPriorityStopping;
        }
    }
}

void EadEarlyPlayer::ProcessVoices()
{
    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        Voice& voice = voices_[i];
        const int voice_index = static_cast<int>(i);
        if (voice.priority == kPriorityDisabled || !RetireVoice(voice_index))
        {
            continue;
        }

        UpdateAdsr(voice);
        UpdateVibrato(voice);
        float frequency = 0.0F;
        float velocity = 0.0F;
        float pan = 0.0F;
        uint8_t reverb = 0;
        if (voice.priority == kPriorityStopping)
        {
            frequency = voice.saved_frequency;
            velocity = voice.saved_velocity;
            pan = voice.saved_pan;
            reverb = voice.saved_reverb;
        }
        else
        {
            const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
            frequency = layer.note_frequency;
            velocity = layer.note_velocity;
            pan = layer.note_pan;
            reverb = channels_[static_cast<std::size_t>(layer.channel)].reverb;
        }

        float scale = static_cast<float>(voice.adsr_volume);
        frequency *= voice.vibrato_scale * voice.portamento_scale;
        if (config_.ai_frequency != 32006)
        {
            frequency *= 32000.0F / static_cast<float>(config_.ai_frequency);
        }

        frequency = frequency < 3.99992F ? frequency : 3.99992F;
        scale *= 4.3498e-5F;
        velocity = velocity * scale * scale;
        voice.frequency = frequency;
        SetVelocityPanReverb(voice, velocity, pan, reverb);
    }
}

// Disables a voice that has finished or faded out, or starts the note of the layer that waits for it. Disables a
// voice whose envelope has ended too. Returns false when the voice is now disabled.
bool EadEarlyPlayer::RetireVoice(int voice_index)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    if (voice.priority == kPriorityStopping || voice.finished)
    {
        if (voice.adsr_volume != 0 && !voice.finished)
        {
            return true;
        }

        if (voice.wanted_layer == -1)
        {
            DisableVoice(voice);
            Remove(voice_index);
            PushBack(Head(voice.pool, kDisabled), voice_index);

            return false;
        }

        DisableVoice(voice);
        if (layers_[static_cast<std::size_t>(voice.wanted_layer)].channel == -1)
        {
            DisableVoice(voice);
            Remove(voice_index);
            PushBack(Head(voice.pool, kDisabled), voice_index);
            voice.wanted_layer = -1;

            return false;
        }

        InitVoiceForLayer(voice_index, voice.wanted_layer);
        InitVibrato(voice_index);
        Remove(voice_index);
        PushBack(Head(voice.pool, kActive), voice_index);
        voice.wanted_layer = -1;

        return true;
    }

    if (voice.adsr.phase == AdsrPhase::kDisabled)
    {
        DisableVoice(voice);
        Remove(voice_index);
        PushBack(Head(voice.pool, kDisabled), voice_index);

        return false;
    }

    return true;
}

void EadEarlyPlayer::SetVelocityPanReverb(Voice& voice, float velocity, float pan, uint8_t reverb)
{
    const int pan_index = static_cast<int32_t>(pan * 127.5F) & 127;
    const auto index = static_cast<std::size_t>(pan_index);
    float left = 0.0F;
    float right = 0.0F;
    if (voice.stereo_effects && config_.sound_mode == EadSoundMode::kHeadset)
    {
        auto small_pan = static_cast<int8_t>(pan * 10.0F);
        if (small_pan >= 9)
        {
            small_pan = 9;
        }

        voice.headset_left = tables::kHeadsetPanDelay[static_cast<std::size_t>(small_pan)];
        voice.headset_right = tables::kHeadsetPanDelay[static_cast<std::size_t>(9 - small_pan)];
        voice.strong_right = false;
        voice.strong_left = false;
        voice.headset_effects = true;
        left = tables::kHeadsetPanVolume[index];
        right = tables::kHeadsetPanVolume[127 - index];
    }
    else if (voice.stereo_effects && config_.sound_mode == EadSoundMode::kStereo)
    {
        voice.headset_left = 0;
        voice.headset_right = 0;
        voice.headset_effects = false;
        left = tables::kStereoPanVolume[index];
        right = tables::kStereoPanVolume[127 - index];
        voice.strong_right = pan_index > 0x60;
        voice.strong_left = pan_index < 0x20;
    }
    else if (config_.sound_mode == EadSoundMode::kMono)
    {
        left = 0.707F;
        right = 0.707F;
    }
    else
    {
        left = tables::kDefaultPanVolume[index];
        right = tables::kDefaultPanVolume[127 - index];
    }

    if (velocity < 0.0F)
    {
        velocity = 0.0F;
    }

    voice.target_left = static_cast<uint16_t>(static_cast<uint16_t>(static_cast<int32_t>(velocity * left)) & ~0x80FF);
    voice.target_right = static_cast<uint16_t>(static_cast<uint16_t>(static_cast<int32_t>(velocity * right)) & ~0x80FF);
    if (voice.target_left == 0)
    {
        voice.target_left++;
    }

    if (voice.target_right == 0)
    {
        voice.target_right++;
    }

    if (voice.reverb != reverb)
    {
        voice.reverb = reverb;
        voice.reverb_shifted = static_cast<int16_t>(reverb << 8);
        voice.mixer_needs_init = true;
        return;
    }

    voice.mixer_needs_init = voice.needs_init;
}

} // namespace ntdlib::n64

// SPDX-License-Identifier: MIT

// The voices in the model of the middle EAD engine: the voice lists, the allocation of voices to layers, the envelopes
// and vibratos, and the processing of the voices at each update.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ead_later_tables.h"
#include "ead_lists.h"
#include "ead_middle_constants.h"
#include "ntdlib/n64/ead_middle.h"

namespace ntdlib::n64
{

namespace
{

using ead_lists::WriteBe16;
using ead_middle_constants::kMuteSoften;
using ead_middle_constants::kMuteStopNotes;
using ead_middle_constants::kMuteStopScript;
using ead_middle_constants::kPriorityDisabled;
using ead_middle_constants::kPriorityMin;
using ead_middle_constants::kPriorityStopping;
using ead_middle_constants::kWaveSamples;

namespace later_tables = ead_later_tables;

constexpr uint8_t kPolicyLayer = 1;
constexpr uint8_t kPolicyChannel = 2;
constexpr uint8_t kPolicyPlayer = 4;
constexpr uint8_t kPolicyGlobal = 8;

// An envelope's states.
constexpr uint8_t kAdsrDisabled = 0;
constexpr uint8_t kAdsrInitial = 1;
constexpr uint8_t kAdsrStartLoop = 2;
constexpr uint8_t kAdsrLoop = 3;
constexpr uint8_t kAdsrFade = 4;
constexpr uint8_t kAdsrHang = 5;
constexpr uint8_t kAdsrDecay = 6;
constexpr uint8_t kAdsrRelease = 7;
constexpr uint8_t kAdsrSustain = 8;

constexpr uint8_t kActionDecay = 0x10;
constexpr uint8_t kActionRelease = 0x20;
constexpr uint8_t kActionHang = 0x40;

constexpr std::array<EadEnvelope::Point, 3> kDefaultEnvelope = {{{4, 32000}, {1000, 32000}, {kEnvelopeHang, 0}}};

// The delay in samples of the far side in headset mode. Offset 0xF6E70 of the American image of Mario Kart 64.
constexpr std::array<uint16_t, 16> kHeadsetPanDelay = {0x40, 0x40, 0x30, 0x30, 0x20, 0x20, 0x10, 0,
                                                       0,    0,    0,    0,    0,    0,    0,    0};

} // namespace

int EadMiddlePlayer::Head(int pool, ListKind kind) const
{
    return config_.voices + pool * 4 + kind;
}

int EadMiddlePlayer::PlayerPool(int player) const
{
    return 1 + player;
}

int EadMiddlePlayer::ChannelPool(int channel) const
{
    return 1 + kPlayers + channel;
}

void EadMiddlePlayer::PushBack(int head, int voice)
{
    const auto place = static_cast<std::size_t>(voice);
    if (previous_[place] != -1)
    {
        return;
    }

    const int last = previous_[static_cast<std::size_t>(head)];
    next_[static_cast<std::size_t>(last)] = voice;
    previous_[place] = last;
    next_[place] = head;
    previous_[static_cast<std::size_t>(head)] = voice;
    voices_[place].pool = (head - config_.voices) / 4;
}

void EadMiddlePlayer::PushFront(int head, int voice)
{
    const auto place = static_cast<std::size_t>(voice);
    if (previous_[place] != -1)
    {
        return;
    }

    const int first = next_[static_cast<std::size_t>(head)];
    previous_[place] = head;
    next_[place] = first;
    previous_[static_cast<std::size_t>(first)] = voice;
    next_[static_cast<std::size_t>(head)] = voice;
    voices_[place].pool = (head - config_.voices) / 4;
}

int EadMiddlePlayer::PopBack(int head)
{
    const int last = previous_[static_cast<std::size_t>(head)];
    if (last == head)
    {
        return kNoVoice;
    }

    Remove(last);

    return last;
}

void EadMiddlePlayer::Remove(int voice)
{
    const auto place = static_cast<std::size_t>(voice);
    if (previous_[place] == -1)
    {
        return;
    }

    next_[static_cast<std::size_t>(previous_[place])] = next_[place];
    previous_[static_cast<std::size_t>(next_[place])] = previous_[place];
    previous_[place] = -1;
}

void EadMiddlePlayer::ClearPool(int pool)
{
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        const int source = Head(pool, kind);
        const int target = Head(0, kind);
        while (next_[static_cast<std::size_t>(source)] != source)
        {
            const int voice = next_[static_cast<std::size_t>(source)];
            Remove(voice);
            PushBack(target, voice);
        }
    }
}

void EadMiddlePlayer::FillPool(int pool, int count)
{
    ClearPool(pool);
    int moved = 0;
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        while (moved < count)
        {
            const int voice = PopBack(Head(0, kind));
            if (voice == kNoVoice)
            {
                break;
            }

            PushBack(Head(pool, kind), voice);
            moved++;
        }
    }
}

int EadMiddlePlayer::PopLowerPriority(int head, int limit)
{
    int item = next_[static_cast<std::size_t>(head)];
    if (item == head)
    {
        return kNoVoice;
    }

    int best = item;
    for (; item != head; item = next_[static_cast<std::size_t>(item)])
    {
        if (voices_[static_cast<std::size_t>(best)].priority >= voices_[static_cast<std::size_t>(item)].priority)
        {
            best = item;
        }
    }

    if (limit <= voices_[static_cast<std::size_t>(best)].priority)
    {
        return kNoVoice;
    }

    Remove(best);

    return best;
}

EadEnvelope::Point EadMiddlePlayer::EnvelopePoint(const EnvelopeRef& envelope, int index) const
{
    const auto place = static_cast<std::size_t>(index);
    if (envelope.bank != nullptr)
    {
        return place < envelope.bank->points.size() ? envelope.bank->points[place]
                                                    : EadEnvelope::Point{kEnvelopeDisable, 0};
    }

    if (envelope.player >= 0)
    {
        const Player& player = players_[static_cast<std::size_t>(envelope.player)];
        const uint32_t at = envelope.offset + static_cast<uint32_t>(index) * 4;

        return {static_cast<int16_t>(Peek16(player, at)), static_cast<int16_t>(Peek16(player, at + 2))};
    }

    return place < kDefaultEnvelope.size() ? kDefaultEnvelope[place] : EadEnvelope::Point{kEnvelopeDisable, 0};
}

void EadMiddlePlayer::PlayerSound(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (player.fade_updates != 0)
    {
        player.fade_volume = std::clamp(player.fade_volume + player.fade_step, 0.0F, 1.0F);
        player.recalculate_volume = true;
        player.fade_updates--;
        if (player.fade_updates == 0 && player.state == 2)
        {
            DisablePlayer(index);

            return;
        }
    }

    if (player.recalculate_volume)
    {
        player.applied_fade = player.fade_volume * player.fade_scale;
    }

    for (const int channel : player.channels)
    {
        if (channel != kNoChannel && channels_[static_cast<std::size_t>(channel)].enabled)
        {
            ChannelSound(channel, player.recalculate_volume);
        }
    }

    player.recalculate_volume = false;
}

void EadMiddlePlayer::ChannelSound(int index, bool recalculate_volume)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    if (channel.change_volume || recalculate_volume)
    {
        float volume = channel.volume * channel.volume_scale * player.applied_fade;
        if (player.muted && (channel.mute_behavior & kMuteSoften) != 0)
        {
            volume = player.mute_scale * volume;
        }

        channel.applied_volume = config_.standard_microcode ? volume : volume * volume;
    }

    if (channel.change_pan)
    {
        channel.pan = channel.new_pan * channel.pan_weight;
    }

    for (const int layer_index : channel.layers)
    {
        if (layer_index == kNoLayer)
        {
            continue;
        }

        Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
        if (!layer.enabled || layer.voice == kNoVoice)
        {
            continue;
        }

        const auto note_pan = static_cast<uint8_t>((channel.pan + layer.pan * (0x80 - channel.pan_weight)) >> 7);
        if (layer.properties_need_init)
        {
            layer.note_frequency = layer.frequency_scale * channel.frequency_scale;
            layer.note_velocity = layer.velocity_square * channel.applied_volume;
            layer.note_pan = note_pan;
            layer.properties_need_init = false;
        }
        else
        {
            if (channel.change_frequency)
            {
                layer.note_frequency = layer.frequency_scale * channel.frequency_scale;
            }

            if (channel.change_volume || recalculate_volume)
            {
                layer.note_velocity = layer.velocity_square * channel.applied_volume;
            }

            if (channel.change_pan)
            {
                layer.note_pan = note_pan;
            }
        }
    }

    channel.change_frequency = false;
    channel.change_volume = false;
    channel.change_pan = false;
}

float EadMiddlePlayer::UpdateAdsr(Adsr& adsr)
{
    const uint8_t action = adsr.action;
    const uint8_t state = adsr.state;
    if (state == kAdsrDisabled)
    {
        return 0.0F;
    }

    if (state == kAdsrInitial && (action & kActionHang) != 0)
    {
        adsr.state = kAdsrHang;
    }
    else if (state == kAdsrInitial || state == kAdsrStartLoop || state == kAdsrLoop || state == kAdsrFade)
    {
        FollowEnvelope(adsr, state);
    }
    else if (state == kAdsrDecay || state == kAdsrRelease)
    {
        adsr.current -= adsr.fade_step;
        if (adsr.sustain != 0.0F && state == kAdsrDecay)
        {
            if (adsr.current < adsr.sustain)
            {
                adsr.current = adsr.sustain;
                adsr.delay = 128;
                adsr.state = kAdsrSustain;
            }
        }
        else if (adsr.current < (config_.standard_microcode ? 0.0F : 0.00001F))
        {
            adsr.current = 0.0F;
            adsr.state = kAdsrDisabled;
        }
    }
    else if (state == kAdsrSustain)
    {
        adsr.delay--;
        if (adsr.delay == 0)
        {
            adsr.state = kAdsrRelease;
        }
    }

    if ((action & kActionDecay) != 0)
    {
        adsr.state = kAdsrDecay;
        adsr.action = static_cast<uint8_t>(action & ~kActionDecay);
    }

    if ((action & kActionRelease) != 0)
    {
        adsr.state = kAdsrRelease;
        adsr.action = static_cast<uint8_t>(action & ~kActionRelease);
    }

    if (adsr.current < 0.0F)
    {
        return 0.0F;
    }

    return adsr.current > 1.0F ? 1.0F : adsr.current;
}

// Steps an envelope that follows its list of points. `state` is the state at the start of the update. Reads the next
// point unless a fade is running. Then steps the fade.
void EadMiddlePlayer::FollowEnvelope(Adsr& adsr, uint8_t state) const
{
    if (state == kAdsrInitial || state == kAdsrStartLoop)
    {
        adsr.index = 0;
        adsr.state = kAdsrLoop;
    }

    if (state != kAdsrFade)
    {
        ReadEnvelopePoint(adsr);
    }

    if (adsr.state == kAdsrFade)
    {
        adsr.current += adsr.velocity;
        adsr.delay--;
        if (adsr.delay <= 0)
        {
            adsr.state = kAdsrLoop;
        }
    }
}

// Reads the envelope point at adsr.index and sets the next state from it. Follows the jumps in the list.
void EadMiddlePlayer::ReadEnvelopePoint(Adsr& adsr) const
{
    // A list that only jumps would hang the console. The model ends it.
    int jumps = 0;
    while (true)
    {
        const EadEnvelope::Point point = EnvelopePoint(adsr.envelope, adsr.index);
        adsr.delay = point.delay;
        if (adsr.delay == kEnvelopeDisable)
        {
            adsr.state = kAdsrDisabled;
        }
        else if (adsr.delay == kEnvelopeHang)
        {
            adsr.state = kAdsrHang;
        }
        else if (adsr.delay == kEnvelopeGoto)
        {
            adsr.index = static_cast<uint8_t>(point.target);
            if (config_.standard_microcode)
            {
                // The jump takes this update. The list's next point waits for the next one.
                break;
            }

            jumps++;
            if (jumps < 256)
            {
                continue;
            }

            adsr.state = kAdsrDisabled;
        }
        else if (adsr.delay == kEnvelopeRestart)
        {
            adsr.state = kAdsrInitial;
        }
        else
        {
            if (adsr.delay >= 4)
            {
                adsr.delay = static_cast<int16_t>(adsr.delay * updates_per_frame_ / config_.frames_per_list / 4);
            }

            if (adsr.delay == 0 && !config_.standard_microcode)
            {
                adsr.delay = 1;
            }

            adsr.target = config_.standard_microcode ? static_cast<float>(static_cast<double>(point.target) / 32767.0)
                                                     : static_cast<float>(point.target) / 32767.0F;
            adsr.target = adsr.target * adsr.target;
            adsr.velocity = (adsr.target - adsr.current) / static_cast<float>(adsr.delay);
            adsr.state = kAdsrFade;
            adsr.index++;
        }

        break;
    }
}

float EadMiddlePlayer::VibratoScale(Vibrato& vibrato)
{
    if (vibrato.delay != 0)
    {
        vibrato.delay--;

        return 1.0F;
    }

    const Channel& channel = channels_[static_cast<std::size_t>(vibrato.channel)];
    const int32_t depth_target = channel.vibrato_depth_target;
    const int32_t rate_target = channel.vibrato_rate_target;
    if (vibrato.depth_timer != 0)
    {
        if (vibrato.depth_timer == 1)
        {
            vibrato.depth = static_cast<float>(depth_target);
        }
        else
        {
            vibrato.depth +=
                (static_cast<float>(depth_target) - vibrato.depth) / static_cast<float>(int32_t{vibrato.depth_timer});
        }

        vibrato.depth_timer--;
    }
    else if (depth_target != static_cast<int32_t>(vibrato.depth))
    {
        vibrato.depth_timer = channel.vibrato_depth_delay;
        if (vibrato.depth_timer == 0)
        {
            vibrato.depth = static_cast<float>(depth_target);
        }
    }

    if (vibrato.rate_timer != 0)
    {
        if (vibrato.rate_timer == 1)
        {
            vibrato.rate = static_cast<float>(rate_target);
        }
        else
        {
            vibrato.rate +=
                (static_cast<float>(rate_target) - vibrato.rate) / static_cast<float>(int32_t{vibrato.rate_timer});
        }

        vibrato.rate_timer--;
    }
    else if (rate_target != static_cast<int32_t>(vibrato.rate))
    {
        vibrato.rate_timer = channel.vibrato_rate_delay;
        if (vibrato.rate_timer == 0)
        {
            vibrato.rate = static_cast<float>(rate_target);
        }
    }

    if (vibrato.depth == 0.0F)
    {
        return 1.0F;
    }

    vibrato.time += static_cast<uint32_t>(static_cast<int32_t>(vibrato.rate));
    const std::size_t place = (vibrato.time >> 10) & 0x3F;
    const int change = later_tables::kWaves[2 * 256 + place] >> 8;
    const float depth = vibrato.depth / 4096.0F;

    return 1.0F + depth * (later_tables::kBendOctave[static_cast<std::size_t>(change + 128)] - 1.0F);
}

void EadMiddlePlayer::UpdateVibrato(Voice& voice)
{
    if (voice.portamento.mode != 0)
    {
        voice.portamento.position += voice.portamento.speed;
        const float position = voice.portamento.position;
        const uint32_t place = position >= 127.0F ? 127U : static_cast<uint32_t>(position);
        voice.portamento_scale = 1.0F + voice.portamento.extent * (later_tables::kBendOctave[place + 128] - 1.0F);
    }

    if (voice.vibrato.active && voice.layer != kNoLayer)
    {
        voice.vibrato_scale = VibratoScale(voice.vibrato);
    }
}

void EadMiddlePlayer::InitVibrato(int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    voice.vibrato_scale = 1.0F;
    voice.portamento_scale = 1.0F;
    const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Vibrato& vibrato = voice.vibrato;
    vibrato.active = true;
    vibrato.time = 0;
    vibrato.channel = layer.channel;
    vibrato.depth_timer = channel.vibrato_depth_delay;
    vibrato.depth = static_cast<float>(
        int32_t{vibrato.depth_timer == 0 ? channel.vibrato_depth_target : channel.vibrato_depth_start});
    vibrato.rate_timer = channel.vibrato_rate_delay;
    vibrato.rate =
        static_cast<float>(int32_t{vibrato.rate_timer == 0 ? channel.vibrato_rate_target : channel.vibrato_rate_start});
    vibrato.delay = channel.vibrato_delay;
    voice.portamento = layer.portamento;
}

void EadMiddlePlayer::DecayOrRelease(int index, bool release)
{
    if (index == kNoLayer)
    {
        return;
    }

    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (layer.voice == kNoVoice)
    {
        return;
    }

    const int voice_index = layer.voice;
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    if (voice.wanted_layer == index)
    {
        voice.wanted_layer = kNoLayer;
    }

    if (voice.layer != index)
    {
        if (voice.layer == kNoLayer && voice.wanted_layer == kNoLayer && voice.previous_layer == index && release)
        {
            voice.adsr.fade_step = updates_inverse_;
            voice.adsr.action |= kActionRelease;
        }

        return;
    }

    layer.loaded = false;
    if (voice.adsr.state != kAdsrDecay)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
        voice.attributes.frequency_scale = layer.note_frequency;
        voice.attributes.velocity = layer.note_velocity;
        voice.attributes.pan = layer.note_pan;
        voice.attributes.reverb = channel.reverb;
        voice.priority = kPriorityStopping;
        voice.previous_layer = voice.layer;
        voice.layer = kNoLayer;
        if (release)
        {
            voice.adsr.fade_step = updates_inverse_;
            voice.adsr.action |= kActionRelease;
        }
        else
        {
            voice.adsr.action |= kActionDecay;
            const uint8_t rate = layer.adsr.release == 0 ? channel.adsr.release : layer.adsr.release;
            voice.adsr.fade_step = static_cast<float>(rate) * updates_inverse_scaled_;
            voice.adsr.sustain = (static_cast<float>(channel.adsr.sustain) * voice.adsr.current) / 256.0F;
        }
    }

    if (!release)
    {
        Remove(voice_index);
        PushFront(Head(voice.pool, kDecaying), voice_index);
    }
}

void EadMiddlePlayer::InitVoice(Voice& voice)
{
    const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
    voice.adsr.action = 0;
    voice.adsr.delay = 0;
    voice.adsr.sustain = 0.0F;
    voice.adsr.current = 0.0F;
    voice.adsr.envelope = layer.adsr.release == 0 ? channels_[static_cast<std::size_t>(layer.channel)].adsr.envelope
                                                  : layer.adsr.envelope;
    voice.adsr.state = kAdsrInitial;
    voice.state = UpdateState{};
    voice.state.enabled = true;
    voice.state.needs_init = true;
}

void EadMiddlePlayer::DisableVoice(Voice& voice)
{
    if (voice.state.needs_init)
    {
        voice.state.needs_init = false;
    }
    else
    {
        SetVolumes(voice, 0.0F, 0x40, 0);
    }

    voice.priority = kPriorityDisabled;
    voice.layer = kNoLayer;
    voice.previous_layer = kNoLayer;
    voice.state.enabled = false;
    voice.state.finished = false;
}

void EadMiddlePlayer::SetVolumes(Voice& voice, float velocity, uint8_t pan, uint8_t reverb)
{
    UpdateState& state = voice.state;
    pan &= 0x7F;
    float left = 0.0F;
    float right = 0.0F;
    if (state.stereo_effects && config_.sound_mode == EadSoundMode::kHeadset)
    {
        const std::size_t small_pan = pan >> 3;
        state.delay_left = static_cast<uint8_t>(kHeadsetPanDelay[small_pan]);
        state.delay_right = static_cast<uint8_t>(kHeadsetPanDelay[15 - small_pan]);
        state.strong_right = false;
        state.strong_left = false;
        state.headset_effects = true;
        left = later_tables::kHeadsetPanVolume[pan];
        right = later_tables::kHeadsetPanVolume[127U - pan];
    }
    else if (state.stereo_effects && config_.sound_mode == EadSoundMode::kStereo)
    {
        state.delay_right = 0;
        state.delay_left = 0;
        state.headset_effects = false;
        left = later_tables::kStereoPanVolume[pan];
        right = later_tables::kStereoPanVolume[127U - pan];
        state.strong_left = pan < 0x20;
        state.strong_right = pan > 0x60;
    }
    else if (config_.sound_mode == EadSoundMode::kMono)
    {
        left = 0.707F;
        right = 0.707F;
    }
    else
    {
        left = later_tables::kDefaultPanVolume[pan];
        right = later_tables::kDefaultPanVolume[127U - pan];
    }

    if (config_.standard_microcode)
    {
        velocity = std::min(std::max(velocity, 0.0F), 32767.0F);
        state.target_left = static_cast<uint16_t>((static_cast<int32_t>(velocity * left) & 0xFFFF) >> 5);
        state.target_right = static_cast<uint16_t>((static_cast<int32_t>(velocity * right) & 0xFFFF) >> 5);
    }
    else
    {
        velocity = std::min(std::max(velocity, 0.0F), 1.0F);
        state.target_left = static_cast<uint16_t>(static_cast<int32_t>(velocity * left * 4095.999F));
        state.target_right = static_cast<uint16_t>(static_cast<int32_t>(velocity * right * 4095.999F));
    }

    state.mixer_needs_init = state.needs_init || state.reverb != reverb;
    state.reverb = reverb;
}

void EadMiddlePlayer::SetPitch(Voice& voice, float frequency)
{
    constexpr float kHighest = 1.9999599F;
    float rate = 0.0F;
    frequency = std::max(frequency, 0.0F);
    if (frequency < 2.0F)
    {
        voice.state.two_parts = false;
        rate = kHighest < frequency ? kHighest : frequency;
    }
    else
    {
        voice.state.two_parts = true;
        rate = 2.0F * kHighest < frequency ? kHighest : frequency * 0.5F;
    }

    voice.state.pitch = static_cast<uint16_t>(static_cast<int32_t>(rate * 32768.0F));
}

int EadMiddlePlayer::BuildWave(Voice& voice, Layer& layer, int wave)
{
    if (wave < 128)
    {
        wave = 128;
    }

    float frequency = layer.frequency_scale;
    if (layer.portamento.mode != 0 && 0.0F < layer.portamento.extent)
    {
        frequency *= layer.portamento.extent + 1.0F;
    }

    int harmonic = 3;
    float ratio = 0.13081F;
    if (frequency < 1.0F)
    {
        harmonic = 0;
        ratio = 1.0465F;
    }
    else if (frequency < 2.0F)
    {
        harmonic = 1;
        ratio = 0.52325F;
    }
    else if (frequency < 4.0F)
    {
        harmonic = 2;
        ratio = 0.26263F;
    }

    layer.frequency_scale *= ratio;
    voice.harmonic = static_cast<uint8_t>(harmonic);
    voice.state.wave_address =
        waves_address_ + static_cast<uint32_t>(wave - 128) * 0x200 + static_cast<uint32_t>(harmonic * kWaveSamples) * 2;

    return harmonic;
}

void EadMiddlePlayer::InitVoiceForLayer(int index, int layer_index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    voice.previous_layer = kNoLayer;
    voice.layer = layer_index;
    voice.priority = channel.priority;
    layer.properties_need_init = true;
    layer.loaded = true;
    layer.voice = index;
    layer.note_velocity = 0.0F;
    InitVoice(voice);
    const int program = layer.program == 0xFF ? channel.program : layer.program;
    voice.state.sound = layer.sound;
    voice.state.synthetic_wave = program >= 0x80;
    if (voice.state.synthetic_wave)
    {
        BuildWave(voice, layer, program);
    }

    voice.state.sound_bank = channel.bank;
    voice.state.stereo_effects = channel.stereo_effects;
    voice.state.reverb_index = channel.reverb_index & 3;
}

void EadMiddlePlayer::TakeAfterRelease(int index, int layer)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    voice.wanted_layer = layer;
    voice.priority = kPriorityStopping;
    voice.adsr.fade_step = updates_inverse_;
    voice.adsr.action |= kActionRelease;
}

int EadMiddlePlayer::AllocFromDisabled(int pool, int layer)
{
    const int voice = PopBack(Head(pool, kDisabled));
    if (voice != kNoVoice)
    {
        InitVoiceForLayer(voice, layer);
        PushFront(Head(pool, kActive), voice);
    }

    return voice;
}

int EadMiddlePlayer::AllocFromDecaying(int pool, int layer)
{
    const int voice = PopBack(Head(pool, kDecaying));
    if (voice != kNoVoice)
    {
        TakeAfterRelease(voice, layer);
        PushBack(Head(pool, kReleasing), voice);
    }

    return voice;
}

int EadMiddlePlayer::AllocFromActive(int pool, int layer_index)
{
    const Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    const int voice =
        PopLowerPriority(Head(pool, kActive), channels_[static_cast<std::size_t>(layer.channel)].priority);
    if (voice != kNoVoice)
    {
        DecayOrRelease(voices_[static_cast<std::size_t>(voice)].layer, true);
        voices_[static_cast<std::size_t>(voice)].wanted_layer = layer_index;
        PushBack(Head(pool, kReleasing), voice);
    }

    return voice;
}

int EadMiddlePlayer::AllocVoice(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const uint8_t policy = channel.policy;
    const int channel_pool = ChannelPool(layer.channel);
    const int player_pool = PlayerPool(channel.player);
    if ((policy & kPolicyLayer) != 0)
    {
        const int voice = layer.voice;
        if (voice != kNoVoice && voices_[static_cast<std::size_t>(voice)].previous_layer == index &&
            voices_[static_cast<std::size_t>(voice)].wanted_layer == kNoLayer)
        {
            TakeAfterRelease(voice, index);
            Remove(voice);
            PushBack(Head(voices_[static_cast<std::size_t>(voice)].pool, kReleasing), voice);

            return voice;
        }
    }

    // The order in which the policy's pools give up a voice: each list of each pool, from the least useful voices.
    std::array<int, 3> pools = {channel_pool, player_pool, 0};
    std::size_t pool_count = 3;
    if ((policy & kPolicyChannel) != 0)
    {
        pool_count = 1;
    }
    else if ((policy & kPolicyPlayer) != 0)
    {
        pool_count = 2;
    }
    else if ((policy & kPolicyGlobal) != 0)
    {
        pools[0] = 0;
        pool_count = 1;
    }

    const std::span<const int> order(pools.data(), pool_count);
    for (const int pool : order)
    {
        if (const int voice = AllocFromDisabled(pool, index); voice != kNoVoice)
        {
            return voice;
        }
    }

    for (const int pool : order)
    {
        if (const int voice = AllocFromDecaying(pool, index); voice != kNoVoice)
        {
            return voice;
        }
    }

    for (const int pool : order)
    {
        if (const int voice = AllocFromActive(pool, index); voice != kNoVoice)
        {
            return voice;
        }
    }

    layer.loaded = false;

    return kNoVoice;
}

void EadMiddlePlayer::ProcessVoices()
{
    for (int i = 0; i < config_.voices; i++)
    {
        ProcessVoice(i);
    }
}

// Processes a voice for an update: stops it with its layer, ends or reuses it, and sets its pitch and volumes.
void EadMiddlePlayer::ProcessVoice(int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    if (voice.layer != kNoLayer)
    {
        if (!StopWithLayer(index))
        {
            return;
        }
    }
    else if (voice.priority >= kPriorityMin)
    {
        return;
    }

    if (voice.priority == kPriorityDisabled)
    {
        return;
    }

    if (!EndOrReuseVoice(index))
    {
        return;
    }

    const float scale = UpdateAdsr(voice.adsr);
    UpdateVibrato(voice);
    float frequency = 0.0F;
    float velocity = 0.0F;
    uint8_t pan = 0;
    uint8_t reverb = 0;
    uint8_t book_offset = 0;
    if (voice.priority == kPriorityStopping)
    {
        frequency = voice.attributes.frequency_scale;
        velocity = voice.attributes.velocity;
        pan = voice.attributes.pan;
        reverb = voice.attributes.reverb;
        book_offset = voice.state.book_offset;
    }
    else
    {
        const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
        const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
        frequency = layer.note_frequency;
        velocity = layer.note_velocity;
        pan = layer.note_pan;
        reverb = channel.reverb;
        book_offset = channel.book_offset & 7;
    }

    frequency *= voice.vibrato_scale * voice.portamento_scale;
    velocity = config_.standard_microcode ? velocity * scale * scale : velocity * scale;
    SetPitch(voice, frequency);
    SetVolumes(voice, velocity, pan, reverb);
    voice.state.book_offset = book_offset;
}

// Stops a voice whose layer has ended or whose player mutes its notes. Returns false when the channel of the layer has
// no player. The channel is then disabled and the voice skips the update.
bool EadMiddlePlayer::StopWithLayer(int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    const int layer_index = voice.layer;
    const Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    bool stop = false;
    if (!layer.enabled && voice.priority >= kPriorityMin)
    {
        stop = true;
    }
    else
    {
        const int player = channels_[static_cast<std::size_t>(layer.channel)].player;
        if (player == -1)
        {
            DisableChannel(layer.channel);
            voice.priority = kPriorityStopping;

            return false;
        }

        stop = players_[static_cast<std::size_t>(player)].muted &&
               (channels_[static_cast<std::size_t>(layer.channel)].mute_behavior &
                (kMuteStopScript | kMuteStopNotes)) != 0;
    }

    if (stop)
    {
        DecayOrRelease(layer_index, true);
        Remove(index);
        PushFront(Head(voice.pool, kDecaying), index);
        voice.priority = kPriorityStopping;
    }

    return true;
}

// Disables a voice whose envelope or sample has ended. A stopping voice then starts the layer that waits for it.
// Returns whether the voice plays in the update.
bool EadMiddlePlayer::EndOrReuseVoice(int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    const UpdateState& state = voice.state;
    if (voice.priority == kPriorityStopping || state.finished)
    {
        if (voice.adsr.state == kAdsrDisabled || state.finished)
        {
            const int wanted = voice.wanted_layer;
            DisableVoice(voice);
            Remove(index);
            voice.wanted_layer = kNoLayer;
            if (wanted == kNoLayer || layers_[static_cast<std::size_t>(wanted)].channel == kNoChannel)
            {
                if (wanted != kNoLayer)
                {
                    DisableVoice(voice);
                }

                PushBack(Head(voice.pool, kDisabled), index);

                return false;
            }

            InitVoiceForLayer(index, wanted);
            InitVibrato(index);
            PushBack(Head(voice.pool, kActive), index);
        }
    }
    else if (voice.adsr.state == kAdsrDisabled)
    {
        DisableVoice(voice);
        Remove(index);
        PushBack(Head(voice.pool, kDisabled), index);

        return false;
    }

    return true;
}

void EadMiddlePlayer::EnsureBank(AudioMemory& memory, int bank)
{
    std::vector<uint32_t>& books = book_addresses_[static_cast<std::size_t>(bank)];
    const EadBank& held = files_->banks[static_cast<std::size_t>(bank)];
    if (!books.empty() || held.books.empty())
    {
        return;
    }

    books.reserve(held.books.size());
    for (const BankBook& book : held.books)
    {
        const uint32_t address = memory.Allocate(book.book.coefficients.size() * 2);
        for (std::size_t i = 0; i < book.book.coefficients.size(); i++)
        {
            WriteBe16(memory, address + static_cast<uint32_t>(i) * 2, book.book.coefficients[i]);
        }

        books.push_back(address);
    }

    std::vector<uint32_t>& loops = loop_addresses_[static_cast<std::size_t>(bank)];
    loops.reserve(held.loops.size());
    for (const BankLoop& loop : held.loops)
    {
        const uint32_t address = memory.Allocate(32);
        for (std::size_t i = 0; i < loop.state.size(); i++)
        {
            WriteBe16(memory, address + static_cast<uint32_t>(i) * 2, loop.state[i]);
        }

        loops.push_back(address);
    }
}

} // namespace ntdlib::n64

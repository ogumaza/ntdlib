// SPDX-License-Identifier: MIT

// The voices in the model of the later EAD engine: the voice lists, the allocation of voices to layers, the envelopes
// and vibratos, and the processing of the voices at each update.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "ead_later_constants.h"
#include "ead_later_tables.h"
#include "ead_lists.h"
#include "ntdlib/n64/ead_later.h"

namespace ntdlib::n64
{

namespace
{

using ead_later_constants::kMuteSoften;
using ead_later_constants::kMuteStopNotes;
using ead_later_constants::kMuteStopVoices;
using ead_later_constants::kWaveSamples;
using ead_lists::WriteBe16;

namespace tables = ead_later_tables;

// A layer's flags in Majora's Mask, all set at the start. A script can clear them.
constexpr uint16_t kLayerUsesChannelReverb = 0x2000;
constexpr uint16_t kLayerFlagVolume = 0x8000;
constexpr uint16_t kLayerFlagFrequency = 0x4000;
constexpr uint16_t kLayerFlagPan = 0x0080;
constexpr uint16_t kLayerUsesChannelVibrato = 0x1000;
constexpr uint16_t kLayerUsesChannelGain = 0x0040;

constexpr int32_t kLayerVibratoRate = 0x800;

constexpr std::array<EadEnvelope::Point, 4> kDefaultEnvelope = {
    {{1, 32000}, {1000, 32000}, {kEnvelopeHang, 0}, {kEnvelopeDisable, 0}}};

} // namespace

int EadLaterPlayer::Head(int pool, ListKind kind) const
{
    return static_cast<int>(voices_.size()) + pool * 4 + kind;
}

int EadLaterPlayer::PlayerPool(int player) const
{
    return 1 + player;
}

int EadLaterPlayer::ChannelPool(int channel) const
{
    return 1 + kPlayers + channel;
}

void EadLaterPlayer::PushBack(int head, int voice)
{
    const auto node = static_cast<std::size_t>(voice);
    if (previous_[node] != -1)
    {
        return;
    }

    const int tail = previous_[static_cast<std::size_t>(head)];
    next_[static_cast<std::size_t>(tail)] = voice;
    previous_[node] = tail;
    next_[node] = head;
    previous_[static_cast<std::size_t>(head)] = voice;
    voices_[node].pool = (head - static_cast<int>(voices_.size())) / 4;
}

void EadLaterPlayer::PushFront(int head, int voice)
{
    const auto node = static_cast<std::size_t>(voice);
    if (previous_[node] != -1)
    {
        return;
    }

    const int first = next_[static_cast<std::size_t>(head)];
    previous_[node] = head;
    next_[node] = first;
    previous_[static_cast<std::size_t>(first)] = voice;
    next_[static_cast<std::size_t>(head)] = voice;
    voices_[node].pool = (head - static_cast<int>(voices_.size())) / 4;
}

int EadLaterPlayer::PopBack(int head)
{
    const int item = previous_[static_cast<std::size_t>(head)];
    if (item == head)
    {
        return -1;
    }

    const int before = previous_[static_cast<std::size_t>(item)];
    next_[static_cast<std::size_t>(before)] = head;
    previous_[static_cast<std::size_t>(head)] = before;
    previous_[static_cast<std::size_t>(item)] = -1;

    return item;
}

void EadLaterPlayer::Remove(int voice)
{
    const auto node = static_cast<std::size_t>(voice);
    if (previous_[node] == -1)
    {
        return;
    }

    next_[static_cast<std::size_t>(previous_[node])] = next_[node];
    previous_[static_cast<std::size_t>(next_[node])] = previous_[node];
    previous_[node] = -1;
}

void EadLaterPlayer::ClearPool(int pool)
{
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        const int source = Head(pool, kind);
        const int target = Head(0, kind);
        while (true)
        {
            const int voice = next_[static_cast<std::size_t>(source)];
            if (voice == source || voice == -1)
            {
                break;
            }

            Remove(voice);
            PushBack(target, voice);
        }
    }
}

void EadLaterPlayer::FillPool(int pool, int count)
{
    ClearPool(pool);
    int taken = 0;
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        while (taken < count)
        {
            const int voice = PopBack(Head(0, kind));
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

int EadLaterPlayer::FindLowerPriority(int head, int limit) const
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

    if (limit <= voices_[static_cast<std::size_t>(best)].priority)
    {
        return -1;
    }

    return best;
}

EadEnvelope::Point EadLaterPlayer::EnvelopePoint(const EnvelopeRef& envelope, int index) const
{
    if (envelope.bank != nullptr)
    {
        return static_cast<std::size_t>(index) < envelope.bank->points.size()
                   ? envelope.bank->points[static_cast<std::size_t>(index)]
                   : EadEnvelope::Point{kEnvelopeDisable, 0};
    }

    if (envelope.player >= 0)
    {
        const Player& player = players_[static_cast<std::size_t>(envelope.player)];
        const uint32_t place = envelope.offset + static_cast<uint32_t>(index) * 4;

        return {static_cast<int16_t>(Peek16(player, place)), static_cast<int16_t>(Peek16(player, place + 2))};
    }

    return static_cast<std::size_t>(index) < kDefaultEnvelope.size() ? kDefaultEnvelope[static_cast<std::size_t>(index)]
                                                                     : EadEnvelope::Point{kEnvelopeDisable, 0};
}

std::array<int16_t, 8> EadLaterPlayer::FilterTaps(const Voice& voice, const FilterRef& filter) const
{
    if (filter.saved)
    {
        return voice.attributes.saved_filter;
    }

    std::array<int16_t, 8> taps = {};
    const Player& player = players_[static_cast<std::size_t>(filter.player)];
    for (std::size_t i = 0; i < taps.size(); i++)
    {
        taps[i] = static_cast<int16_t>(Peek16(player, filter.offset + static_cast<uint32_t>(i) * 2));
    }

    return taps;
}

void EadLaterPlayer::PlayerSound(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (player.fade_updates != 0 && (!majora_ || player.skip_ticks == 0))
    {
        player.fade_volume += player.fade_step;
        player.recalculate_volume = true;
        if (player.fade_volume > 1.0F)
        {
            player.fade_volume = 1.0F;
        }

        if (player.fade_volume < 0.0F)
        {
            player.fade_volume = 0.0F;
        }

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

    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        const int channel = index * kChannelsPerPlayer + slot;
        if (channels_[static_cast<std::size_t>(channel)].enabled)
        {
            ChannelSound(channel, player.recalculate_volume, player.apply_bend);
        }
    }

    player.recalculate_volume = false;
}

// Sets the note properties of a layer in Pokémon Stadium 2 from the changes of its channel.
void EadLaterPlayer::SetLayerNoteStadium2(Layer& layer, const Channel& channel, float frequency, uint8_t note_pan,
                                          bool recalculate_volume)
{
    // Stadium 2's code at 0x8001B7A0 to 0x8001B8BC. Each of three flags of the layer lets the channel's value reach it.
    // With the pan's flag clear, the code writes the layer's pan to the note's velocity.
    const bool all = layer.properties_need_init;
    layer.properties_need_init = false;
    if (all || channel.change_frequency)
    {
        layer.note_frequency =
            (layer.flags & kLayerFlagFrequency) != 0 ? layer.frequency_scale * frequency : layer.frequency_scale;
    }

    if (all || channel.change_volume || recalculate_volume)
    {
        layer.note_velocity = (layer.flags & kLayerFlagVolume) != 0 ? layer.velocity_varied * channel.applied_volume
                                                                    : layer.velocity_varied;
    }

    if (all || channel.change_pan)
    {
        if ((layer.flags & kLayerFlagPan) != 0)
        {
            layer.note_pan = note_pan;
        }
        else
        {
            layer.note_velocity = static_cast<float>(layer.pan);
        }
    }
}

// Sets the note properties of a layer from the changes of its channel.
void EadLaterPlayer::SetLayerNote(Layer& layer, const Channel& channel, float frequency, uint8_t note_pan,
                                  bool recalculate_volume)
{
    if (layer.properties_need_init)
    {
        layer.note_frequency = layer.frequency_scale * frequency;
        layer.note_velocity = layer.velocity_varied * channel.applied_volume;
        layer.note_pan = note_pan;
        layer.properties_need_init = false;

        return;
    }

    if (channel.change_frequency)
    {
        layer.note_frequency = layer.frequency_scale * frequency;
    }

    if (channel.change_volume || recalculate_volume)
    {
        layer.note_velocity = layer.velocity_varied * channel.applied_volume;
    }

    if (channel.change_pan)
    {
        layer.note_pan = note_pan;
    }
}

void EadLaterPlayer::ChannelSound(int index, bool recalculate_volume, bool apply_bend)
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

        channel.applied_volume = volume * volume;
    }

    if (channel.change_pan)
    {
        channel.pan = channel.new_pan * channel.pan_weight;
    }

    float frequency = channel.frequency_scale;
    if (apply_bend)
    {
        frequency *= player.bend;
        channel.change_frequency = true;
    }

    for (const int layer_index : channel.layers)
    {
        if (layer_index == kNoLayer)
        {
            continue;
        }

        Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
        if (!layer.enabled || layer.voice == -1)
        {
            continue;
        }

        const auto note_pan = static_cast<uint8_t>((channel.pan + layer.pan * (0x80 - channel.pan_weight)) >> 7);
        if (stadium_2_)
        {
            SetLayerNoteStadium2(layer, channel, frequency, note_pan, recalculate_volume);
        }
        else
        {
            SetLayerNote(layer, channel, frequency, note_pan, recalculate_volume);
        }
    }

    channel.change_frequency = false;
    channel.change_volume = false;
    channel.change_pan = false;
}

// Reads the points of an envelope from its current index, up to one that starts a fade or changes the phase.
void EadLaterPlayer::ReadEnvelopePoints(Adsr& adsr)
{
    while (true)
    {
        const EadEnvelope::Point point = EnvelopePoint(adsr.envelope, adsr.index);
        adsr.delay = point.delay;
        if (adsr.delay == kEnvelopeDisable)
        {
            adsr.phase = AdsrPhase::kDisabled;
        }
        else if (adsr.delay == kEnvelopeHang)
        {
            adsr.phase = AdsrPhase::kHang;
        }
        else if (adsr.delay == kEnvelopeGoto)
        {
            adsr.index = static_cast<uint8_t>(point.target);
            continue;
        }
        else if (adsr.delay == kEnvelopeRestart)
        {
            adsr.phase = AdsrPhase::kInitial;
        }
        else if (adsr.delay == kEnvelopeSkip && stadium_2_)
        {
            adsr.start_at_target = true;
            adsr.index++;
            continue;
        }
        else
        {
            adsr.delay = static_cast<int16_t>(static_cast<float>(adsr.delay) * updates_scaled_);
            if (adsr.delay == 0)
            {
                adsr.delay = 1;
            }

            adsr.target = static_cast<float>(point.target) / 32767.0F;
            adsr.target = adsr.target * adsr.target;
            adsr.velocity = (adsr.target - adsr.current) / static_cast<float>(adsr.delay);
            adsr.phase = AdsrPhase::kFade;
            adsr.index++;
        }

        break;
    }
}

// Lowers the level of an envelope in its decay or its release. A decay with a sustain level stops there.
void EadLaterPlayer::FadeAdsr(Adsr& adsr, AdsrPhase phase)
{
    adsr.current -= adsr.fade_step;
    if (adsr.sustain != 0.0F && phase == AdsrPhase::kDecay)
    {
        if (adsr.current < adsr.sustain)
        {
            adsr.current = adsr.sustain;
            adsr.delay = 128;
            adsr.phase = AdsrPhase::kSustain;
        }

        return;
    }

    if (adsr.current < 0.00001F)
    {
        adsr.current = 0.0F;
        adsr.phase = AdsrPhase::kDisabled;
    }
}

float EadLaterPlayer::UpdateAdsr(Adsr& adsr)
{
    const AdsrPhase phase = adsr.phase;
    switch (phase)
    {
    case AdsrPhase::kDisabled:
        return 0.0F;
    case AdsrPhase::kInitial:
        if (adsr.hang)
        {
            adsr.phase = AdsrPhase::kHang;
            break;
        }

        [[fallthrough]];
    case AdsrPhase::kStartLoop:
        adsr.index = 0;
        adsr.phase = AdsrPhase::kLoop;
        [[fallthrough]];
    case AdsrPhase::kLoop:
        ReadEnvelopePoints(adsr);
        if (adsr.phase != AdsrPhase::kFade)
        {
            break;
        }

        [[fallthrough]];
    case AdsrPhase::kFade:
        adsr.current += adsr.velocity;
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
        FadeAdsr(adsr, phase);
        break;
    case AdsrPhase::kSustain:
        adsr.delay--;
        if (adsr.delay == 0)
        {
            adsr.phase = AdsrPhase::kRelease;
        }

        break;
    }

    if (adsr.decay)
    {
        adsr.phase = AdsrPhase::kDecay;
        adsr.decay = false;
    }

    if (adsr.release)
    {
        adsr.phase = AdsrPhase::kRelease;
        adsr.release = false;
    }

    if (adsr.current < 0.0F)
    {
        return 0.0F;
    }

    return adsr.current > 1.0F ? 1.0F : adsr.current;
}

float EadLaterPlayer::VibratoScale(Vibrato& vibrato)
{
    if (vibrato.delay != 0)
    {
        vibrato.delay--;

        return 1.0F;
    }

    const Channel* channel = vibrato.channel >= 0 ? &channels_[static_cast<std::size_t>(vibrato.channel)] : nullptr;
    const int32_t depth_target = channel != nullptr ? channel->vibrato_depth_target : 0;
    const int32_t rate_target = channel != nullptr ? channel->vibrato_rate_target : kLayerVibratoRate;
    const uint16_t depth_delay = channel != nullptr ? channel->vibrato_depth_delay : uint16_t{0};
    const uint16_t rate_delay = channel != nullptr ? channel->vibrato_rate_delay : uint16_t{0};
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
        vibrato.depth_timer = depth_delay;
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
        vibrato.rate_timer = rate_delay;
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
    const std::size_t index = (vibrato.time >> 10) & 0x3F;
    if (stadium_2_)
    {
        // The code at 0x8001BC5C in Pokémon Stadium 2.
        const float wave = static_cast<float>(tables::kWaves[2 * 256 + index]);

        return 65536.0F / (wave * (vibrato.depth * (1.0F / 4096.0F)) + 65536.0F);
    }

    const float change = static_cast<float>(tables::kWaves[2 * 256 + index]) + 32768.0F;
    const float depth = vibrato.depth / 4096.0F + 1.0F;
    const float inverse = 1.0F / depth;

    return 1.0F / ((depth - inverse) * change / 65536.0F + inverse);
}

void EadLaterPlayer::UpdateVibrato(Voice& voice)
{
    if (voice.portamento.mode != 0)
    {
        voice.portamento.position = static_cast<uint16_t>(voice.portamento.position + voice.portamento.speed);
        uint32_t index = (voice.portamento.position >> 8) & 0xFF;
        if (index >= 127)
        {
            index = 127;
            voice.portamento.mode = 0;
        }

        voice.portamento_scale = 1.0F + voice.portamento.extent * (tables::kBendOctave[index + 128] - 1.0F);
    }

    if (voice.vibrato.active)
    {
        voice.vibrato_scale = VibratoScale(voice.vibrato);
    }
}

void EadLaterPlayer::InitVibrato(int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    voice.vibrato_scale = 1.0F;
    Vibrato& vibrato = voice.vibrato;
    vibrato.active = true;
    vibrato.time = 0;
    const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
    if ((layer.flags & kLayerUsesChannelVibrato) == 0)
    {
        vibrato.channel = -1;
        vibrato.depth_timer = 0;
        vibrato.depth = 0.0F;
        vibrato.rate_timer = 0;
        vibrato.rate = static_cast<float>(kLayerVibratoRate);
        vibrato.delay = 0;

        return;
    }

    vibrato.channel = layer.channel;
    const Channel& channel = channels_[static_cast<std::size_t>(vibrato.channel)];
    vibrato.depth_timer = channel.vibrato_depth_delay;
    vibrato.depth = static_cast<float>(
        int32_t{vibrato.depth_timer == 0 ? channel.vibrato_depth_target : channel.vibrato_depth_start});
    vibrato.rate_timer = channel.vibrato_rate_delay;
    vibrato.rate =
        static_cast<float>(int32_t{vibrato.rate_timer == 0 ? channel.vibrato_rate_target : channel.vibrato_rate_start});
    vibrato.delay = channel.vibrato_delay;
}

// Copies the note properties of a layer and its channel to a voice. The voice plays with them after the layer leaves
// it. A voice of a muted player that stops voices finishes.
void EadLaterPlayer::SaveNoteAttributes(Voice& voice, const Layer& layer, const Channel& channel)
{
    Attributes& attributes = voice.attributes;
    attributes.frequency_scale = layer.note_frequency;
    attributes.velocity = layer.note_velocity;
    attributes.pan = layer.note_pan;
    attributes.reverb = (layer.flags & kLayerUsesChannelReverb) != 0 ? channel.reverb : layer.reverb;
    attributes.surround = layer.surround == 0x80 ? channel.surround : layer.surround;
    attributes.gain = (layer.flags & kLayerUsesChannelGain) != 0 ? channel.gain : uint8_t{0};
    attributes.filter.reset();
    if (channel.filter != -1)
    {
        attributes.filter = FilterRef{channel.player, static_cast<uint32_t>(channel.filter), false};
        attributes.saved_filter = FilterTaps(voice, *attributes.filter);
        attributes.filter = FilterRef{-1, 0, true};
    }

    attributes.comb_gain = channel.comb_gain;
    attributes.comb_size = channel.comb_size;
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    if (player.muted && (channel.mute_behavior & kMuteStopVoices) != 0)
    {
        voice.state.finished = true;
    }

    attributes.stereo = layer.stereo == 0 ? channel.stereo : layer.stereo;
}

void EadLaterPlayer::DecayOrRelease(int index, AdsrPhase target)
{
    if (index == kNoLayer)
    {
        return;
    }

    Layer& layer = layers_[static_cast<std::size_t>(index)];
    layer.loaded = false;
    if (layer.voice == -1)
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
        if (voice.layer == kNoLayer && voice.wanted_layer == kNoLayer && voice.previous_layer == index &&
            target != AdsrPhase::kDecay)
        {
            voice.adsr.fade_step = updates_inverse_;
            voice.adsr.release = true;
        }

        return;
    }

    if (voice.adsr.phase != AdsrPhase::kDecay)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
        SaveNoteAttributes(voice, layer, channel);
        voice.priority = channel.release_priority;
        voice.previous_layer = voice.layer;
        voice.layer = kNoLayer;
        if (target == AdsrPhase::kRelease)
        {
            voice.adsr.fade_step = updates_inverse_;
            voice.adsr.release = true;
            voice.release_kind = 2;
        }
        else
        {
            voice.release_kind = 1;
            voice.adsr.decay = true;
            voice.adsr.fade_step =
                layer.adsr.release == 0 ? decay_table_[channel.adsr.release] : decay_table_[layer.adsr.release];
            voice.adsr.sustain = (static_cast<float>(int32_t{channel.adsr.sustain}) * voice.adsr.current) / 256.0F;
        }
    }

    if (target == AdsrPhase::kDecay)
    {
        Remove(voice_index);
        PushFront(Head(voice.pool, kDecaying), voice_index);
    }
}

void EadLaterPlayer::InitVoice(Voice& voice)
{
    const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
    voice.adsr.hang = false;
    voice.adsr.decay = false;
    voice.adsr.release = false;
    voice.adsr.start_at_target = false;
    voice.adsr.delay = 0;
    voice.adsr.envelope = layer.adsr.release == 0 ? channels_[static_cast<std::size_t>(layer.channel)].adsr.envelope
                                                  : layer.adsr.envelope;
    voice.adsr.sustain = 0.0F;
    voice.adsr.current = 0.0F;
    voice.release_kind = 0;
    voice.adsr.phase = AdsrPhase::kInitial;
    voice.state = {};
    voice.state.enabled = true;
    voice.state.needs_init = true;
}

void EadLaterPlayer::DisableVoice(Voice& voice)
{
    if (voice.state.needs_init)
    {
        voice.state.needs_init = false;
    }

    voice.priority = 0;
    voice.state.enabled = false;
    voice.release_kind = 0;
    voice.state.finished = false;
    voice.layer = kNoLayer;
    voice.previous_layer = kNoLayer;
    voice.adsr.phase = AdsrPhase::kDisabled;
    voice.adsr.current = 0.0F;
}

int EadLaterPlayer::BuildWave(Voice& voice, Layer& layer, int wave)
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
    float ratio = 1.0465F / 8.0F - 2.5E-6F;
    if (frequency < 0.99999F)
    {
        harmonic = 0;
        ratio = 1.0465F;
    }
    else if (frequency < 1.99999F)
    {
        harmonic = 1;
        ratio = 1.0465F / 2.0F;
    }
    else if (frequency < 3.99999F)
    {
        harmonic = 2;
        ratio = 1.0465F / 4.0F + 1.005E-3F;
    }

    layer.frequency_scale *= ratio;
    voice.wave = static_cast<uint8_t>(wave);
    voice.harmonic = static_cast<uint8_t>(harmonic);
    voice.state.wave_address =
        waves_address_ + static_cast<uint32_t>(wave - 128) * 0x200 + static_cast<uint32_t>(harmonic * kWaveSamples * 2);

    return harmonic;
}

void EadLaterPlayer::InitWave(Voice& voice, Layer& layer)
{
    int wave = layer.program;
    if (wave == 0xFF)
    {
        wave = channels_[static_cast<std::size_t>(layer.channel)].program;
    }

    const int previous = voice.harmonic;
    const int current = BuildWave(voice, layer, wave);
    if (current != previous)
    {
        voice.state.harmonics = static_cast<uint8_t>((current << 2) + previous);
    }
}

void EadLaterPlayer::InitVoiceForLayer(int index, int layer_index)
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
    int program = layer.program;
    if (program == 0xFF)
    {
        program = channel.program;
    }

    voice.state.sound = layer.sound;
    voice.state.sound_font = layer.sound_font;
    voice.state.synthetic_wave = program >= 0x80 && program < 0xC0;
    if (voice.state.synthetic_wave)
    {
        BuildWave(voice, layer, program);
    }

    if (majora_ && !voice.state.synthetic_wave)
    {
        const EadSample* sample = SampleOf(layer.sound_font, layer.sound);
        if (sample != nullptr && sample->loop != kNoObject)
        {
            const BankLoop& loop = files_->banks[layer.sound_font].loops[static_cast<std::size_t>(sample->loop)];
            voice.start_position = channel.start_sample == 1 ? loop.start : uint32_t{channel.start_sample};
            if (channel.start_sample != 1 && voice.start_position >= loop.end)
            {
                voice.start_position = 0;
            }
        }
    }

    voice.font = channel.font;
    voice.stereo_effects = channel.stereo_effects;
    voice.state.reverb_index = channel.reverb_index & 3;
}

void EadLaterPlayer::TakeAfterRelease(int index, int layer_index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    voice.wanted_layer = layer_index;
    voice.priority =
        channels_[static_cast<std::size_t>(layers_[static_cast<std::size_t>(layer_index)].channel)].priority;
    voice.adsr.fade_step = updates_inverse_;
    voice.adsr.release = true;
}

int EadLaterPlayer::AllocFromDisabled(int pool, int layer)
{
    const int voice = PopBack(Head(pool, kDisabled));
    if (voice != -1)
    {
        InitVoiceForLayer(voice, layer);
        PushFront(Head(pool, kActive), voice);
    }

    return voice;
}

int EadLaterPlayer::AllocFromDecaying(int pool, int layer)
{
    const int voice = PopBack(Head(pool, kDecaying));
    if (voice != -1)
    {
        TakeAfterRelease(voice, layer);
        PushBack(Head(pool, kReleasing), voice);
    }

    return voice;
}

int EadLaterPlayer::AllocFromActive(int pool, int layer_index)
{
    const int priority =
        channels_[static_cast<std::size_t>(layers_[static_cast<std::size_t>(layer_index)].channel)].priority;
    int releasing_priority = 0x10;
    int active_priority = 0x10;
    const int releasing = FindLowerPriority(Head(pool, kReleasing), priority);
    if (releasing != -1)
    {
        releasing_priority = voices_[static_cast<std::size_t>(releasing)].priority;
    }

    const int active = FindLowerPriority(Head(pool, kActive), priority);
    if (active != -1)
    {
        active_priority = voices_[static_cast<std::size_t>(active)].priority;
    }

    if (releasing == -1 && active == -1)
    {
        return -1;
    }

    if (active_priority < releasing_priority)
    {
        Remove(active);
        Voice& voice = voices_[static_cast<std::size_t>(active)];
        DecayOrRelease(voice.layer, AdsrPhase::kRelease);
        voice.wanted_layer = layer_index;
        PushBack(Head(pool, kReleasing), active);
        voice.priority = static_cast<uint8_t>(priority);

        return active;
    }

    Voice& voice = voices_[static_cast<std::size_t>(releasing)];
    voice.wanted_layer = layer_index;
    voice.priority = static_cast<uint8_t>(priority);

    return releasing;
}

int EadLaterPlayer::AllocVoice(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const int channel = layer.channel;
    const int channel_pool = ChannelPool(channel);
    const int player_pool = PlayerPool(channels_[static_cast<std::size_t>(channel)].player);
    const uint32_t policy = channels_[static_cast<std::size_t>(channel)].policy;
    if ((policy & 1) != 0)
    {
        const int voice_index = layer.voice;
        if (voice_index != -1)
        {
            Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
            if (voice.previous_layer == index && voice.wanted_layer == kNoLayer)
            {
                TakeAfterRelease(voice_index, index);
                Remove(voice_index);
                PushBack(Head(voice.pool, kReleasing), voice_index);

                return voice_index;
            }
        }
    }

    int voice = -1;
    const auto try_pools = [&](std::initializer_list<int> pools)
    {
        for (const int pool : pools)
        {
            voice = AllocFromDisabled(pool, index);
            if (voice != -1)
            {
                return;
            }
        }

        for (const int pool : pools)
        {
            voice = AllocFromDecaying(pool, index);
            if (voice != -1)
            {
                return;
            }
        }

        for (const int pool : pools)
        {
            voice = AllocFromActive(pool, index);
            if (voice != -1)
            {
                return;
            }
        }
    };

    if ((policy & 2) != 0)
    {
        try_pools({channel_pool});
    }
    else if ((policy & 4) != 0)
    {
        try_pools({channel_pool, player_pool});
    }
    else if ((policy & 8) != 0)
    {
        try_pools({0});
    }
    else
    {
        try_pools({channel_pool, player_pool, 0});
    }

    if (voice == -1)
    {
        layer.loaded = true;
    }

    return voice;
}

// Sets the stereo fields of an update state from a pan, the stereo flags and the output mode. Returns the volumes of
// the left and right sides.
std::array<float, 2> EadLaterPlayer::ApplyPan(const Voice& voice, UpdateState& state, uint8_t pan, uint8_t stereo) const
{
    const bool strong_right_bit = (stereo & 0x08) != 0;
    const bool strong_left_bit = (stereo & 0x04) != 0;
    state.strong_right = false;
    state.strong_left = false;
    state.stereo_effects = (stereo & 0x02) != 0;
    state.headset_effects = (stereo & 0x01) != 0;
    float left = 0.0F;
    float right = 0.0F;
    if (voice.stereo_effects && config_.output == EadLaterOutput::kHeadset)
    {
        int half = pan >> 1;
        if (half > 0x3F)
        {
            half = 0x3F;
        }

        state.delay_right = static_cast<uint8_t>(tables::kHaasDelays[static_cast<std::size_t>(half)]);
        state.delay_left = static_cast<uint8_t>(tables::kHaasDelays[static_cast<std::size_t>(0x3F - half)]);
        state.use_delay = true;
        left = tables::kHeadsetPanVolume[pan];
        right = tables::kHeadsetPanVolume[0x7FU - pan];
    }
    else if (voice.stereo_effects && config_.output == EadLaterOutput::kStereo)
    {
        const bool strong_left = pan < 0x20;
        const bool strong_right = pan > 0x60;
        state.delay_left = 0;
        state.delay_right = 0;
        state.use_delay = false;
        left = tables::kStereoPanVolume[pan];
        right = tables::kStereoPanVolume[0x7FU - pan];
        state.strong_right = strong_right;
        state.strong_left = strong_left;
        switch ((stereo >> 4) & 3)
        {
        case 1:
            state.strong_right = strong_right_bit;
            state.strong_left = strong_left_bit;
            break;
        case 2:
            state.strong_right = strong_right_bit || strong_right;
            state.strong_left = strong_left_bit || strong_left;
            break;
        case 3:
            state.strong_right = strong_right_bit != strong_right;
            state.strong_left = strong_left_bit != strong_left;
            break;
        default:
            break;
        }
    }
    else if (config_.output == EadLaterOutput::kMono)
    {
        state.stereo_effects = false;
        state.headset_effects = false;
        left = 0.707F;
        right = 0.707F;
    }
    else
    {
        state.strong_right = strong_right_bit;
        state.strong_left = strong_left_bit;
        left = tables::kDefaultPanVolume[pan];
        right = tables::kDefaultPanVolume[0x7FU - pan];
    }

    return {left, right};
}

void EadLaterPlayer::InitUpdateState(Voice& voice, UpdateState& state, float frequency, float velocity, uint8_t pan,
                                     uint8_t reverb, uint8_t stereo, uint8_t gain,
                                     const std::optional<FilterRef>& filter, uint8_t comb_size, uint16_t comb_gain)
{
    state.enabled = voice.state.enabled;
    state.needs_init = voice.state.needs_init;
    state.finished = voice.state.finished;
    state.strong_right = voice.state.strong_right;
    state.strong_left = voice.state.strong_left;
    state.stereo_effects = voice.state.stereo_effects;
    state.headset_effects = voice.state.headset_effects;
    state.reverb_index = voice.state.reverb_index;
    state.book_offset = voice.state.book_offset;
    state.synthetic_wave = voice.state.synthetic_wave;
    state.two_parts = voice.state.two_parts;
    state.use_delay = voice.state.use_delay;
    state.sound = voice.state.sound;
    state.sound_font = voice.state.sound_font;
    state.wave_address = voice.state.wave_address;
    state.harmonics = voice.state.harmonics;

    float rate = 0.0F;
    if (frequency < 2.0F)
    {
        state.two_parts = false;
        rate = frequency > 1.99998F ? 1.99998F : frequency;
    }
    else
    {
        state.two_parts = true;
        rate = frequency > 3.99996F ? 1.99998F : frequency * 0.5F;
    }

    state.pitch = static_cast<uint16_t>(static_cast<int32_t>(rate * 32768.0F));

    pan &= 0x7F;
    const std::array<float, 2> volumes = ApplyPan(voice, state, pan, stereo);
    const float left = volumes[0];
    const float right = volumes[1];
    velocity = 0.0F > velocity ? 0.0F : velocity;
    velocity = 1.0F < velocity ? 1.0F : velocity;
    constexpr float kVolumeScale = 0x1000 - 0.001F;
    state.target_left = static_cast<uint16_t>(static_cast<int32_t>((velocity * left) * kVolumeScale));
    state.target_right = static_cast<uint16_t>(static_cast<int32_t>((velocity * right) * kVolumeScale));
    state.gain = gain;
    state.filter = filter;
    state.comb_size = comb_size;
    state.comb_gain = comb_gain;
    state.reverb = reverb;
}

// Releases a voice whose layer has taken another voice, has stopped, or belongs to a muted player that stops notes.
void EadLaterPlayer::ReleaseStoppedVoice(Voice& voice, int index)
{
    const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    bool release_now = false;
    if (index != layer.voice && voice.release_kind == 0)
    {
        voice.adsr.release = true;
        voice.adsr.fade_step = updates_inverse_;
        voice.priority = 1;
        voice.release_kind = 2;
    }
    else if (!layer.enabled && voice.release_kind == 0 && voice.priority >= 1)
    {
        release_now = true;
    }
    else if (player.muted && (channel.mute_behavior & kMuteStopNotes) != 0)
    {
        release_now = true;
    }

    if (release_now)
    {
        DecayOrRelease(voice.layer, AdsrPhase::kRelease);
        Remove(index);
        PushFront(Head(voice.pool, kDecaying), index);
        voice.priority = 1;
        voice.release_kind = 2;
    }
}

// Turns off a voice and moves it to the disabled list of its pool. Sets `continuous_started` on the layer that the
// voice still plays, if any.
void EadLaterPlayer::RetireVoice(Voice& voice, int index)
{
    if (voice.layer != kNoLayer)
    {
        layers_[static_cast<std::size_t>(voice.layer)].continuous_started = true;
    }

    DisableVoice(voice);
    Remove(index);
    PushBack(Head(voice.pool, kDisabled), index);
}

// Starts the note of the layer that waits for a voice, once the voice has ended.
void EadLaterPlayer::StartWantedLayer(Voice& voice, int index)
{
    DisableVoice(voice);
    InitVoiceForLayer(index, voice.wanted_layer);
    InitVibrato(index);
    voice.portamento_scale = 1.0F;
    voice.portamento = layers_[static_cast<std::size_t>(voice.layer)].portamento;
    Remove(index);
    PushBack(Head(voice.pool, kActive), index);
    voice.wanted_layer = kNoLayer;
}

// Runs the envelope, the vibrato and the portamento of a voice. Records the settings of the voice for the update in
// `recorded`.
void EadLaterPlayer::RecordUpdate(Voice& voice, UpdateState& recorded)
{
    const float scale = UpdateAdsr(voice.adsr);
    UpdateVibrato(voice);
    float frequency = 0.0F;
    float velocity = 0.0F;
    uint8_t pan = 0;
    uint8_t reverb = 0;
    uint8_t stereo = 0;
    uint8_t gain = 0;
    std::optional<FilterRef> filter;
    uint8_t comb_size = 0;
    uint16_t comb_gain = 0;
    uint8_t book_offset = 0;
    uint8_t surround = 0xFF;
    if (voice.release_kind == 1 || voice.release_kind == 2)
    {
        const Attributes& attributes = voice.attributes;
        frequency = attributes.frequency_scale;
        velocity = attributes.velocity;
        pan = attributes.pan;
        reverb = attributes.reverb;
        surround = attributes.surround;
        stereo = attributes.stereo;
        gain = attributes.gain;
        filter = attributes.filter;
        comb_size = attributes.comb_size;
        comb_gain = attributes.comb_gain;
        book_offset = voice.state.book_offset;
    }
    else
    {
        const Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
        const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
        frequency = layer.note_frequency;
        velocity = layer.note_velocity;
        pan = layer.note_pan;
        const bool layer_stereo = majora_ ? ((layer.stereo >> 4) & 3) != 0 : layer.stereo != 0;
        stereo = layer_stereo ? layer.stereo : channel.stereo;
        reverb = (layer.flags & kLayerUsesChannelReverb) != 0 ? channel.reverb : layer.reverb;
        surround = layer.surround == 0x80 ? channel.surround : layer.surround;
        gain = (layer.flags & kLayerUsesChannelGain) != 0 ? channel.gain : uint8_t{0};
        if (channel.filter != -1)
        {
            filter = FilterRef{channel.player, static_cast<uint32_t>(channel.filter), false};
        }

        comb_size = channel.comb_size;
        comb_gain = channel.comb_gain;
        book_offset = channel.book_offset & 0x7;
        if (players_[static_cast<std::size_t>(channel.player)].muted && (channel.mute_behavior & kMuteStopVoices) != 0)
        {
            frequency = 0.0F;
            velocity = 0.0F;
        }
    }

    frequency *= voice.vibrato_scale * voice.portamento_scale;
    frequency *= resample_rate_;
    velocity *= scale;
    InitUpdateState(voice, recorded, frequency, velocity, pan, reverb, stereo, gain, filter, comb_size, comb_gain);
    recorded.surround = surround;
    voice.state.book_offset = book_offset & 3;
}

void EadLaterPlayer::ProcessVoices(int state_offset)
{
    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        Voice& voice = voices_[i];
        const int index = static_cast<int>(i);
        if (voice.layer != kNoLayer)
        {
            ReleaseStoppedVoice(voice, index);
        }
        else if (voice.release_kind == 0 && voice.priority >= 1)
        {
            continue;
        }

        if (voice.priority == 0)
        {
            continue;
        }

        if (voice.release_kind >= 1 || voice.state.finished)
        {
            if (voice.adsr.phase == AdsrPhase::kDisabled || voice.state.finished)
            {
                if (voice.wanted_layer == kNoLayer)
                {
                    RetireVoice(voice, index);
                    continue;
                }

                StartWantedLayer(voice, index);
            }
        }
        else if (voice.adsr.phase == AdsrPhase::kDisabled)
        {
            RetireVoice(voice, index);
            continue;
        }

        RecordUpdate(voice, update_states_[static_cast<std::size_t>(state_offset) + i]);
    }
}

void EadLaterPlayer::EnsureBank(AudioMemory& memory, int font)
{
    std::vector<uint32_t>& books = book_addresses_[static_cast<std::size_t>(font)];
    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (!books.empty() || bank.books.empty())
    {
        return;
    }

    books.reserve(bank.books.size());
    for (const BankBook& book : bank.books)
    {
        const uint32_t address = memory.Allocate(book.book.coefficients.size() * 2);
        for (std::size_t i = 0; i < book.book.coefficients.size(); i++)
        {
            WriteBe16(memory, address + static_cast<uint32_t>(i) * 2, book.book.coefficients[i]);
        }

        books.push_back(address);
    }

    std::vector<uint32_t>& loops = loop_addresses_[static_cast<std::size_t>(font)];
    loops.reserve(bank.loops.size());
    for (const BankLoop& loop : bank.loops)
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

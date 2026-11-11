// SPDX-License-Identifier: MIT

// The model of the middle EAD engine: the players, channels and layers, and what the game writes to them.
// ead_middle_scripts.cpp contains the scripts, ead_middle_voices.cpp the voices, and ead_middle_synthesis.cpp the audio
// lists.

#include "ntdlib/n64/ead_middle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ead_later_tables.h"
#include "ead_lists.h"
#include "ead_middle_constants.h"

namespace ntdlib::n64
{

namespace
{

using ead_lists::Align16;
using ead_lists::WriteBe16;
using ead_middle_constants::kMuteSoften;
using ead_middle_constants::kMuteStopNotes;
using ead_middle_constants::kMuteStopScript;
using ead_middle_constants::kOneChannel;
using ead_middle_constants::kPriorityDefault;
using ead_middle_constants::kStdOneChannel;
using ead_middle_constants::kTicksPerBeat;
using ead_middle_constants::kTwoChannels;

namespace later_tables = ead_later_tables;

constexpr int kPoolCount = 1 + EadMiddlePlayer::kPlayers + EadMiddlePlayer::kChannels;

} // namespace

EadMiddlePlayer::EadMiddlePlayer(const EadMiddleConfig& config, const EadEarlyFiles& files, AudioMemory& memory)
    : config_(config), files_(&files)
{
    const int frame_target = Align16(config.frequency / config.refresh_rate);
    const int updates = (frame_target + 16) / (config.standard_microcode ? 160 : 192) + 1;
    samples_per_update_ = (frame_target / updates) & ~7;
    samples_per_update_max_ = samples_per_update_ + 8;
    samples_per_update_min_ = samples_per_update_ - 8;
    updates_inverse_scaled_ = 0.001171875F / static_cast<float>(updates);
    updates_inverse_ = 1.0F / static_cast<float>(updates);
    max_tempo_ = static_cast<int16_t>(static_cast<uint32_t>(static_cast<float>(updates) * 2880000.0F /
                                                            static_cast<float>(kTicksPerBeat) / config.tempo_divisor));
    updates_per_frame_ = updates * config.frames_per_list;

    InitVoiceLists();
    if (config.standard_microcode)
    {
        BuildRampTables();
    }

    AllocateVoices(memory);
    waves_address_ = memory.Allocate(later_tables::kWaves.size() * 2);
    for (std::size_t i = 0; i < later_tables::kWaves.size(); i++)
    {
        WriteBe16(memory, waves_address_ + static_cast<uint32_t>(i) * 2, later_tables::kWaves[i]);
    }

    AllocateReverbs(memory);
    channels_.resize(kChannels);
    layers_.resize(kLayers);
    layer_is_free_.assign(kLayers, 1);
    free_layers_.resize(kLayers);
    for (int i = 0; i < kLayers; i++)
    {
        free_layers_[static_cast<std::size_t>(i)] = i;
    }

    bank_loaded_.assign(files.banks.size(), 0);
    book_addresses_.resize(files.banks.size());
    loop_addresses_.resize(files.banks.size());
    for (Player& player : players_)
    {
        player.channels.fill(kNoChannel);
        player.mute_behavior = kMuteStopScript | kMuteStopNotes | kMuteSoften;
    }

    for (int i = 0; i < kPlayers; i++)
    {
        ResetPlayer(i);
    }
}

// Sizes the voices and their update states. Empties each list of voices: the head of a list links to itself.
void EadMiddlePlayer::InitVoiceLists()
{
    const auto voice_count = static_cast<std::size_t>(config_.voices);
    voices_.resize(voice_count);
    next_.resize(voice_count + static_cast<std::size_t>(kPoolCount) * 4);
    previous_.assign(next_.size(), -1);
    for (int head = 0; head < kPoolCount * 4; head++)
    {
        const int place = config_.voices + head;
        next_[static_cast<std::size_t>(place)] = place;
        previous_[static_cast<std::size_t>(place)] = place;
    }

    update_states_.resize(voice_count * static_cast<std::size_t>(updates_per_frame_));
}

// Allocates the buffers of each voice and adds the voice to the list of disabled voices.
void EadMiddlePlayer::AllocateVoices(AudioMemory& memory)
{
    for (int i = 0; i < config_.voices; i++)
    {
        Voice& voice = voices_[static_cast<std::size_t>(i)];
        voice.adpcm_state = memory.Allocate(0x20);
        voice.resample_state = memory.Allocate(0x20);
        voice.pan_resample_state = memory.Allocate(0x20);
        voice.pan_samples = memory.Allocate(0x40);
        voice.mixer_state = memory.Allocate(0x50);
        voice.halving_state = memory.Allocate(0x20);
        PushBack(Head(0, kDisabled), i);
    }
}

// Sets each reverb from the session's settings and allocates its buffers.
void EadMiddlePlayer::AllocateReverbs(AudioMemory& memory)
{
    reverbs_.resize(config_.reverbs.size());
    for (std::size_t i = 0; i < reverbs_.size(); i++)
    {
        const EadMiddleReverb& settings = config_.reverbs[i];
        Reverb& reverb = reverbs_[i];
        reverb.downsample = settings.downsample;
        reverb.gain = settings.gain;
        reverb.buffer_samples = int32_t{settings.window} * 64;
        reverb.left = memory.Allocate(static_cast<std::size_t>(reverb.buffer_samples) * 2);
        reverb.right = memory.Allocate(static_cast<std::size_t>(reverb.buffer_samples) * 2);
        for (std::vector<ReverbItem>& frame : reverb.items)
        {
            frame.resize(static_cast<std::size_t>(updates_per_frame_));
        }

        if (reverb.downsample != 1)
        {
            reverb.downsample_pitch = static_cast<uint16_t>(0x8000 / reverb.downsample);
            reverb.resample_left = memory.Allocate(0x20);
            reverb.resample_right = memory.Allocate(0x20);
            for (std::vector<ReverbItem>& frame : reverb.items)
            {
                for (ReverbItem& item : frame)
                {
                    item.downsample_left = memory.Allocate(kTwoChannels);
                    item.downsample_right =
                        item.downsample_left + (config_.standard_microcode ? kStdOneChannel : kOneChannel);
                }
            }
        }
    }
}

bool EadMiddlePlayer::BankLoaded(int bank) const
{
    return static_cast<std::size_t>(bank) < bank_loaded_.size() && bank_loaded_[static_cast<std::size_t>(bank)] != 0;
}

const EadInstrument* EadMiddlePlayer::Instrument(int bank, int program) const
{
    if (!BankLoaded(bank))
    {
        return nullptr;
    }

    const EadBank& held = files_->banks[static_cast<std::size_t>(bank)];
    if (static_cast<std::size_t>(program) >= held.program_instruments.size())
    {
        return nullptr;
    }

    const int32_t object = held.program_instruments[static_cast<std::size_t>(program)];

    return object == kNoObject ? nullptr : &held.instruments[static_cast<std::size_t>(object)];
}

const EadDrum* EadMiddlePlayer::Drum(int bank, int note) const
{
    if (!BankLoaded(bank))
    {
        return nullptr;
    }

    const EadBank& held = files_->banks[static_cast<std::size_t>(bank)];
    if (static_cast<std::size_t>(note) >= held.drum_notes.size())
    {
        return nullptr;
    }

    const int32_t object = held.drum_notes[static_cast<std::size_t>(note)];

    return object == kNoObject ? nullptr : &held.drums[static_cast<std::size_t>(object)];
}

const EadSample* EadMiddlePlayer::SampleOf(int bank, const EadTunedSample* sound) const
{
    if (sound == nullptr || sound->sample == kNoObject)
    {
        return nullptr;
    }

    return &files_->banks[static_cast<std::size_t>(bank)].samples[static_cast<std::size_t>(sound->sample)];
}

EadMiddlePlayer::EnvelopeRef EadMiddlePlayer::BankEnvelope(int bank, int32_t envelope) const
{
    EnvelopeRef result;
    if (envelope != kNoObject)
    {
        result.bank = &files_->banks[static_cast<std::size_t>(bank)].envelopes[static_cast<std::size_t>(envelope)];
    }

    return result;
}

void EadMiddlePlayer::ResetPlayer(int index)
{
    DisablePlayer(index);
    Player& player = players_[static_cast<std::size_t>(index)];
    player.delay = 0;
    player.state = 1;
    player.fade_updates = 0;
    player.fade_updates_next = 0;
    player.tempo_count = 0;
    player.tempo = 120 * kTicksPerBeat;
    player.transpose = 0;
    player.policy = 0;
    player.short_velocities = -1;
    player.short_gates = -1;
    player.fade_volume = 1.0F;
    player.fade_scale = 1.0F;
    player.fade_step = 0.0F;
    player.mute_scale = 0.5F;
}

void EadMiddlePlayer::DisablePlayer(int index)
{
    DisableChannels(index, 0xFFFF);
    ClearPool(PlayerPool(index));
    Player& player = players_[static_cast<std::size_t>(index)];
    player.finished = true;
    player.enabled = false;
}

void EadMiddlePlayer::InitChannels(int index, uint32_t mask)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        if ((mask & 1) != 0)
        {
            int channel = player.channels[static_cast<std::size_t>(slot)];
            if (channel != kNoChannel && channels_[static_cast<std::size_t>(channel)].player == index)
            {
                DisableChannel(channel);
                channels_[static_cast<std::size_t>(channel)].player = -1;
            }

            channel = kNoChannel;
            for (int i = 0; i < kChannels; i++)
            {
                if (channels_[static_cast<std::size_t>(i)].player == -1)
                {
                    channel = i;
                    break;
                }
            }

            player.channels[static_cast<std::size_t>(slot)] = channel;
            if (channel != kNoChannel)
            {
                InitChannel(channel);
                Channel& held = channels_[static_cast<std::size_t>(channel)];
                held.player = index;
                held.bank = player.default_bank;
                held.mute_behavior = player.mute_behavior;
                held.policy = player.policy;
            }
        }

        mask >>= 1;
    }
}

void EadMiddlePlayer::DisableChannels(int index, uint32_t mask)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        if ((mask & 1) != 0)
        {
            const int channel = player.channels[static_cast<std::size_t>(slot)];
            if (channel != kNoChannel)
            {
                if (channels_[static_cast<std::size_t>(channel)].player == index)
                {
                    DisableChannel(channel);
                    channels_[static_cast<std::size_t>(channel)].player = -1;
                }

                player.channels[static_cast<std::size_t>(slot)] = kNoChannel;
            }
        }

        mask >>= 1;
    }
}

void EadMiddlePlayer::InitChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const std::array<int, kLayersPerChannel> layers = channel.layers;
    channel = Channel{};
    channel.layers = layers;
    channel.change_frequency = true;
    channel.change_volume = true;
    channel.change_pan = true;
    channel.new_pan = 0x40;
    channel.pan_weight = 0x80;
    channel.priority = kPriorityDefault;
    channel.adsr.envelope = {};
    channel.adsr.release = 0x20;
    channel.adsr.sustain = 0;
    channel.vibrato_rate_target = 0x800;
    channel.vibrato_rate_start = 0x800;
    channel.ports.fill(-1);
    for (const ListKind kind : {kDisabled, kDecaying, kReleasing, kActive})
    {
        const int head = Head(ChannelPool(index), kind);
        next_[static_cast<std::size_t>(head)] = head;
        previous_[static_cast<std::size_t>(head)] = head;
    }
}

void EadMiddlePlayer::DisableChannel(int index)
{
    for (int slot = 0; slot < kLayersPerChannel; slot++)
    {
        FreeLayer(index, slot);
    }

    ClearPool(ChannelPool(index));
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel.enabled = false;
    channel.finished = true;
}

void EadMiddlePlayer::EnableChannel(int index, int slot, uint32_t script)
{
    const int channel_index = players_[static_cast<std::size_t>(index)].channels[static_cast<std::size_t>(slot)];
    if (channel_index == kNoChannel)
    {
        return;
    }

    Channel& channel = channels_[static_cast<std::size_t>(channel_index)];
    channel.enabled = true;
    channel.finished = false;
    channel.script.depth = 0;
    channel.script.pc = script;
    channel.delay = 0;
    for (int i = 0; i < kLayersPerChannel; i++)
    {
        FreeLayer(channel_index, i);
    }
}

bool EadMiddlePlayer::SetLayer(int channel_index, int slot)
{
    Channel& channel = channels_[static_cast<std::size_t>(channel_index)];
    int& held = channel.layers[static_cast<std::size_t>(slot)];
    if (held == kNoLayer)
    {
        if (free_layers_.empty())
        {
            return false;
        }

        held = free_layers_.back();
        free_layers_.pop_back();
        layer_is_free_[static_cast<std::size_t>(held)] = 0;
    }
    else
    {
        DecayOrRelease(held, false);
    }

    Layer& layer = layers_[static_cast<std::size_t>(held)];
    layer.channel = channel_index;
    layer.adsr = channel.adsr;
    layer.adsr.release = 0;
    layer.enabled = true;
    layer.muted = false;
    layer.continuous = false;
    layer.finished = false;
    layer.ignore_drum_pan = false;
    layer.portamento.mode = 0;
    layer.script.depth = 0;
    layer.loaded = false;
    layer.gate = 0x80;
    layer.pan = 0x40;
    layer.transpose = 0;
    layer.delay = 0;
    layer.gate_delay = 0;
    layer.voice = kNoVoice;
    layer.instrument = nullptr;
    layer.frequency_scale = 1.0F;
    layer.velocity_square = 0.0F;
    layer.program = 0xFF;

    return true;
}

void EadMiddlePlayer::DisableLayer(int index)
{
    DecayOrRelease(index, false);
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    layer.enabled = false;
    layer.finished = true;
}

void EadMiddlePlayer::FreeLayer(int channel_index, int slot)
{
    Channel& channel = channels_[static_cast<std::size_t>(channel_index)];
    const int index = channel.layers[static_cast<std::size_t>(slot)];
    if (index == kNoLayer)
    {
        return;
    }

    if (layer_is_free_[static_cast<std::size_t>(index)] == 0)
    {
        layer_is_free_[static_cast<std::size_t>(index)] = 1;
        free_layers_.push_back(index);
    }

    DisableLayer(index);
    channel.layers[static_cast<std::size_t>(slot)] = kNoLayer;
}

uint8_t EadMiddlePlayer::FindProgram(const Channel& channel, uint8_t program, const EadInstrument*& out,
                                     AdsrSettings& adsr) const
{
    const EadInstrument* instrument = Instrument(channel.bank, program);
    out = instrument;
    if (instrument == nullptr)
    {
        return 0;
    }

    adsr.envelope = BankEnvelope(channel.bank, instrument->envelope);
    adsr.release = instrument->release_rate;

    return static_cast<uint8_t>(program + 1);
}

void EadMiddlePlayer::SetProgram(int index, uint8_t program)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (program >= 0x80)
    {
        channel.program = program;
        channel.instrument = nullptr;
    }
    else if (program == 0x7F)
    {
        channel.program = 0;
        channel.instrument = nullptr;
    }
    else
    {
        channel.program = FindProgram(channel, program, channel.instrument, channel.adsr);
        if (channel.program == 0)
        {
            channel.has_instrument = false;

            return;
        }
    }

    channel.has_instrument = true;
}

bool EadMiddlePlayer::ChangeBank(Channel& channel, const Player& player, uint8_t from_back)
{
    const auto set = static_cast<std::size_t>(player.sequence);
    if (set >= files_->bank_sets.size() || from_back >= files_->bank_sets[set].size())
    {
        return false;
    }

    const std::vector<uint8_t>& banks = files_->bank_sets[set];
    const uint8_t bank = banks[banks.size() - 1 - from_back];
    if (!BankLoaded(bank))
    {
        return false;
    }

    channel.bank = bank;

    return true;
}

EadMiddlePlayer::Channel* EadMiddlePlayer::ChannelOf(int player, int channel)
{
    const int index = players_[static_cast<std::size_t>(player)].channels[static_cast<std::size_t>(channel & 0xF)];

    return index == kNoChannel ? nullptr : &channels_[static_cast<std::size_t>(index)];
}

void EadMiddlePlayer::Start(int index, int sequence, int fade_in_updates)
{
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= files_->sequences.Entries().size())
    {
        return;
    }

    if (files_->sequences.Entries()[static_cast<std::size_t>(sequence)].length == 0)
    {
        sequence = static_cast<int>(files_->sequences.Entries()[static_cast<std::size_t>(sequence)].offset);
    }

    DisablePlayer(index);
    Player& player = players_[static_cast<std::size_t>(index)];
    const auto set = static_cast<std::size_t>(sequence);
    if (set < files_->bank_sets.size() && !files_->bank_sets[set].empty())
    {
        for (const uint8_t bank : files_->bank_sets[set])
        {
            MarkBankLoaded(bank);
        }

        player.default_bank = files_->bank_sets[set].back();
    }

    player.sequence = sequence;
    ResetPlayer(index);
    const std::span<const uint8_t> bytes = files_->sequences.Sequence(set);
    player.bytes.assign(bytes.begin(), bytes.end());
    player.script = {};
    player.delay = 0;
    player.enabled = true;
    if (fade_in_updates != 0)
    {
        player.state = 1;
        player.fade_updates_next = static_cast<uint16_t>(fade_in_updates);
        player.fade_updates = static_cast<uint16_t>(fade_in_updates);
        player.fade_volume = 0.0F;
        player.fade_step = 0.0F;
    }
}

void EadMiddlePlayer::Stop(int player)
{
    if (players_[static_cast<std::size_t>(player)].enabled)
    {
        DisablePlayer(player);
    }
}

bool EadMiddlePlayer::Enabled(int player) const
{
    return players_[static_cast<std::size_t>(player)].enabled;
}

void EadMiddlePlayer::FadeOut(int index, int updates)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (!player.enabled)
    {
        return;
    }

    if (updates == 0)
    {
        DisablePlayer(index);

        return;
    }

    player.state = 2;
    player.fade_updates = static_cast<uint16_t>(updates);
    player.fade_step = -(player.fade_volume / static_cast<float>(updates));
}

void EadMiddlePlayer::SetVolumeScale(int index, float scale)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.fade_scale = scale;
    player.recalculate_volume = true;
}

void EadMiddlePlayer::SetTempo(int index, int beats_per_minute)
{
    players_[static_cast<std::size_t>(index)].tempo = static_cast<uint16_t>(beats_per_minute * kTicksPerBeat);
}

void EadMiddlePlayer::SetTranspose(int index, int8_t transpose)
{
    players_[static_cast<std::size_t>(index)].transpose = transpose;
}

void EadMiddlePlayer::SetChannelVolumeScale(int player, int channel, float scale)
{
    if (Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr)
    {
        held->volume_scale = scale;
        held->change_volume = true;
    }
}

void EadMiddlePlayer::SetChannelVolume(int player, int channel, float volume)
{
    if (Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr)
    {
        held->volume = volume;
        held->change_volume = true;
    }
}

void EadMiddlePlayer::SetChannelPan(int player, int channel, uint8_t pan)
{
    if (Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr)
    {
        held->new_pan = pan;
        held->change_pan = true;
    }
}

void EadMiddlePlayer::SetChannelFrequency(int player, int channel, float scale)
{
    if (Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr)
    {
        held->frequency_scale = scale;
        held->change_frequency = true;
    }
}

void EadMiddlePlayer::SetChannelReverb(int player, int channel, uint8_t reverb)
{
    if (Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr)
    {
        held->reverb = reverb;
    }
}

void EadMiddlePlayer::SetChannelStopNotes(int player, int channel, bool stop)
{
    if (Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr)
    {
        held->stop_notes = stop;
    }
}

void EadMiddlePlayer::WritePort(int player, int channel, int port, int8_t value)
{
    Channel* held = players_[static_cast<std::size_t>(player)].enabled ? ChannelOf(player, channel) : nullptr;
    if (held != nullptr && port >= 0 && port < kPorts)
    {
        held->ports[static_cast<std::size_t>(port)] = value;
    }
}

void EadMiddlePlayer::SetVariation(int player, int8_t value)
{
    players_[static_cast<std::size_t>(player)].variation = value;
}

void EadMiddlePlayer::SetMuted(bool muted)
{
    for (Player& player : players_)
    {
        player.muted = muted;
        player.recalculate_volume = true;
    }
}

void EadMiddlePlayer::SetSoundMode(EadSoundMode mode)
{
    config_.sound_mode = mode;
}

void EadMiddlePlayer::MarkBankLoaded(int bank)
{
    if (static_cast<std::size_t>(bank) < bank_loaded_.size())
    {
        bank_loaded_[static_cast<std::size_t>(bank)] = 1;
    }
}

int EadMiddlePlayer::UpdatesPerFrame() const
{
    return updates_per_frame_;
}

int EadMiddlePlayer::TempoLimit() const
{
    return max_tempo_;
}

uint64_t EadMiddlePlayer::UpdateCount() const
{
    return updates_;
}

} // namespace ntdlib::n64

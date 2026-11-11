// SPDX-License-Identifier: MIT

// The model of the early EAD engine: the sound files, and the players, channels and layers. ead_early_scripts.cpp
// contains their scripts, ead_early_voices.cpp the voices and ead_early_synthesis.cpp the audio lists.

#include "ntdlib/n64/ead_early.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ead_early_constants.h"
#include "ead_lists.h"
#include "ntdlib/n64/rom_data.h"

namespace ntdlib::n64
{

namespace
{

using ead_early_constants::kGlobalPool;
using ead_early_constants::kMuteSoften;
using ead_early_constants::kMuteStopNotes;
using ead_early_constants::kMuteStopScript;
using ead_early_constants::kPriorityDefault;
using ead_early_constants::kStateFadeTo;
using ead_early_constants::kTatumsPerBeat;
using ead_lists::WriteBe16;

constexpr int kChannelCount = 32;
constexpr int kLayerCount = 52;

constexpr int kPoolCount = 1 + EadEarlyPlayer::kPlayers + kChannelCount;

// The default envelope: up to 32000 over 4 updates, a hold, and a stay.
const EadEnvelope kDefaultEnvelope = {.offset = kNoOffset, .points = {{4, 32000}, {1000, 32000}, {-1, 0}}};

} // namespace

std::optional<EadEarlyFiles> EadEarlyFiles::Read(std::span<const uint8_t> image)
{
    const std::optional<EarlySoundData> data = FindEarlySoundData(image);
    if (!data)
    {
        return std::nullopt;
    }

    EadEarlyFiles files;
    files.sequences = SeqFile::Parse(image.subspan(data->sequences), kEarlySequenceFileRevision);
    const SeqFile banks = SeqFile::Parse(image.subspan(data->banks), kEarlyBankFileRevision);
    const SeqFile samples = SeqFile::Parse(image.subspan(data->samples), kEarlySampleFileRevision);
    files.banks.reserve(banks.Entries().size());
    for (std::size_t i = 0; i < banks.Entries().size(); i++)
    {
        files.banks.push_back(EadBank::ParseEarly(banks.Sequence(i)));
        const uint32_t offset = i < samples.Entries().size() ? samples.Entries()[i].offset : 0;
        files.sample_places.push_back(static_cast<uint32_t>(data->samples) + offset);
    }

    if (data->bank_sets)
    {
        files.bank_sets = ParseEarlyBankSets(image.subspan(*data->bank_sets), files.sequences.Entries().size());
    }

    return files;
}

EadEarlyPlayer::EadEarlyPlayer(const EadEarlyConfig& config, const EadEarlyFiles& files, AudioMemory& memory)
    : config_(config), files_(&files)
{
    // A frame's target is the rate's share of a 60th of a second, on a multiple of 16 samples.
    const int frame_target = (config.ai_frequency / 60 + 15) & ~15;
    updates_per_frame_ = frame_target / 160 + 1;
    tempo_limit_ = static_cast<int16_t>(static_cast<uint32_t>(static_cast<float>(updates_per_frame_) * 2880000.0F /
                                                              static_cast<float>(kTatumsPerBeat) / 16.713F));

    channels_.resize(kChannelCount);
    layers_.resize(kLayerCount);
    layer_is_free_.assign(kLayerCount, 1);
    free_layers_.reserve(kLayerCount);
    for (int i = 0; i < kLayerCount; i++)
    {
        free_layers_.push_back(i);
    }

    InitVoices(memory);
    WriteBooksAndLoops(memory);

    if (config.reverb_window != 0)
    {
        reverb_left_ = memory.Allocate(static_cast<std::size_t>(config.reverb_window) * 2);
        reverb_right_ = memory.Allocate(static_cast<std::size_t>(config.reverb_window) * 2);
    }

    for (int i = 0; i < kPlayers; i++)
    {
        players_[static_cast<std::size_t>(i)].channels.fill(-1);
        ResetPlayer(i);
    }
}

// Sets up the voice lists, allocates the buffers of each voice, and puts every voice on the disabled list of the
// global pool.
void EadEarlyPlayer::InitVoices(AudioMemory& memory)
{
    voices_.resize(static_cast<std::size_t>(config_.voices));
    const std::size_t nodes = voices_.size() + kPoolCount * 4;
    next_.assign(nodes, -1);
    previous_.assign(nodes, -1);
    for (std::size_t node = voices_.size(); node < nodes; node++)
    {
        next_[node] = static_cast<int>(node);
        previous_[node] = static_cast<int>(node);
    }

    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        Voice& voice = voices_[i];
        voice.buffers.adpcm_state = memory.Allocate(32);
        voice.buffers.final_resample = memory.Allocate(32);
        voice.buffers.mix_state = memory.Allocate(80);
        voice.buffers.pan_resample = memory.Allocate(32);
        voice.buffers.pan_samples = memory.Allocate(64);
        voice.buffers.dummy_resample = memory.Allocate(32);
        voice.buffers.samples = memory.Allocate(128);
        voice.pool = kGlobalPool;
        PushBack(Head(kGlobalPool, kDisabled), static_cast<int>(i));
    }
}

// Writes the codebooks and loop states of every bank to the model's memory, and records their addresses.
void EadEarlyPlayer::WriteBooksAndLoops(AudioMemory& memory)
{
    for (const EadBank& bank : files_->banks)
    {
        std::vector<uint32_t> books;
        books.reserve(bank.books.size());
        for (const BankBook& book : bank.books)
        {
            const std::size_t size = book.book.coefficients.size() * 2;
            const uint32_t address = memory.Allocate(std::max<std::size_t>(size, 16));
            for (std::size_t i = 0; i < book.book.coefficients.size(); i++)
            {
                WriteBe16(memory, address + static_cast<uint32_t>(i) * 2, book.book.coefficients[i]);
            }

            books.push_back(address);
        }

        book_addresses_.push_back(std::move(books));
        std::vector<uint32_t> loops;
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

        loop_addresses_.push_back(std::move(loops));
    }
}

void EadEarlyPlayer::ResetPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.muted = false;
    player.delay = 0;
    player.state = 0;
    player.fade_updates = 0;
    player.tempo_count = 0;
    player.tempo = 120 * kTatumsPerBeat;
    player.transpose = 0;
    player.mute_behavior = kMuteStopScript | kMuteStopNotes | kMuteSoften;
    player.policy = 0;
    player.short_velocities = -1;
    player.short_gates = -1;
    player.fade_volume = 1.0F;
    player.fade_step = 0.0F;
    player.volume = 0.0F;
    player.mute_scale = 0.5F;
}

void EadEarlyPlayer::Start(int index, int sequence)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= files_->sequences.Entries().size())
    {
        return;
    }

    DisablePlayer(index);
    const auto set = static_cast<std::size_t>(sequence);
    if (set < files_->bank_sets.size() && !files_->bank_sets[set].empty())
    {
        player.default_bank = files_->bank_sets[set].back();
    }

    player.sequence = static_cast<uint8_t>(sequence);
    ResetPlayer(index);
    const std::span<const uint8_t> bytes = files_->sequences.Sequence(set);
    player.bytes.assign(bytes.begin(), bytes.end());
    player.script = {};
    player.delay = 0;
    player.enabled = true;
}

void EadEarlyPlayer::Stop(int player)
{
    DisablePlayer(player);
}

bool EadEarlyPlayer::Enabled(int player) const
{
    return players_[static_cast<std::size_t>(player)].enabled;
}

void EadEarlyPlayer::SetFadeVolume(int index, float volume)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.fade_volume = volume;
    player.fade_updates = 0;
    player.state = 0;
}

void EadEarlyPlayer::FadeTo(int index, float volume, int updates)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (updates <= 0)
    {
        SetFadeVolume(index, volume);
        return;
    }

    player.state = kStateFadeTo;
    player.fade_updates = static_cast<uint16_t>(updates);
    player.fade_step = (volume - player.fade_volume) / static_cast<float>(updates);
}

void EadEarlyPlayer::WritePort(int index, int channel, int port, int8_t value)
{
    const int slot = players_[static_cast<std::size_t>(index)].channels[static_cast<std::size_t>(channel)];
    if (slot >= 0)
    {
        channels_[static_cast<std::size_t>(slot)].ports[static_cast<std::size_t>(port)] = value;
    }
}

void EadEarlyPlayer::SetVariation(int index, int8_t value)
{
    players_[static_cast<std::size_t>(index)].variation = value;
}

int EadEarlyPlayer::UpdatesPerFrame() const
{
    return updates_per_frame_;
}

int EadEarlyPlayer::TempoLimit() const
{
    return tempo_limit_;
}

uint64_t EadEarlyPlayer::UpdateCount() const
{
    return updates_;
}

void EadEarlyPlayer::DisablePlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    DisableChannels(index, 0xFFFF);
    ClearPool(PlayerPool(index));
    player.enabled = false;
}

int EadEarlyPlayer::AllocateChannel() const
{
    for (int i = 0; i < kChannelCount; i++)
    {
        if (channels_[static_cast<std::size_t>(i)].player == -1)
        {
            return i;
        }
    }

    return -1;
}

void EadEarlyPlayer::InitChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel.enabled = false;
    channel.finished = false;
    channel.script_stopped = false;
    channel.has_instrument = false;
    channel.stereo_effects = false;
    channel.transpose = 0;
    channel.large_notes = false;
    channel.script.depth = 0;
    channel.volume = 1.0F;
    channel.volume_scale = 1.0F;
    channel.frequency_scale = 1.0F;
    channel.pan = 0.5F;
    channel.pan_weight = 1.0F;
    channel.reverb = 0;
    channel.priority = kPriorityDefault;
    channel.delay = 0;
    channel.adsr.envelope = {.bank = &kDefaultEnvelope};
    channel.adsr.release = 0x20;
    channel.adsr.sustain = 0;
    channel.vibrato_rate_target = 0x800;
    channel.vibrato_rate_start = 0x800;
    channel.vibrato_extent_target = 0;
    channel.vibrato_extent_start = 0;
    channel.vibrato_rate_delay = 0;
    channel.vibrato_extent_delay = 0;
    channel.vibrato_delay = 0;
    channel.ports.fill(-1);
}

void EadEarlyPlayer::InitChannels(int index, uint16_t mask)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kChannelsPerPlayer; i++)
    {
        if ((mask & (1U << i)) == 0)
        {
            continue;
        }

        int& slot = player.channels[static_cast<std::size_t>(i)];
        if (slot >= 0 && channels_[static_cast<std::size_t>(slot)].player == index)
        {
            DisableChannel(slot);
            channels_[static_cast<std::size_t>(slot)].player = -1;
        }

        const int free_channel = AllocateChannel();
        slot = free_channel;
        if (free_channel >= 0)
        {
            InitChannel(free_channel);
            Channel& channel = channels_[static_cast<std::size_t>(free_channel)];
            channel.player = index;
            channel.bank = player.default_bank;
            channel.mute_behavior = player.mute_behavior;
            channel.policy = player.policy;
        }
    }
}

void EadEarlyPlayer::DisableChannels(int index, uint16_t mask)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kChannelsPerPlayer; i++)
    {
        if ((mask & (1U << i)) == 0)
        {
            continue;
        }

        int& slot = player.channels[static_cast<std::size_t>(i)];
        if (slot >= 0)
        {
            if (channels_[static_cast<std::size_t>(slot)].player == index)
            {
                DisableChannel(slot);
                channels_[static_cast<std::size_t>(slot)].player = -1;
            }

            slot = -1;
        }
    }
}

void EadEarlyPlayer::EnableChannel(int index, int slot, uint32_t script)
{
    const int channel_index = players_[static_cast<std::size_t>(index)].channels[static_cast<std::size_t>(slot)];
    if (channel_index < 0)
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
        if (channel.layers[static_cast<std::size_t>(i)] != -1)
        {
            FreeLayer(channel_index, i);
        }
    }
}

void EadEarlyPlayer::DisableChannel(int index)
{
    for (int i = 0; i < kLayersPerChannel; i++)
    {
        FreeLayer(index, i);
    }

    ClearPool(ChannelPool(index));
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel.enabled = false;
    channel.finished = true;
}

int EadEarlyPlayer::SetLayer(int channel_index, int slot)
{
    Channel& channel = channels_[static_cast<std::size_t>(channel_index)];
    int& layer_index = channel.layers[static_cast<std::size_t>(slot)];
    if (layer_index == -1)
    {
        if (free_layers_.empty())
        {
            return -1;
        }

        layer_index = free_layers_.back();
        free_layers_.pop_back();
        layer_is_free_[static_cast<std::size_t>(layer_index)] = 0;
    }
    else
    {
        DecayOrRelease(layer_index, AdsrPhase::kDecay);
    }

    Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
    layer.channel = channel_index;
    layer.adsr = channel.adsr;
    layer.adsr.release = 0;
    layer.enabled = true;
    layer.silent = false;
    layer.continuous = false;
    layer.finished = false;
    layer.portamento.mode = 0;
    layer.script.depth = 0;
    layer.loaded = false;
    layer.gate = 0x80;
    layer.transpose = 0;
    layer.delay = 0;
    layer.duration = 0;
    layer.voice = -1;
    layer.instrument = nullptr;
    layer.velocity_square = 0.0F;
    layer.pan = 0.5F;

    return 0;
}

void EadEarlyPlayer::DisableLayer(int index)
{
    if (index >= 0)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
        Layer& layer = layers_[static_cast<std::size_t>(index)];
        layer.enabled = false;
        layer.finished = true;
    }
}

void EadEarlyPlayer::FreeLayer(int channel_index, int slot)
{
    Channel& channel = channels_[static_cast<std::size_t>(channel_index)];
    const int index = channel.layers[static_cast<std::size_t>(slot)];
    if (index != -1)
    {
        if (layer_is_free_[static_cast<std::size_t>(index)] == 0)
        {
            free_layers_.push_back(index);
            layer_is_free_[static_cast<std::size_t>(index)] = 1;
        }

        DisableLayer(index);
        channel.layers[static_cast<std::size_t>(slot)] = -1;
    }
}

int EadEarlyPlayer::FindInstrument(const Channel& channel, uint8_t program, const EadInstrument*& out,
                                   AdsrSettings& adsr) const
{
    const EadBank& bank = files_->banks[channel.bank];
    const auto count = static_cast<int>(bank.program_instruments.size());
    int id = program;
    if (id >= count)
    {
        id = count;
        if (id == 0)
        {
            return 0;
        }

        id--;
    }

    // An empty program takes the nearest one below it.
    int object = bank.program_instruments[static_cast<std::size_t>(id)];
    while (object == kNoObject && id > 0)
    {
        id--;
        object = bank.program_instruments[static_cast<std::size_t>(id)];
    }

    if (object == kNoObject)
    {
        out = nullptr;

        return 0;
    }

    const EadInstrument& instrument = bank.instruments[static_cast<std::size_t>(object)];
    adsr.envelope = {};
    if (instrument.envelope != kNoObject)
    {
        adsr.envelope.bank = &bank.envelopes[static_cast<std::size_t>(instrument.envelope)];
    }

    adsr.release = instrument.release_rate;
    out = &instrument;

    return id + 1;
}

void EadEarlyPlayer::SetInstrument(int index, uint8_t program)
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
        channel.program = static_cast<int16_t>(FindInstrument(channel, program, channel.instrument, channel.adsr));
        if (channel.program == 0)
        {
            channel.has_instrument = false;
            return;
        }
    }

    channel.has_instrument = true;
}

} // namespace ntdlib::n64

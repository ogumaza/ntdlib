// SPDX-License-Identifier: MIT

#include "ntdlib/n64/ead_early.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "ead_tables.h"
#include "ntdlib/n64/rom_data.h"

namespace ntdlib::n64
{

namespace
{

using namespace ead_tables;

// DMEM's buffers in the engine's commands.
constexpr uint32_t kTemp = 0x0;
constexpr uint32_t kResampled = 0x20;
constexpr uint32_t kResampled2 = 0x160;
constexpr uint32_t kDecoded = 0x180;
constexpr uint32_t kPanTemp = 0x200;
constexpr uint32_t kStrongDry = 0x200;
constexpr uint32_t kStrongWet = 0x340;
constexpr uint32_t kCompressed = 0x3F0;
constexpr uint32_t kLeft = 0x4C0;
constexpr uint32_t kRight = 0x600;
constexpr uint32_t kWetLeft = 0x740;
constexpr uint32_t kWetRight = 0x880;
constexpr uint32_t kOneChannel = 0x140;
constexpr uint32_t kTwoChannels = 0x280;

constexpr uint8_t kFlagInit = 0x01;
constexpr uint8_t kFlagLoop = 0x02;
constexpr uint8_t kFlagLeft = 0x02;
constexpr uint8_t kFlagVolume = 0x04;
constexpr uint8_t kFlagAux = 0x08;

constexpr uint8_t kMuteStopScript = 0x80;
constexpr uint8_t kMuteStopNotes = 0x40;
constexpr uint8_t kMuteSoften = 0x20;

constexpr uint8_t kPriorityDisabled = 0;
constexpr uint8_t kPriorityStopping = 1;
constexpr uint8_t kPriorityMinimum = 2;
constexpr uint8_t kPriorityDefault = 3;

constexpr uint8_t kActionDecay = 0x20;
constexpr uint8_t kActionRelease = 0x10;

constexpr uint8_t kPolicyLayer = 1;
constexpr uint8_t kPolicyChannel = 2;
constexpr uint8_t kPolicySequence = 4;
constexpr uint8_t kPolicyGlobal = 8;

constexpr uint8_t kStateFadeOut = 1;
constexpr uint8_t kStateFadeIn = 2;
constexpr uint8_t kStateFadeTo = 3;
constexpr uint8_t kStateFadeStay = 4;

constexpr int kChannelCount = 32;
constexpr int kLayerCount = 52;
constexpr int kTatumsPerBeat = 48;

constexpr int kGlobalPool = 0;
constexpr int kPoolCount = 1 + EadEarlyPlayer::kPlayers + kChannelCount;

// The default envelope: up to 32000 over 4 updates, a hold, and a stay.
const EadEnvelope kDefaultEnvelope = {.offset = kNoOffset, .points = {{4, 32000}, {1000, 32000}, {-1, 0}}};

constexpr int Align(int value, int bits)
{
    return (value + (1 << bits) - 1) & ~((1 << bits) - 1);
}

constexpr uint8_t PortamentoMode(uint8_t mode)
{
    return mode & 0x7F;
}

void WriteBe16(AudioMemory& memory, uint32_t address, int16_t value)
{
    const std::array<uint8_t, 2> bytes = {static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8),
                                          static_cast<uint8_t>(value)};
    memory.Write(address, bytes.data(), bytes.size());
}

float RampValue(uint16_t source, uint16_t target, int samples)
{
    const std::size_t from = source >> 8;
    const std::size_t to = target >> 8;
    switch (samples)
    {
    case 128:
        return kRampTarget128[to & 127] * kRampSource128[from & 127];
    case 144:
        return kRampTarget144[to & 127] * kRampSource144[from & 127];
    default:
        return kRampTarget136[to & 127] * kRampSource136[from & 127];
    }
}

// A portamento's scale of the frequency, at a place from 0 to 127 in the engine's table of bends.
float PortamentoScale(float extent, float position)
{
    uint32_t index = static_cast<uint32_t>(position);
    if (index >= 127)
    {
        index = 127;
    }

    return 1.0F + extent * (kBendScale[index + 127] - 1.0F);
}

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

    voices_.resize(static_cast<std::size_t>(config.voices));
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

    for (const EadBank& bank : files.banks)
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

uint8_t EadEarlyPlayer::PeekByte(const Player& player, uint32_t place) const
{
    return place < player.bytes.size() ? player.bytes[place] : uint8_t{0};
}

uint8_t EadEarlyPlayer::Read8(const Player& player, Script& script) const
{
    return PeekByte(player, script.pc++);
}

uint16_t EadEarlyPlayer::Read16(const Player& player, Script& script) const
{
    const uint8_t high = Read8(player, script);
    const uint8_t low = Read8(player, script);

    return static_cast<uint16_t>((high << 8) | low);
}

uint16_t EadEarlyPlayer::ReadVariable(const Player& player, Script& script) const
{
    uint16_t value = Read8(player, script);
    if ((value & 0x80) != 0)
    {
        value = static_cast<uint16_t>((value << 8) & 0x7F00);
        value = static_cast<uint16_t>(value | Read8(player, script));
    }

    return value;
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

void EadEarlyPlayer::RunPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (!player.enabled)
    {
        return;
    }

    if (player.muted && (player.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count + player.tempo);
    if (player.tempo_count < tempo_limit_)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count - static_cast<uint16_t>(tempo_limit_));

    Script& script = player.script;
    int32_t value = 0;
    if (player.delay > 1)
    {
        player.delay--;
    }
    else
    {
        while (true)
        {
            const uint8_t command = Read8(player, script);
            if (command == 0xFF)
            {
                if (script.depth == 0)
                {
                    DisablePlayer(index);
                    break;
                }

                script.depth--;
                script.pc = script.stack[script.depth & 3];
            }

            if (command == 0xFD)
            {
                player.delay = ReadVariable(player, script);
                break;
            }

            if (command == 0xFE)
            {
                player.delay = 1;
                break;
            }

            if (command >= 0xC0)
            {
                switch (command)
                {
                case 0xFC:
                    {
                        const uint16_t target = Read16(player, script);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = target;
                        break;
                    }
                case 0xF8:
                    script.loops[script.depth & 3] = Read8(player, script);
                    script.stack[script.depth & 3] = script.pc;
                    script.depth++;
                    break;
                case 0xF7:
                    {
                        uint8_t& loops = script.loops[(script.depth - 1) & 3];
                        loops--;
                        if (loops != 0)
                        {
                            script.pc = script.stack[(script.depth - 1) & 3];
                        }
                        else
                        {
                            script.depth--;
                        }

                        break;
                    }
                case 0xFB:
                case 0xFA:
                case 0xF9:
                case 0xF5:
                    {
                        const uint16_t target = Read16(player, script);
                        if ((command == 0xFA && value != 0) || (command == 0xF9 && value >= 0) ||
                            (command == 0xF5 && value < 0))
                        {
                            break;
                        }

                        script.pc = target;
                        break;
                    }
                case 0xF2:
                    ClearPool(PlayerPool(index));
                    FillPool(PlayerPool(index), Read8(player, script));
                    break;
                case 0xF1:
                    ClearPool(PlayerPool(index));
                    break;
                case 0xDF:
                    player.transpose = static_cast<int16_t>(static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xDE:
                    player.transpose =
                        static_cast<int16_t>(player.transpose + static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xDD:
                case 0xDC:
                    {
                        const uint8_t amount = Read8(player, script);
                        if (command == 0xDD)
                        {
                            player.tempo = static_cast<uint16_t>(amount * kTatumsPerBeat);
                        }
                        else
                        {
                            player.tempo =
                                static_cast<uint16_t>(player.tempo + static_cast<int8_t>(amount) * kTatumsPerBeat);
                        }

                        if (player.tempo > tempo_limit_)
                        {
                            player.tempo = static_cast<uint16_t>(tempo_limit_);
                        }

                        if (static_cast<int16_t>(player.tempo) <= 0)
                        {
                            player.tempo = 1;
                        }

                        break;
                    }
                case 0xDB:
                    {
                        const float target = static_cast<float>(Read8(player, script)) / 127.0F;
                        switch (player.state)
                        {
                        case kStateFadeIn:
                            if (player.fade_updates != 0)
                            {
                                player.fade_step = (target - player.fade_volume) /
                                                   static_cast<float>(static_cast<int32_t>(player.fade_updates));
                                break;
                            }

                            player.fade_volume = target;
                            break;
                        case 0:
                            player.fade_volume = target;
                            break;
                        case kStateFadeOut:
                        case kStateFadeStay:
                            player.volume = target;
                            break;
                        default:
                            break;
                        }

                        break;
                    }
                case 0xDA:
                    player.fade_volume += static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
                    break;
                case 0xD7:
                    InitChannels(index, Read16(player, script));
                    break;
                case 0xD6:
                    DisableChannels(index, Read16(player, script));
                    break;
                case 0xD5:
                    player.mute_scale = static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
                    break;
                case 0xD4:
                    player.muted = true;
                    break;
                case 0xD3:
                    player.mute_behavior = Read8(player, script);
                    break;
                case 0xD2:
                    player.short_velocities = Read16(player, script);
                    break;
                case 0xD1:
                    player.short_gates = Read16(player, script);
                    break;
                case 0xD0:
                    player.policy = Read8(player, script);
                    break;
                case 0xCC:
                    value = Read8(player, script);
                    break;
                case 0xC9:
                    value = Read8(player, script) & value;
                    break;
                case 0xC8:
                    value = value - Read8(player, script);
                    break;
                default:
                    break;
                }
            }
            else
            {
                const int low = command & 0xF;
                switch (command & 0xF0)
                {
                case 0x00:
                    {
                        const int slot = player.channels[static_cast<std::size_t>(low)];
                        if (slot >= 0)
                        {
                            value = channels_[static_cast<std::size_t>(slot)].finished ? 1 : 0;
                        }

                        break;
                    }
                case 0x50:
                    value -= player.variation;
                    break;
                case 0x70:
                    player.variation = static_cast<int8_t>(value);
                    break;
                case 0x80:
                    value = player.variation;
                    break;
                case 0x90:
                    EnableChannel(index, low, Read16(player, script));
                    break;
                default:
                    break;
                }
            }
        }
    }

    for (int i = 0; i < kChannelsPerPlayer; i++)
    {
        const int slot = player.channels[static_cast<std::size_t>(i)];
        if (slot >= 0)
        {
            RunChannel(slot);
        }
    }
}

void EadEarlyPlayer::RunChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.enabled)
    {
        return;
    }

    const auto run_layers = [this, &channel]()
    {
        for (int i = 0; i < kLayersPerChannel; i++)
        {
            const int layer = channel.layers[static_cast<std::size_t>(i)];
            if (layer != -1)
            {
                RunLayer(layer);
            }
        }
    };

    if (channel.script_stopped)
    {
        run_layers();
        return;
    }

    const int player_index = channel.player;
    Player& player = players_[static_cast<std::size_t>(player_index)];
    if (player.muted && (channel.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    if (channel.delay != 0)
    {
        channel.delay--;
    }

    Script& script = channel.script;
    int8_t value = 0;
    if (channel.delay == 0)
    {
        while (true)
        {
            const uint8_t command = Read8(player, script);
            if (command == 0xFF)
            {
                if (script.depth == 0)
                {
                    DisableChannel(index);
                    break;
                }

                script.depth--;
                script.pc = script.stack[script.depth & 3];
            }

            if (command == 0xFE)
            {
                break;
            }

            if (command == 0xFD)
            {
                channel.delay = ReadVariable(player, script);
                break;
            }

            if (command == 0xF3)
            {
                channel.script_stopped = true;
                break;
            }

            if (command > 0xC0)
            {
                switch (command)
                {
                case 0xFC:
                    {
                        const uint16_t target = Read16(player, script);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = target;
                        break;
                    }
                case 0xF8:
                    script.loops[script.depth & 3] = Read8(player, script);
                    script.stack[script.depth & 3] = script.pc;
                    script.depth++;
                    break;
                case 0xF7:
                    {
                        uint8_t& loops = script.loops[(script.depth - 1) & 3];
                        loops--;
                        if (loops != 0)
                        {
                            script.pc = script.stack[(script.depth - 1) & 3];
                        }
                        else
                        {
                            script.depth--;
                        }

                        break;
                    }
                case 0xF6:
                    script.depth--;
                    break;
                case 0xFB:
                case 0xFA:
                case 0xF9:
                case 0xF5:
                    {
                        const uint16_t target = Read16(player, script);
                        if ((command == 0xFA && value != 0) || (command == 0xF9 && value >= 0) ||
                            (command == 0xF5 && value < 0))
                        {
                            break;
                        }

                        script.pc = target;
                        break;
                    }
                case 0xF2:
                    ClearPool(ChannelPool(index));
                    FillPool(ChannelPool(index), Read8(player, script));
                    break;
                case 0xF1:
                    ClearPool(ChannelPool(index));
                    break;
                case 0xC2:
                    channel.dynamic_table = Read16(player, script);
                    break;
                case 0xC5:
                    if (value != -1)
                    {
                        const uint32_t entry = channel.dynamic_table + static_cast<uint32_t>(int32_t{value} * 2);
                        channel.dynamic_table =
                            static_cast<uint16_t>((PeekByte(player, entry) << 8) + PeekByte(player, entry + 1));
                    }

                    break;
                case 0xC1:
                    SetInstrument(index, Read8(player, script));
                    break;
                case 0xC3:
                    channel.large_notes = false;
                    break;
                case 0xC4:
                    channel.large_notes = true;
                    break;
                case 0xDF:
                    channel.volume = static_cast<float>(Read8(player, script)) / 127.0F;
                    break;
                case 0xE0:
                    channel.volume_scale = static_cast<float>(Read8(player, script)) / 128.0F;
                    break;
                case 0xDE:
                    channel.frequency_scale =
                        static_cast<float>(static_cast<int32_t>(Read16(player, script))) / 32768.0F;
                    break;
                case 0xD3:
                    {
                        const auto bend = static_cast<uint8_t>(Read8(player, script) + 127);
                        channel.frequency_scale = bend < kBendScale.size() ? kBendScale[bend] : kNoteFrequencies[0];
                        break;
                    }
                case 0xDD:
                    channel.pan = static_cast<float>(Read8(player, script)) / 128.0F;
                    break;
                case 0xDC:
                    channel.pan_weight = static_cast<float>(Read8(player, script)) / 128.0F;
                    break;
                case 0xDB:
                    channel.transpose = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xDA:
                    channel.adsr.envelope = {.player = player_index, .offset = Read16(player, script)};
                    break;
                case 0xD9:
                    channel.adsr.release = Read8(player, script);
                    break;
                case 0xD8:
                    channel.vibrato_extent_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_extent_start = 0;
                    channel.vibrato_extent_delay = 0;
                    break;
                case 0xD7:
                    {
                        const auto rate = static_cast<uint16_t>(Read8(player, script) * 32);
                        channel.vibrato_rate_start = rate;
                        channel.vibrato_rate_target = rate;
                        channel.vibrato_rate_delay = 0;
                        break;
                    }
                case 0xE2:
                    channel.vibrato_extent_start = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_extent_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_extent_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE1:
                    channel.vibrato_rate_start = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_target = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE3:
                    channel.vibrato_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xD6:
                    Read8(player, script);
                    break;
                case 0xD4:
                    channel.reverb = Read8(player, script);
                    break;
                case 0xC6:
                    {
                        const uint8_t from_back = Read8(player, script);
                        const std::vector<uint8_t>& set = files_->bank_sets[player.sequence];
                        if (from_back < set.size())
                        {
                            channel.bank = set[set.size() - 1 - from_back];
                        }

                        break;
                    }
                case 0xC7:
                    {
                        const uint8_t add = Read8(player, script);
                        const uint16_t place = Read16(player, script);
                        if (place < player.bytes.size())
                        {
                            player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(value) + add);
                        }

                        break;
                    }
                case 0xC8:
                    value = static_cast<int8_t>(value - static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xC9:
                    value = static_cast<int8_t>(value & static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xCC:
                    value = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xCA:
                    channel.mute_behavior = Read8(player, script);
                    break;
                case 0xCB:
                    {
                        const int32_t place = int32_t{Read16(player, script)} + value;
                        value = static_cast<int8_t>(PeekByte(player, static_cast<uint32_t>(place)));
                        break;
                    }
                case 0xD0:
                    channel.stereo_effects = Read8(player, script) != 0;
                    break;
                case 0xD1:
                    channel.policy = Read8(player, script);
                    break;
                case 0xD2:
                    channel.adsr.sustain = static_cast<uint16_t>(Read8(player, script) << 8);
                    break;
                case 0xE4:
                    if (value != -1)
                    {
                        const uint32_t entry = channel.dynamic_table + static_cast<uint32_t>(int32_t{value} * 2);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = static_cast<uint16_t>((PeekByte(player, entry) << 8) + PeekByte(player, entry + 1));
                    }

                    break;
                default:
                    break;
                }
            }
            else
            {
                const int low = command & 0xF;
                const auto layer_slot = static_cast<std::size_t>(low & 3);
                const bool real_slot = low < kLayersPerChannel;
                switch (command & 0xF0)
                {
                case 0x00:
                    if (real_slot && channel.layers[layer_slot] != -1)
                    {
                        value = layers_[static_cast<std::size_t>(channel.layers[layer_slot])].finished ? 1 : 0;
                    }

                    break;
                case 0x70:
                    channel.ports[static_cast<std::size_t>(low & 7)] = value;
                    break;
                case 0x80:
                    value = channel.ports[static_cast<std::size_t>(low & 7)];
                    if (low < 4)
                    {
                        channel.ports[static_cast<std::size_t>(low)] = -1;
                    }

                    break;
                case 0x50:
                    value = static_cast<int8_t>(value - channel.ports[static_cast<std::size_t>(low & 7)]);
                    break;
                case 0x90:
                    {
                        const uint16_t target = Read16(player, script);
                        if (real_slot && SetLayer(index, low) == 0)
                        {
                            layers_[static_cast<std::size_t>(channel.layers[layer_slot])].script.pc = target;
                        }

                        break;
                    }
                case 0xA0:
                    if (real_slot)
                    {
                        FreeLayer(index, low);
                    }

                    break;
                case 0xB0:
                    if (value != -1 && real_slot && SetLayer(index, low) != -1)
                    {
                        const uint32_t entry = channel.dynamic_table + static_cast<uint32_t>(int32_t{value} * 2);
                        layers_[static_cast<std::size_t>(channel.layers[layer_slot])].script.pc =
                            static_cast<uint16_t>((PeekByte(player, entry) << 8) + PeekByte(player, entry + 1));
                    }

                    break;
                case 0x60:
                    channel.priority = static_cast<uint8_t>(low);
                    break;
                case 0x10:
                    EnableChannel(player_index, low, Read16(player, script));
                    break;
                case 0x20:
                    {
                        const int other = player.channels[static_cast<std::size_t>(low)];
                        if (other >= 0)
                        {
                            DisableChannel(other);
                        }

                        break;
                    }
                case 0x30:
                    {
                        const uint8_t port = Read8(player, script);
                        const int other = player.channels[static_cast<std::size_t>(low)];
                        if (other >= 0)
                        {
                            channels_[static_cast<std::size_t>(other)].ports[port & 7] = value;
                        }

                        break;
                    }
                case 0x40:
                    {
                        const uint8_t port = Read8(player, script);
                        const int other = player.channels[static_cast<std::size_t>(low)];
                        if (other >= 0)
                        {
                            value = channels_[static_cast<std::size_t>(other)].ports[port & 7];
                        }

                        break;
                    }
                default:
                    break;
                }
            }
        }
    }

    run_layers();
}

void EadEarlyPlayer::RunLayer(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (!layer.enabled)
    {
        return;
    }

    if (layer.delay > 1)
    {
        layer.delay--;
        if (!layer.silent && layer.delay <= layer.duration)
        {
            DecayOrRelease(index, AdsrPhase::kDecay);
            layer.silent = true;
        }

        return;
    }

    if (!layer.continuous)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }

    if (PortamentoMode(layer.portamento.mode) == 1 || PortamentoMode(layer.portamento.mode) == 2)
    {
        layer.portamento.mode = 0;
    }

    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    bool same_sound = true;
    uint8_t command = 0;
    uint32_t command_place = 0;
    while (true)
    {
        command_place = script.pc;
        command = Read8(player, script);
        if (command <= 0xC0)
        {
            break;
        }

        switch (command)
        {
        case 0xFF:
            if (script.depth == 0)
            {
                DisableLayer(index);
                return;
            }

            script.depth--;
            script.pc = script.stack[script.depth & 3];
            break;
        case 0xFC:
            {
                const uint16_t target = Read16(player, script);
                script.stack[script.depth & 3] = script.pc;
                script.depth++;
                script.pc = target;
                break;
            }
        case 0xF8:
            script.loops[script.depth & 3] = Read8(player, script);
            script.stack[script.depth & 3] = script.pc;
            script.depth++;
            break;
        case 0xF7:
            {
                uint8_t& loops = script.loops[(script.depth - 1) & 3];
                loops--;
                if (loops != 0)
                {
                    script.pc = script.stack[(script.depth - 1) & 3];
                }
                else
                {
                    script.depth--;
                }

                break;
            }
        case 0xFB:
            script.pc = Read16(player, script);
            break;
        case 0xC1:
            {
                const uint8_t velocity = Read8(player, script);
                layer.velocity_square = static_cast<float>(int32_t{velocity} * velocity);
                break;
            }
        case 0xCA:
            layer.pan = static_cast<float>(Read8(player, script)) / 128.0F;
            break;
        case 0xC2:
            layer.transpose = static_cast<int16_t>(Read8(player, script));
            break;
        case 0xC9:
            layer.gate = Read8(player, script);
            break;
        case 0xC4:
        case 0xC5:
            layer.continuous = command == 0xC4;
            DecayOrRelease(index, AdsrPhase::kDecay);
            break;
        case 0xC3:
            layer.default_length = static_cast<int16_t>(ReadVariable(player, script));
            break;
        case 0xC6:
            {
                const uint8_t program = Read8(player, script);
                if (program < 127)
                {
                    FindInstrument(channel, program, layer.instrument, layer.adsr);
                }

                break;
            }
        case 0xC7:
            {
                layer.portamento.mode = Read8(player, script);
                auto target = static_cast<uint8_t>(Read8(player, script) + channel.transpose + layer.transpose +
                                                   player.transpose);
                if (target >= 0x80)
                {
                    target = 0;
                }

                layer.portamento_target = target;
                if ((layer.portamento.mode & 0x80) != 0)
                {
                    layer.portamento_time = Read8(player, script);
                }
                else
                {
                    layer.portamento_time = ReadVariable(player, script);
                }

                break;
            }
        case 0xC8:
            layer.portamento.mode = 0;
            break;
        default:
            if ((command & 0xF0) == 0xD0)
            {
                const auto from_table = static_cast<std::size_t>(command & 0xF);
                const int velocity = player.short_velocities >= 0
                                         ? PeekByte(player, static_cast<uint32_t>(player.short_velocities) +
                                                                static_cast<uint32_t>(from_table))
                                         : kShortVelocity[from_table];
                layer.velocity_square = static_cast<float>(static_cast<uint16_t>(velocity * velocity));
            }
            else if ((command & 0xF0) == 0xE0)
            {
                const auto from_table = static_cast<std::size_t>(command & 0xF);
                layer.gate = player.short_gates >= 0 ? PeekByte(player, static_cast<uint32_t>(player.short_gates) +
                                                                            static_cast<uint32_t>(from_table))
                                                     : kShortGate[from_table];
            }

            break;
        }
    }

    EadNoteStart start;
    if (command == 0xC0)
    {
        layer.delay = static_cast<int16_t>(ReadVariable(player, script));
        layer.silent = true;
    }
    else
    {
        layer.silent = false;
        uint16_t length = 0;
        if (channel.large_notes)
        {
            int velocity = 0;
            switch (command & 0xC0)
            {
            case 0x00:
                length = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                layer.last_length = static_cast<int16_t>(length);
                break;
            case 0x40:
                length = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = 0;
                layer.last_length = static_cast<int16_t>(length);
                break;
            default:
                length = static_cast<uint16_t>(layer.last_length);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                break;
            }

            layer.velocity_square = static_cast<float>(velocity * velocity);
        }
        else
        {
            switch (command & 0xC0)
            {
            case 0x00:
                length = ReadVariable(player, script);
                layer.last_length = static_cast<int16_t>(length);
                break;
            case 0x40:
                length = static_cast<uint16_t>(layer.default_length);
                break;
            default:
                length = static_cast<uint16_t>(layer.last_length);
                break;
            }
        }

        auto semitone = static_cast<uint8_t>(command - (command & 0xC0));
        layer.delay = static_cast<int16_t>(length);
        layer.duration = static_cast<int16_t>(layer.gate * int32_t{length} / 256);
        if ((player.muted && (channel.mute_behavior & kMuteStopNotes) != 0) || !channel.has_instrument)
        {
            layer.silent = true;
        }
        else if (channel.program == 0)
        {
            // A drum.
            semitone = static_cast<uint8_t>(semitone + channel.transpose + layer.transpose);
            const EadBank& bank = files_->banks[channel.bank];
            bool have_drum = true;
            if (semitone >= bank.drum_notes.size())
            {
                if (bank.drum_notes.empty())
                {
                    layer.silent = true;
                    have_drum = false;
                }
                else
                {
                    semitone = static_cast<uint8_t>(bank.drum_notes.size() - 1);
                }
            }

            if (have_drum)
            {
                const int object = bank.drum_notes[semitone];
                if (object == kNoObject)
                {
                    layer.silent = true;
                }
                else
                {
                    const EadDrum& drum = bank.drums[static_cast<std::size_t>(object)];
                    layer.adsr.envelope = {};
                    if (drum.envelope != kNoObject)
                    {
                        layer.adsr.envelope.bank = &bank.envelopes[static_cast<std::size_t>(drum.envelope)];
                    }

                    layer.adsr.release = drum.release_rate;
                    layer.pan = static_cast<float>(drum.pan) / 128.0F;
                    layer.sound = &drum.sound;
                    layer.frequency_scale = drum.sound.tuning;
                }
            }

            start.drum = true;
        }
        else
        {
            semitone = static_cast<uint8_t>(semitone + player.transpose + channel.transpose + layer.transpose);
            if (semitone >= 0x80)
            {
                layer.silent = true;
            }
            else
            {
                const EadInstrument* instrument = layer.instrument != nullptr ? layer.instrument : channel.instrument;
                const auto choose_sound = [](const EadInstrument& from, int key) -> const EadTunedSample*
                {
                    if (key < from.normal_range_low)
                    {
                        return &from.low;
                    }

                    return key <= from.normal_range_high ? &from.normal : &from.high;
                };

                if (layer.portamento.mode != 0)
                {
                    const uint8_t higher = std::max(layer.portamento_target, semitone);
                    float tuning = 1.0F;
                    if (instrument != nullptr)
                    {
                        const EadTunedSample* sound = choose_sound(*instrument, higher);
                        same_sound = sound == layer.sound;
                        layer.sound = sound;
                        tuning = sound->tuning;
                    }
                    else
                    {
                        layer.sound = nullptr;
                    }

                    const float from_note = kNoteFrequencies[semitone] * tuning;
                    const float to_note = kNoteFrequencies[layer.portamento_target] * tuning;
                    float base = 0.0F;
                    float other = 0.0F;
                    switch (PortamentoMode(layer.portamento.mode))
                    {
                    case 1:
                    case 3:
                    case 5:
                        other = from_note;
                        base = to_note;
                        break;
                    case 2:
                    case 4:
                        base = from_note;
                        other = to_note;
                        break;
                    default:
                        break;
                    }

                    layer.portamento.extent = other / base - 1.0F;
                    if ((layer.portamento.mode & 0x80) != 0)
                    {
                        layer.portamento.speed = 32512.0F * static_cast<float>(static_cast<int32_t>(player.tempo)) /
                                                 (static_cast<float>(layer.delay) * static_cast<float>(tempo_limit_) *
                                                  static_cast<float>(static_cast<int32_t>(layer.portamento_time)));
                    }
                    else
                    {
                        layer.portamento.speed =
                            127.0F / static_cast<float>(static_cast<int32_t>(layer.portamento_time));
                    }

                    layer.portamento.position = 0.0F;
                    layer.frequency_scale = base;
                    if (PortamentoMode(layer.portamento.mode) == 5)
                    {
                        layer.portamento_target = semitone;
                    }
                }
                else if (instrument != nullptr)
                {
                    const EadTunedSample* sound = choose_sound(*instrument, semitone);
                    same_sound = sound == layer.sound;
                    layer.sound = sound;
                    layer.frequency_scale = kNoteFrequencies[semitone] * sound->tuning;
                }
                else
                {
                    layer.sound = nullptr;
                    layer.frequency_scale = kNoteFrequencies[semitone];
                }
            }
        }

        start.key = semitone;
        start.length = length;
        start.gate = layer.duration;
    }

    if (layer.silent)
    {
        if (layer.voice != -1 || layer.continuous)
        {
            DecayOrRelease(index, AdsrPhase::kDecay);
        }

        return;
    }

    bool allocate = false;
    if (!layer.continuous)
    {
        allocate = true;
    }
    else if (layer.voice == -1 || !layer.loaded)
    {
        allocate = true;
    }
    else if (!same_sound)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
        allocate = true;
    }
    else if (layer.sound == nullptr)
    {
        InitWave(voices_[static_cast<std::size_t>(layer.voice)], layer);
    }

    if (allocate)
    {
        layer.voice = AllocVoice(index);
    }

    if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        InitVibrato(layer.voice);
    }

    if (layer.voice != -1)
    {
        for (int i = 0; i < kChannelsPerPlayer; i++)
        {
            if (player.channels[static_cast<std::size_t>(i)] == layer.channel)
            {
                voices_[static_cast<std::size_t>(layer.voice)].track = channel.player * kChannelsPerPlayer + i;
            }
        }
    }

    if (on_note)
    {
        start.player = channel.player;
        for (int i = 0; i < kChannelsPerPlayer; i++)
        {
            if (player.channels[static_cast<std::size_t>(i)] == layer.channel)
            {
                start.channel = i;
            }
        }

        for (int i = 0; i < kLayersPerChannel; i++)
        {
            if (channel.layers[static_cast<std::size_t>(i)] == index)
            {
                start.layer = i;
            }
        }

        start.bank = channel.bank;
        start.program = channel.program;
        start.velocity = layer.velocity_square;
        start.pan = layer.pan;
        start.offset = command_place;
        start.update = updates_;
        start.voice = layer.voice;
        on_note(start);
    }
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
    if (vibrato.extent_timer != 0)
    {
        if (vibrato.extent_timer == 1)
        {
            vibrato.extent = channel.vibrato_extent_target;
        }
        else
        {
            vibrato.extent =
                static_cast<uint16_t>(vibrato.extent + (int32_t{channel.vibrato_extent_target} - vibrato.extent) /
                                                           int32_t{vibrato.extent_timer});
        }

        vibrato.extent_timer--;
    }
    else if (channel.vibrato_extent_target != vibrato.extent)
    {
        vibrato.extent_timer = channel.vibrato_extent_delay;
        if (vibrato.extent_timer == 0)
        {
            vibrato.extent = channel.vibrato_extent_target;
        }
    }

    if (vibrato.rate_timer != 0)
    {
        if (vibrato.rate_timer == 1)
        {
            vibrato.rate = channel.vibrato_rate_target;
        }
        else
        {
            vibrato.rate = static_cast<uint16_t>(vibrato.rate + (int32_t{channel.vibrato_rate_target} - vibrato.rate) /
                                                                    int32_t{vibrato.rate_timer});
        }

        vibrato.rate_timer--;
    }
    else if (channel.vibrato_rate_target != vibrato.rate)
    {
        vibrato.rate_timer = channel.vibrato_rate_delay;
        if (vibrato.rate_timer == 0)
        {
            vibrato.rate = channel.vibrato_rate_target;
        }
    }

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
        change = kVibratoCurve[static_cast<std::size_t>(31 - wave_index)];
        break;
    case 0x00:
        change = kVibratoCurve[static_cast<std::size_t>(wave_index)];
        break;
    case 0x20:
        change = -kVibratoCurve[static_cast<std::size_t>(wave_index - 0x20)];
        break;
    default:
        change = -kVibratoCurve[static_cast<std::size_t>(63 - wave_index)];
        break;
    }

    const float extent = static_cast<float>(static_cast<int32_t>(vibrato.extent)) / 4096.0F;

    return 1.0F + extent * (kBendScale[static_cast<std::size_t>(static_cast<int8_t>(change) + 127)] - 1.0F);
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

            if (adsr.phase != AdsrPhase::kFade)
            {
                break;
            }

            [[fallthrough]];
        }
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
        adsr.current = static_cast<int16_t>(adsr.current - adsr.fade_step);
        if (adsr.sustain != 0 && adsr.phase == AdsrPhase::kDecay)
        {
            if (adsr.current < adsr.sustain)
            {
                adsr.current = adsr.sustain;
                adsr.delay = static_cast<int16_t>(adsr.sustain / 16);
                adsr.phase = AdsrPhase::kSustain;
            }

            break;
        }

        if (adsr.current < 100)
        {
            adsr.current = 0;
            adsr.phase = AdsrPhase::kDisabled;
        }

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
                  kWaves[wave][static_cast<std::size_t>(source)]);
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

int EadEarlyPlayer::AllocVoice(int layer_index)
{
    Layer& layer = layers_[static_cast<std::size_t>(layer_index)];
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

    int voice = -1;
    const auto try_pools = [&](std::initializer_list<int> pools) -> bool
    {
        for (const int pool : pools)
        {
            voice = AllocFromDisabled(pool, layer_index);
            if (voice != -1)
            {
                return true;
            }
        }

        return false;
    };

    if ((policy & kPolicyChannel) != 0)
    {
        if ((voice = AllocFromDisabled(channel_pool, layer_index)) == -1 &&
            (voice = AllocFromDecaying(channel_pool, layer_index)) == -1 &&
            (voice = AllocFromActive(channel_pool, layer_index)) == -1)
        {
            layer.loaded = false;
        }

        return voice;
    }

    if ((policy & kPolicySequence) != 0)
    {
        if (!try_pools({channel_pool, player_pool}) && (voice = AllocFromDecaying(channel_pool, layer_index)) == -1 &&
            (voice = AllocFromDecaying(player_pool, layer_index)) == -1 &&
            (voice = AllocFromActive(channel_pool, layer_index)) == -1 &&
            (voice = AllocFromActive(player_pool, layer_index)) == -1)
        {
            layer.loaded = false;
        }

        return voice;
    }

    if ((policy & kPolicyGlobal) != 0)
    {
        if (!try_pools({kGlobalPool}) && (voice = AllocFromDecaying(kGlobalPool, layer_index)) == -1 &&
            (voice = AllocFromActive(kGlobalPool, layer_index)) == -1)
        {
            layer.loaded = false;
        }

        return voice;
    }

    if (!try_pools({channel_pool, player_pool, kGlobalPool}) &&
        (voice = AllocFromDecaying(channel_pool, layer_index)) == -1 &&
        (voice = AllocFromDecaying(player_pool, layer_index)) == -1 &&
        (voice = AllocFromDecaying(kGlobalPool, layer_index)) == -1 &&
        (voice = AllocFromActive(channel_pool, layer_index)) == -1 &&
        (voice = AllocFromActive(player_pool, layer_index)) == -1 &&
        (voice = AllocFromActive(kGlobalPool, layer_index)) == -1)
    {
        layer.loaded = false;
    }

    return voice;
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
        if (voice.priority == kPriorityDisabled)
        {
            continue;
        }

        if (voice.priority == kPriorityStopping || voice.finished)
        {
            if (voice.adsr_volume == 0 || voice.finished)
            {
                if (voice.wanted_layer == -1)
                {
                    DisableVoice(voice);
                    Remove(voice_index);
                    PushBack(Head(voice.pool, kDisabled), voice_index);
                    continue;
                }

                DisableVoice(voice);
                if (layers_[static_cast<std::size_t>(voice.wanted_layer)].channel == -1)
                {
                    DisableVoice(voice);
                    Remove(voice_index);
                    PushBack(Head(voice.pool, kDisabled), voice_index);
                    voice.wanted_layer = -1;
                    continue;
                }

                InitVoiceForLayer(voice_index, voice.wanted_layer);
                InitVibrato(voice_index);
                Remove(voice_index);
                PushBack(Head(voice.pool, kActive), voice_index);
                voice.wanted_layer = -1;
            }
        }
        else if (voice.adsr.phase == AdsrPhase::kDisabled)
        {
            DisableVoice(voice);
            Remove(voice_index);
            PushBack(Head(voice.pool, kDisabled), voice_index);
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

        voice.headset_left = kHeadsetPanDelay[static_cast<std::size_t>(small_pan)];
        voice.headset_right = kHeadsetPanDelay[static_cast<std::size_t>(9 - small_pan)];
        voice.strong_right = false;
        voice.strong_left = false;
        voice.headset_effects = true;
        left = kHeadsetPanVolume[index];
        right = kHeadsetPanVolume[127 - index];
    }
    else if (voice.stereo_effects && config_.sound_mode == EadSoundMode::kStereo)
    {
        voice.headset_left = 0;
        voice.headset_right = 0;
        voice.headset_effects = false;
        left = kStereoPanVolume[index];
        right = kStereoPanVolume[127 - index];
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
        left = kDefaultPanVolume[index];
        right = kDefaultPanVolume[127 - index];
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

void EadEarlyPlayer::RunUpdate()
{
    updates_++;
    for (int i = 0; i < kPlayers; i++)
    {
        if (players_[static_cast<std::size_t>(i)].enabled)
        {
            RunPlayer(i);
            PlayerSound(i);
        }
    }

    ReclaimVoices();
    ProcessVoices();
}

bool EadEarlyPlayer::Busy() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return player.enabled; }) ||
           std::ranges::any_of(voices_, [](const Voice& voice) { return voice.enabled; });
}

void EadEarlyPlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    build_memory_ = &memory;
    list.Segment(0, 0);
    int left = samples;
    uint32_t place = output;
    for (int remaining = updates_per_frame_; remaining > 0; remaining--)
    {
        int chunk = left;
        if (remaining != 1)
        {
            const int share = left / remaining;
            chunk = share - (share & 7);
            if ((share & 7) >= 4)
            {
                chunk += 8;
            }
        }

        RunUpdate();
        Update(list, memory, place, chunk);
        left -= chunk;
        place += static_cast<uint32_t>(chunk) * 4;
    }

    build_memory_ = nullptr;
}

void EadEarlyPlayer::Update(AudioList& list, AudioMemory& /*memory*/, uint32_t output, int samples)
{
    if (config_.reverb_window == 0)
    {
        list.ClearBuffer(kLeft, kTwoChannels);
        BuildVoices(list, output, samples);
        return;
    }

    // The ring buffer's place for this update's samples, in two parts where it wraps.
    if (samples + reverb_position_ - config_.reverb_window < 0)
    {
        reverb_length_a_ = samples * 2;
        reverb_length_b_ = 0;
        reverb_start_ = reverb_position_;
        reverb_position_ += samples;
    }
    else
    {
        const int excess = samples + reverb_position_ - config_.reverb_window;
        reverb_length_a_ = (samples - excess) * 2;
        reverb_length_b_ = excess * 2;
        reverb_start_ = reverb_position_;
        reverb_position_ = excess;
    }

    BuildReverbStart(list, samples);
    BuildVoices(list, output, samples);
    BuildReverbEnd(list);
}

void EadEarlyPlayer::BuildReverbStart(AudioList& list, int /*samples*/)
{
    const auto load_pair = [this, &list](uint32_t into, uint32_t from)
    {
        list.SetBuffer(0, into + kWetLeft, 0, kOneChannel - into);
        list.LoadBuffer(reverb_left_ + from * 2);
        list.SetBuffer(0, into + kWetRight, 0, kOneChannel - into);
        list.LoadBuffer(reverb_right_ + from * 2);
    };

    load_pair(0, static_cast<uint32_t>(reverb_start_));
    if (reverb_length_b_ != 0)
    {
        load_pair(static_cast<uint32_t>(reverb_length_a_), 0);
    }

    list.DmemMove(kWetLeft, kLeft, kTwoChannels);
    list.SetBuffer(0, 0, 0, kTwoChannels);
    list.Mixer(0, static_cast<uint32_t>(0x8000 + config_.reverb_gain) & 0xFFFF, kWetLeft, kWetLeft);
}

void EadEarlyPlayer::BuildReverbEnd(AudioList& list)
{
    const auto save_pair = [this, &list](uint32_t from, uint32_t count, uint32_t into)
    {
        list.SetBuffer(0, 0, from + kWetLeft, count);
        list.SaveBuffer(reverb_left_ + into * 2);
        list.SetBuffer(0, 0, from + kWetRight, count);
        list.SaveBuffer(reverb_right_ + into * 2);
    };

    save_pair(0, static_cast<uint32_t>(reverb_length_a_), static_cast<uint32_t>(reverb_start_));
    if (reverb_length_b_ != 0)
    {
        save_pair(static_cast<uint32_t>(reverb_length_a_), static_cast<uint32_t>(reverb_length_b_), 0);
    }
}

void EadEarlyPlayer::BuildVoices(AudioList& list, uint32_t output, int samples)
{
    const BankBook* loaded_book = nullptr;
    for (Voice& voice : voices_)
    {
        if (!voice.enabled)
        {
            continue;
        }

        list.MarkVoice(voice.track);
        int flags = 0;
        if (voice.needs_init)
        {
            flags = kFlagInit;
            voice.position = 0;
            voice.position_fraction = 0;
        }

        // A region without a sample plays nothing. The games' data never reaches one, but a hand-made file can.
        if (voice.sound != nullptr && voice.sound->sample == kNoObject)
        {
            continue;
        }

        int parts = 1;
        float rate = 0.0F;
        if (voice.frequency < 2.0F)
        {
            if (voice.frequency > 1.99996F)
            {
                voice.frequency = 1.99996F;
            }

            rate = voice.frequency;
        }
        else
        {
            parts = 2;
            if (voice.frequency >= 3.99993F)
            {
                voice.frequency = 3.99993F;
            }

            rate = voice.frequency * 0.5F;
        }

        const auto pitch = static_cast<uint16_t>(static_cast<int32_t>(rate * 32768.0F));
        const uint32_t advance =
            uint32_t{voice.position_fraction} + static_cast<uint32_t>(int32_t{pitch} * samples) * 2;
        voice.position_fraction = static_cast<uint16_t>(advance);
        const auto whole_samples = static_cast<int32_t>(advance >> 16);
        uint32_t input = 0;
        if (voice.sound == nullptr)
        {
            // A wave: its samples, repeated in DMEM.
            list.SetBuffer(0, kDecoded, 0, 128);
            list.LoadBuffer(voice.buffers.samples);
            voice.position &= voice.sample_count - 1;
            const int left_in_copy = 64 - voice.position;
            if (left_in_copy < whole_samples)
            {
                for (int i = 0; i <= (whole_samples - left_in_copy + 63) / 64 - 1; i++)
                {
                    list.DmemMove(kDecoded, kDecoded + static_cast<uint32_t>(1 + i) * 128, 128);
                }
            }

            input = kDecoded + static_cast<uint32_t>(voice.position) * 2;
            voice.position += whole_samples;
        }
        else
        {
            const EadBank& bank = files_->banks[voice.bank];
            const EadSample& sample = bank.samples[static_cast<std::size_t>(voice.sound->sample)];
            const BankLoop& loop = bank.loops[static_cast<std::size_t>(sample.loop)];
            const BankBook& book = bank.books[static_cast<std::size_t>(sample.book)];
            const uint32_t sample_address =
                AudioMemory::kImageBase + files_->sample_places[voice.bank] + sample.address;
            const auto end_position = static_cast<int32_t>(loop.end);
            int resampled_length = 0;
            int first_offset = 0;
            for (int part = 0; part < parts; part++)
            {
                int processed = 0;
                int written = 0;
                int wanted = whole_samples;
                if (parts != 1 && (whole_samples & 1) != 0)
                {
                    wanted = (whole_samples & ~1) + part * 2;
                }

                if (loaded_book != &book)
                {
                    loaded_book = &book;
                    const uint32_t size = static_cast<uint32_t>(book.book.order * book.book.predictor_count) * 16;
                    list.LoadAdpcm(size, book_addresses_[voice.bank][static_cast<std::size_t>(sample.book)]);
                }

                while (processed != wanted)
                {
                    bool finished = false;
                    bool restart = false;
                    const int to_process = wanted - processed;
                    int skip = voice.position & 0xF;
                    const int remaining = end_position - voice.position;
                    if (skip == 0 && !voice.restart)
                    {
                        skip = 16;
                    }

                    int first_part = 16 - skip;
                    int frames = 0;
                    int frame_samples = 0;
                    int overrun = 0;
                    if (to_process < remaining)
                    {
                        frames = (to_process - first_part + 0xF) / 16;
                        frame_samples = frames * 16;
                        overrun = first_part + frame_samples - to_process;
                    }
                    else
                    {
                        frame_samples = remaining + skip - 0x10;
                        overrun = 0;
                        if (frame_samples <= 0)
                        {
                            frame_samples = 0;
                            first_part = remaining;
                        }

                        frames = (frame_samples + 0xF) / 16;
                        if (loop.count != 0)
                        {
                            restart = true;
                        }
                        else
                        {
                            finished = true;
                        }
                    }

                    uint32_t misaligned = 0;
                    if (frames != 0)
                    {
                        const int frame = (voice.position - skip + 0x10) / 16;
                        const uint32_t address = sample_address + static_cast<uint32_t>(frame) * 9;
                        misaligned = address & 0xF;
                        list.SetBuffer(0, kCompressed, 0, static_cast<uint32_t>(frames) * 9 + misaligned);
                        list.LoadBuffer(address - misaligned);
                    }
                    else
                    {
                        frame_samples = 0;
                        misaligned = 0;
                    }

                    if (voice.restart)
                    {
                        list.SetLoop(loop_addresses_[voice.bank][static_cast<std::size_t>(sample.loop)]);
                        flags = kFlagLoop;
                        voice.restart = false;
                    }

                    const int this_time = frame_samples + first_part - overrun;
                    const int rounded = Align(written, 5);
                    if (processed == 0)
                    {
                        list.SetBuffer(0, kCompressed + misaligned, kDecoded, static_cast<uint32_t>(frame_samples) * 2);
                        list.Adpcm(static_cast<uint8_t>(flags), voice.buffers.adpcm_state);
                        first_offset = skip * 2;
                    }
                    else
                    {
                        list.SetBuffer(0, kCompressed + misaligned, kDecoded + static_cast<uint32_t>(rounded),
                                       static_cast<uint32_t>(frame_samples) * 2);
                        list.Adpcm(static_cast<uint8_t>(flags), voice.buffers.adpcm_state);
                        list.DmemMove(kDecoded + static_cast<uint32_t>(rounded) + static_cast<uint32_t>(skip) * 2,
                                      kDecoded + static_cast<uint32_t>(written), static_cast<uint32_t>(this_time) * 2);
                    }

                    processed += this_time;
                    switch (flags)
                    {
                    case kFlagInit:
                        first_offset = 0;
                        written = frame_samples * 2 + written;
                        break;
                    case kFlagLoop:
                        written = this_time * 2 + written;
                        break;
                    default:
                        written = written != 0 ? this_time * 2 + written : (skip + this_time) * 2;
                        break;
                    }

                    flags = 0;
                    if (finished)
                    {
                        list.ClearBuffer(kDecoded + static_cast<uint32_t>(written),
                                         static_cast<uint32_t>(wanted - processed) * 2);
                        voice.position = 0;
                        voice.finished = true;
                        voice.enabled = false;
                        break;
                    }

                    if (restart)
                    {
                        voice.restart = true;
                        voice.position = static_cast<int32_t>(loop.start);
                    }
                    else
                    {
                        voice.position += to_process;
                    }
                }

                if (parts == 1)
                {
                    input = kDecoded + static_cast<uint32_t>(first_offset);
                }
                else if (part == 0)
                {
                    list.SetBuffer(0, kDecoded + static_cast<uint32_t>(first_offset), kResampled,
                                   static_cast<uint32_t>(wanted) + 4);
                    list.Resample(kFlagInit, 0xFF60, voice.buffers.dummy_resample);
                    resampled_length = wanted + 4;
                    input = kResampled + 4;
                    if (voice.finished)
                    {
                        list.ClearBuffer(kResampled + static_cast<uint32_t>(resampled_length),
                                         static_cast<uint32_t>(wanted) + 0x10);
                    }
                }
                else
                {
                    list.SetBuffer(0, kDecoded + static_cast<uint32_t>(first_offset), kResampled2,
                                   static_cast<uint32_t>(wanted) + 8);
                    list.Resample(kFlagInit, 0xFF60, voice.buffers.dummy_resample);
                    list.DmemMove(kResampled2 + 4, kResampled + static_cast<uint32_t>(resampled_length),
                                  static_cast<uint32_t>(wanted) + 4);
                }

                if (voice.finished)
                {
                    break;
                }
            }
        }

        flags = 0;
        if (voice.needs_init)
        {
            flags = kFlagInit;
            voice.needs_init = false;
        }

        list.SetBuffer(0, input, kTemp, static_cast<uint32_t>(samples) * 2);
        list.Resample(static_cast<uint8_t>(flags), pitch, voice.buffers.final_resample);

        int headset = 0;
        if (voice.headset_right != 0 || voice.previous_headset_right != 0)
        {
            headset = 1;
        }
        else if (voice.headset_left != 0 || voice.previous_headset_left != 0)
        {
            headset = 2;
        }

        BuildEnvelope(list, voice, samples, headset);
        if (voice.headset_effects)
        {
            BuildHeadset(list, voice, samples * 2, flags, headset);
        }
    }

    list.MarkChunkEnd();
    auto length = static_cast<uint32_t>(samples) * 2;
    list.SetBuffer(0, 0, kTemp, length);
    list.Interleave(kLeft, kRight);
    length *= 2;
    list.SetBuffer(0, 0, kTemp, length);
    list.SaveBuffer(output);
}

void EadEarlyPlayer::BuildEnvelope(AudioList& list, Voice& voice, int samples, int headset)
{
    const auto source_left = static_cast<uint16_t>(voice.current_left);
    const auto source_right = static_cast<uint16_t>(voice.current_right);
    const uint16_t target_left = voice.target_left;
    const uint16_t target_right = voice.target_right;
    voice.current_left = static_cast<int16_t>(target_left);
    voice.current_right = static_cast<int16_t>(target_right);
    const auto count = static_cast<uint32_t>(samples) * 2;
    if (voice.headset_effects)
    {
        list.ClearBuffer(kPanTemp, kOneChannel);
        switch (headset)
        {
        case 1:
            list.SetBuffer(0, 0, kPanTemp, count);
            list.SetBuffer(kFlagAux, kRight, kWetLeft, kWetRight);
            break;
        case 2:
            list.SetBuffer(0, 0, kLeft, count);
            list.SetBuffer(kFlagAux, kPanTemp, kWetLeft, kWetRight);
            break;
        default:
            list.SetBuffer(0, 0, kLeft, count);
            list.SetBuffer(kFlagAux, kRight, kWetLeft, kWetRight);
            break;
        }
    }
    else if (voice.strong_right)
    {
        list.ClearBuffer(kStrongDry, kTwoChannels);
        list.SetBuffer(0, 0, kStrongDry, count);
        list.SetBuffer(kFlagAux, kRight, kStrongWet, kWetRight);
    }
    else if (voice.strong_left)
    {
        list.ClearBuffer(kStrongDry, kTwoChannels);
        list.SetBuffer(0, 0, kLeft, count);
        list.SetBuffer(kFlagAux, kStrongDry, kWetLeft, kStrongWet);
    }
    else
    {
        list.SetBuffer(0, 0, kLeft, count);
        list.SetBuffer(kFlagAux, kRight, kWetLeft, kWetRight);
    }

    uint8_t mixer_flags = 0;
    if (target_left != source_left || target_right != source_right || voice.mixer_needs_init)
    {
        mixer_flags = kFlagInit;
        const auto ramp_left = static_cast<int32_t>(RampValue(source_left, target_left, samples));
        const auto ramp_right = static_cast<int32_t>(RampValue(source_right, target_right, samples));
        list.SetVolume(kFlagVolume | kFlagLeft, source_left, 0, 0);
        list.SetVolume(kFlagVolume, source_right, 0, 0);
        list.commands.push_back(
            {(static_cast<uint32_t>(kAudioSetVolume) << 24) | (uint32_t{kFlagLeft} << 16) | target_left,
             static_cast<uint32_t>(ramp_left)});
        list.commands.push_back(
            {(static_cast<uint32_t>(kAudioSetVolume) << 24) | target_right, static_cast<uint32_t>(ramp_right)});
        list.SetVolume(kFlagAux, static_cast<uint16_t>(config_.volume), 0, static_cast<uint16_t>(voice.reverb_shifted));
    }

    const bool use_wet = config_.reverb_window != 0 && voice.reverb != 0;
    list.EnvMixer(use_wet ? static_cast<uint8_t>(mixer_flags | kFlagAux) : mixer_flags, voice.buffers.mix_state);
    if (voice.strong_right || voice.strong_left)
    {
        const uint32_t dry_target = voice.strong_right ? kLeft : kRight;
        const uint32_t wet_target = voice.strong_right ? kWetLeft : kWetRight;
        list.SetBuffer(0, 0, 0, count);
        list.Mixer(0, 0x8000, kStrongDry, dry_target);
        if (use_wet)
        {
            list.Mixer(0, 0x8000, kStrongWet, wet_target);
        }
    }
}

void EadEarlyPlayer::BuildHeadset(AudioList& list, Voice& voice, int length, int flags, int headset)
{
    uint32_t target = 0;
    uint32_t shift = 0;
    uint32_t previous_shift = 0;
    switch (headset)
    {
    case 1:
        target = kLeft;
        shift = voice.headset_right;
        voice.previous_headset_left = 0;
        previous_shift = voice.previous_headset_right;
        voice.previous_headset_right = static_cast<uint16_t>(shift);
        break;
    case 2:
        target = kRight;
        shift = voice.headset_left;
        voice.previous_headset_right = 0;
        previous_shift = voice.previous_headset_left;
        voice.previous_headset_left = static_cast<uint16_t>(shift);
        break;
    default:
        return;
    }

    const auto bytes = static_cast<uint32_t>(length);
    if (flags != kFlagInit)
    {
        if (previous_shift == 0)
        {
            list.DmemMove(kPanTemp, kTemp, 8);
            list.ClearBuffer(8, 8);
            list.DmemMove(kPanTemp, kTemp + 0x10, 0x10);
            list.SetBuffer(0, 0, kTemp, 32);
            list.SaveBuffer(voice.buffers.pan_resample);
            const auto pitch = static_cast<uint16_t>((bytes << 15) / (shift + bytes - previous_shift + 8));
            list.SetBuffer(0, kPanTemp + 8, kTemp, shift + bytes - previous_shift);
            list.Resample(0, pitch, voice.buffers.pan_resample);
        }
        else
        {
            const auto pitch = static_cast<uint16_t>(shift == 0 ? (bytes << 15) / (bytes - previous_shift - 4)
                                                                : (bytes << 15) / (bytes + shift - previous_shift));
            list.SetBuffer(0, kPanTemp, kTemp, shift + bytes - previous_shift);
            list.Resample(0, pitch, voice.buffers.pan_resample);
        }

        if (previous_shift != 0)
        {
            list.SetBuffer(0, kPanTemp, 0, previous_shift);
            list.LoadBuffer(voice.buffers.pan_samples);
            list.DmemMove(kTemp, kPanTemp + previous_shift, shift + bytes - previous_shift);
        }
        else
        {
            list.DmemMove(kTemp, kPanTemp, shift + bytes - previous_shift);
        }
    }
    else
    {
        list.DmemMove(kPanTemp, kTemp, bytes);
        list.DmemMove(kTemp, kPanTemp + shift, bytes);
        list.ClearBuffer(kPanTemp, shift);
    }

    if (shift != 0)
    {
        list.SetBuffer(0, 0, kPanTemp + bytes, shift);
        list.SaveBuffer(voice.buffers.pan_samples);
    }

    list.SetBuffer(0, 0, 0, bytes);
    list.Mixer(0, 0x7FFF, kPanTemp, target);
}

} // namespace ntdlib::n64

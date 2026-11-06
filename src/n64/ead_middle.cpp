// SPDX-License-Identifier: MIT

#include "ntdlib/n64/ead_middle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "ead_later_tables.h"
#include "ead_tables.h"

namespace ntdlib::n64
{

namespace
{

namespace later_tables = ead_later_tables;

// The places in the RSP's memory that the engine's commands name.
constexpr uint32_t kDmemTemp = 0x000;
constexpr uint32_t kDmemResampled = 0x020;
constexpr uint32_t kDmemDecoded = 0x1A0;
constexpr uint32_t kDmemPanTemp = 0x200;
constexpr uint32_t kDmemCompressedEnd = 0x540;
constexpr uint32_t kDmemLeft = 0x540;
constexpr uint32_t kDmemRight = 0x6C0;
constexpr uint32_t kDmemWetLeft = 0x840;
constexpr uint32_t kDmemWetRight = 0x9C0;
constexpr uint32_t kOneChannel = 0x180;
constexpr uint32_t kTwoChannels = 0x300;

// The same places in the lists for the standard microcode.
constexpr uint32_t kStdResampled = 0x020;
constexpr uint32_t kStdResampled2 = 0x160;
constexpr uint32_t kStdDecoded = 0x180;
constexpr uint32_t kStdPanTemp = 0x200;
constexpr uint32_t kStdStrongDry = 0x200;
constexpr uint32_t kStdStrongWet = 0x340;
constexpr uint32_t kStdCompressed = 0x3F0;
constexpr uint32_t kStdLeft = 0x4C0;
constexpr uint32_t kStdRight = 0x600;
constexpr uint32_t kStdWetLeft = 0x740;
constexpr uint32_t kStdWetRight = 0x880;
constexpr uint32_t kStdOneChannel = 0x140;
constexpr uint32_t kStdTwoChannels = 0x280;

constexpr uint32_t kFlagContinue = 0;
constexpr uint32_t kFlagInit = 1;
constexpr uint32_t kFlagLoop = 2;

constexpr int kFrameSamples = 16;
constexpr int kWaveSamples = 64;

// The microcode's opcodes.
constexpr uint32_t kOpAdpcm = 0x01;
constexpr uint32_t kOpClear = 0x02;
constexpr uint32_t kOpResample = 0x05;
constexpr uint32_t kOpSegment = 0x07;
constexpr uint32_t kOpSetBuffer = 0x08;
constexpr uint32_t kOpDuplicate = 0x09;
constexpr uint32_t kOpMove = 0x0A;
constexpr uint32_t kOpLoadBook = 0x0B;
constexpr uint32_t kOpMixer = 0x0C;
constexpr uint32_t kOpInterleave = 0x0D;
constexpr uint32_t kOpSetLoop = 0x0F;
constexpr uint32_t kOpHalve = 0x11;
constexpr uint32_t kOpEnvSetup1 = 0x12;
constexpr uint32_t kOpEnvMixer = 0x13;
constexpr uint32_t kOpLoad = 0x14;
constexpr uint32_t kOpSave = 0x15;
constexpr uint32_t kOpEnvSetup2 = 0x16;

constexpr uint8_t kMuteSoften = 1U << 5;
constexpr uint8_t kMuteStopNotes = 1U << 6;
constexpr uint8_t kMuteStopScript = 1U << 7;

constexpr uint8_t kPortamentoSpecial = 0x80;

constexpr uint8_t kPolicyLayer = 1;
constexpr uint8_t kPolicyChannel = 2;
constexpr uint8_t kPolicyPlayer = 4;
constexpr uint8_t kPolicyGlobal = 8;

constexpr uint8_t kPriorityDisabled = 0;
constexpr uint8_t kPriorityStopping = 1;
constexpr uint8_t kPriorityMin = 2;
constexpr uint8_t kPriorityDefault = 3;

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

constexpr int kTicksPerBeat = 48;
constexpr int kPoolCount = 1 + EadMiddlePlayer::kPlayers + EadMiddlePlayer::kChannels;

constexpr std::array<EadEnvelope::Point, 3> kDefaultEnvelope = {{{4, 32000}, {1000, 32000}, {kEnvelopeHang, 0}}};

// The delay in samples of the far side in headset mode. Offset 0xF6E70 of the American image of Mario Kart 64.
constexpr std::array<uint16_t, 16> kHeadsetPanDelay = {0x40, 0x40, 0x30, 0x30, 0x20, 0x20, 0x10, 0,
                                                       0,    0,    0,    0,    0,    0,    0,    0};

// The samples in one period of each harmonic of a wave.
constexpr std::array<int, 4> kSamplesInPeriod = {kWaveSamples, kWaveSamples / 2, kWaveSamples / 4, kWaveSamples / 8};

constexpr int Align8(int value)
{
    return (value + 7) & ~7;
}

constexpr int Align16(int value)
{
    return (value + 15) & ~15;
}

// The k-th root of `value`, for k from 9 to 24, by Newton's method from 1.5, as the code at 0x800B7378 in Wave Race 64.
double KthRoot(double value, int k)
{
    constexpr double kClose = 1e-07;
    constexpr int kMostSteps = 64;
    double root = 1.5;
    for (int step = 0; step < kMostSteps; step++)
    {
        const double square = root * root;
        const double fourth = square * square;
        const double eighth = fourth * fourth;
        const int degree = k - 9;
        double power = eighth;
        if ((degree & 1) != 0)
        {
            power *= root;
        }

        if ((degree & 2) != 0)
        {
            power *= square;
        }

        if ((degree & 4) != 0)
        {
            power *= fourth;
        }

        if ((degree & 8) != 0)
        {
            power *= eighth;
        }

        const double next = root - (power * root - value) / (k * power);
        const double change = std::abs(next - root);
        root = next;
        if (change < kClose)
        {
            break;
        }
    }

    return root;
}

uint32_t Word0(uint32_t opcode, uint32_t middle, uint32_t low)
{
    return (opcode << 24) | ((middle & 0xFF) << 16) | (low & 0xFFFF);
}

uint32_t Pair(uint32_t high, uint32_t low)
{
    return (high << 16) | (low & 0xFFFF);
}

void Clear(AudioList& list, uint32_t dmem, uint32_t size)
{
    list.commands.push_back({Word0(kOpClear, 0, dmem), size});
}

void SetBuffer(AudioList& list, uint32_t in, uint32_t out, uint32_t size)
{
    list.commands.push_back({Word0(kOpSetBuffer, 0, in), Pair(out, size)});
}

void Load(AudioList& list, uint32_t address, uint32_t dmem, uint32_t size)
{
    list.commands.push_back({Word0(kOpLoad, size >> 4, dmem), address});
}

void Save(AudioList& list, uint32_t dmem, uint32_t address, uint32_t size)
{
    list.commands.push_back({Word0(kOpSave, size >> 4, dmem), address});
}

void Move(AudioList& list, uint32_t in, uint32_t out, uint32_t size)
{
    list.commands.push_back({(kOpMove << 24) | (in & 0xFFFFFF), Pair(out, size)});
}

void Mix(AudioList& list, uint32_t size, uint32_t gain, uint32_t in, uint32_t out)
{
    list.commands.push_back({Word0(kOpMixer, size >> 4, gain), Pair(in, out)});
}

void Resample(AudioList& list, uint32_t flags, uint32_t pitch, uint32_t state)
{
    list.commands.push_back({Word0(kOpResample, flags, pitch), state});
}

void Halve(AudioList& list, uint32_t count, uint32_t in, uint32_t out)
{
    list.commands.push_back({(kOpHalve << 24) | (count & 0xFFFF), Pair(in, out)});
}

void WriteBe16(AudioMemory& memory, uint32_t address, int16_t value)
{
    const std::array<uint8_t, 2> bytes = {static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8),
                                          static_cast<uint8_t>(static_cast<uint16_t>(value))};
    memory.Write(address, bytes.data(), bytes.size());
}

int16_t ReadBe16(const AudioMemory& memory, uint32_t address)
{
    std::array<uint8_t, 2> bytes = {};
    memory.Read(address, bytes.data(), bytes.size());

    return static_cast<int16_t>((uint32_t{bytes[0]} << 8) | bytes[1]);
}

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

    const auto voice_count = static_cast<std::size_t>(config.voices);
    voices_.resize(voice_count);
    next_.resize(voice_count + static_cast<std::size_t>(kPoolCount) * 4);
    previous_.assign(next_.size(), -1);
    for (int head = 0; head < kPoolCount * 4; head++)
    {
        const int place = config.voices + head;
        next_[static_cast<std::size_t>(place)] = place;
        previous_[static_cast<std::size_t>(place)] = place;
    }

    update_states_.resize(voice_count * static_cast<std::size_t>(updates_per_frame_));
    if (config.standard_microcode)
    {
        BuildRampTables();
    }

    for (int i = 0; i < config.voices; i++)
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

    waves_address_ = memory.Allocate(later_tables::kWaves.size() * 2);
    for (std::size_t i = 0; i < later_tables::kWaves.size(); i++)
    {
        WriteBe16(memory, waves_address_ + static_cast<uint32_t>(i) * 2, later_tables::kWaves[i]);
    }

    reverbs_.resize(config.reverbs.size());
    for (std::size_t i = 0; i < reverbs_.size(); i++)
    {
        const EadMiddleReverb& settings = config.reverbs[i];
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
                        item.downsample_left + (config.standard_microcode ? kStdOneChannel : kOneChannel);
                }
            }
        }
    }

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

uint8_t EadMiddlePlayer::PeekByte(const Player& player, uint32_t place) const
{
    return place < player.bytes.size() ? player.bytes[place] : uint8_t{0};
}

uint16_t EadMiddlePlayer::Peek16(const Player& player, uint32_t place) const
{
    return static_cast<uint16_t>((PeekByte(player, place) << 8) | PeekByte(player, place + 1));
}

uint8_t EadMiddlePlayer::Read8(const Player& player, Script& script) const
{
    return PeekByte(player, script.pc++);
}

uint16_t EadMiddlePlayer::Read16(const Player& player, Script& script) const
{
    const uint16_t value = Peek16(player, script.pc);
    script.pc += 2;

    return value;
}

uint16_t EadMiddlePlayer::ReadVariable(const Player& player, Script& script) const
{
    uint16_t value = Read8(player, script);
    if ((value & 0x80) != 0)
    {
        value = static_cast<uint16_t>(((value << 8) & 0x7F00) | Read8(player, script));
    }

    return value;
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

void EadMiddlePlayer::RunPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (player.muted && (player.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count + player.tempo);
    if (static_cast<int32_t>(player.tempo_count) < max_tempo_)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count - static_cast<uint16_t>(max_tempo_));
    if (player.delay > 1)
    {
        player.delay--;
    }
    else
    {
        player.recalculate_volume = true;
        Script& script = player.script;
        int value = 0;
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
                        const std::size_t top = (script.depth - 1U) & 3U;
                        script.loops[top]--;
                        if (script.loops[top] != 0)
                        {
                            script.pc = script.stack[top];
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
                case 0xF4:
                case 0xF3:
                case 0xF2:
                    {
                        const auto relative = static_cast<int8_t>(Read8(player, script));
                        if ((command == 0xF3 && value != 0) || (command == 0xF2 && value >= 0))
                        {
                            break;
                        }

                        script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                        break;
                    }
                case 0xF1:
                    FillPool(PlayerPool(index), Read8(player, script));
                    break;
                case 0xF0:
                    ClearPool(PlayerPool(index));
                    break;
                case 0xDF:
                    player.transpose = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xDE:
                    player.transpose =
                        static_cast<int16_t>(player.transpose + static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xDC:
                case 0xDD:
                    {
                        const uint8_t tempo = Read8(player, script);
                        if (command == 0xDD)
                        {
                            player.tempo = static_cast<uint16_t>(tempo * kTicksPerBeat);
                        }
                        else
                        {
                            player.tempo =
                                static_cast<uint16_t>(player.tempo + static_cast<int8_t>(tempo) * kTicksPerBeat);
                        }

                        if (static_cast<int32_t>(player.tempo) > max_tempo_)
                        {
                            player.tempo = static_cast<uint16_t>(max_tempo_);
                        }

                        if (static_cast<int16_t>(player.tempo) <= 0)
                        {
                            player.tempo = 1;
                        }

                        break;
                    }
                case 0xDA:
                    {
                        const uint8_t mode = Read8(player, script);
                        const uint16_t updates = Read16(player, script);
                        if (mode == 0 || mode == 1)
                        {
                            if (player.state != 2)
                            {
                                player.fade_updates_next = updates;
                                player.state = mode;
                            }
                        }
                        else if (mode == 2)
                        {
                            player.fade_updates = updates;
                            player.state = mode;
                            player.fade_step = (0.0F - player.fade_volume) / static_cast<float>(int32_t{updates});
                        }

                        break;
                    }
                case 0xDB:
                    {
                        const int volume = Read8(player, script);
                        if (player.state == 1)
                        {
                            player.state = 0;
                            player.fade_volume = 0.0F;
                        }

                        if (player.state == 0)
                        {
                            player.fade_updates = player.fade_updates_next;
                            if (player.fade_updates_next != 0)
                            {
                                player.fade_step = (static_cast<float>(volume) / 127.0F - player.fade_volume) /
                                                   static_cast<float>(int32_t{player.fade_updates});
                            }
                            else
                            {
                                player.fade_volume = static_cast<float>(volume) / 127.0F;
                            }
                        }

                        break;
                    }
                case 0xD9:
                    player.fade_scale = static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
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
                    value &= Read8(player, script);
                    break;
                case 0xC8:
                    value -= Read8(player, script);
                    break;
                default:
                    break;
                }

                continue;
            }

            const int low = command & 0xF;
            switch (command & 0xF0)
            {
            case 0x00:
                {
                    const Channel* channel = ChannelOf(index, low);
                    value = channel != nullptr && channel->finished ? 1 : 0;
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

    for (const int channel : player.channels)
    {
        if (channel != kNoChannel)
        {
            RunChannel(channel);
        }
    }
}

void EadMiddlePlayer::RunChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.enabled)
    {
        return;
    }

    const auto run_layers = [&]()
    {
        for (const int layer : channel.layers)
        {
            if (layer != kNoLayer)
            {
                RunLayer(layer);
            }
        }
    };
    if (channel.stop_script)
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
    const auto table_entry = [&](int value)
    {
        return Peek16(player, static_cast<uint32_t>(static_cast<int32_t>(channel.dynamic_table) + value * 2));
    };
    if (channel.delay == 0)
    {
        int8_t value = 0;
        bool running = true;
        while (running)
        {
            uint8_t command = Read8(player, script);
            if (command > 0xC0)
            {
                switch (command)
                {
                case 0xFF:
                    if (script.depth == 0)
                    {
                        DisableChannel(index);
                        running = false;
                    }
                    else
                    {
                        script.depth--;
                        script.pc = script.stack[script.depth & 3];
                    }

                    break;
                case 0xFE:
                    running = false;
                    break;
                case 0xFD:
                    channel.delay = ReadVariable(player, script);
                    running = false;
                    break;
                case 0xEA:
                    channel.stop_script = true;
                    running = false;
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
                        const std::size_t top = (script.depth - 1U) & 3U;
                        script.loops[top]--;
                        if (script.loops[top] != 0)
                        {
                            script.pc = script.stack[top];
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
                case 0xF5:
                case 0xF9:
                case 0xFA:
                case 0xFB:
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
                case 0xF3:
                case 0xF4:
                    {
                        const auto relative = static_cast<int8_t>(Read8(player, script));
                        if ((command == 0xF3 && value != 0) || (command == 0xF2 && value >= 0))
                        {
                            break;
                        }

                        script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                        break;
                    }
                case 0xF1:
                    FillPool(ChannelPool(index), Read8(player, script));
                    break;
                case 0xF0:
                    ClearPool(ChannelPool(index));
                    break;
                case 0xC2:
                    channel.dynamic_table = Read16(player, script);
                    break;
                case 0xC5:
                    if (value != -1)
                    {
                        channel.dynamic_table = table_entry(value);
                    }

                    break;
                case 0xEB:
                    ChangeBank(channel, player, Read8(player, script));
                    SetProgram(index, Read8(player, script));
                    break;
                case 0xC1:
                    SetProgram(index, Read8(player, script));
                    break;
                case 0xC3:
                    channel.large_notes = false;
                    break;
                case 0xC4:
                    channel.large_notes = true;
                    break;
                case 0xDF:
                    channel.volume = static_cast<float>(Read8(player, script)) / 127.0F;
                    channel.change_volume = true;
                    break;
                case 0xE0:
                    channel.volume_scale = static_cast<float>(Read8(player, script)) / 128.0F;
                    channel.change_volume = true;
                    break;
                case 0xDE:
                    channel.frequency_scale = static_cast<float>(int32_t{Read16(player, script)}) / 32768.0F;
                    channel.change_frequency = true;
                    break;
                case 0xD3:
                    channel.frequency_scale =
                        later_tables::kBendOctave[static_cast<uint8_t>(Read8(player, script) + 127)];
                    channel.change_frequency = true;
                    break;
                case 0xDD:
                    channel.new_pan = Read8(player, script);
                    channel.change_pan = true;
                    break;
                case 0xDC:
                    channel.pan_weight = Read8(player, script);
                    channel.change_pan = true;
                    break;
                case 0xDB:
                    channel.transpose = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xDA:
                    channel.adsr.envelope = {};
                    channel.adsr.envelope.player = player_index;
                    channel.adsr.envelope.offset = Read16(player, script);
                    break;
                case 0xD9:
                    channel.adsr.release = Read8(player, script);
                    break;
                case 0xD8:
                    channel.vibrato_depth_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_depth_start = 0;
                    channel.vibrato_depth_delay = 0;
                    break;
                case 0xD7:
                    channel.vibrato_rate_target = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_start = channel.vibrato_rate_target;
                    channel.vibrato_rate_delay = 0;
                    break;
                case 0xE2:
                    channel.vibrato_depth_start = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_depth_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_depth_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE1:
                    channel.vibrato_rate_start = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_target = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE3:
                    channel.vibrato_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xD4:
                    channel.reverb = Read8(player, script);
                    break;
                case 0xC6:
                    ChangeBank(channel, player, Read8(player, script));
                    break;
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
                case 0xC9:
                case 0xCC:
                    {
                        const auto operand = static_cast<int8_t>(Read8(player, script));
                        if (command == 0xC8)
                        {
                            value = static_cast<int8_t>(value - operand);
                        }
                        else if (command == 0xCC)
                        {
                            value = operand;
                        }
                        else
                        {
                            value = static_cast<int8_t>(value & operand);
                        }

                        break;
                    }
                case 0xCA:
                    channel.mute_behavior = Read8(player, script);
                    break;
                case 0xCB:
                    {
                        const int place = Read16(player, script) + value;
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
                    channel.adsr.sustain = Read8(player, script);
                    break;
                case 0xE5:
                    channel.reverb_index = Read8(player, script);
                    break;
                case 0xE4:
                    if (value != -1)
                    {
                        const uint16_t target = table_entry(value);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = target;
                    }

                    break;
                case 0xE6:
                    channel.book_offset = Read8(player, script);
                    break;
                case 0xE7:
                case 0xE8:
                    {
                        // The settings lie in a table in the sequence, or follow the command.
                        Script from = script;
                        if (command == 0xE7)
                        {
                            from.pc = Read16(player, script);
                        }

                        channel.mute_behavior = Read8(player, from);
                        channel.policy = Read8(player, from);
                        channel.priority = Read8(player, from);
                        channel.transpose = static_cast<int8_t>(Read8(player, from));
                        channel.new_pan = Read8(player, from);
                        channel.pan_weight = Read8(player, from);
                        channel.reverb = Read8(player, from);
                        channel.reverb_index = Read8(player, from);
                        channel.change_pan = true;
                        if (command == 0xE8)
                        {
                            script.pc = from.pc;
                        }

                        break;
                    }
                case 0xEC:
                    channel.vibrato_depth_target = 0;
                    channel.vibrato_depth_start = 0;
                    channel.vibrato_depth_delay = 0;
                    channel.vibrato_rate_target = 0;
                    channel.vibrato_rate_start = 0;
                    channel.vibrato_rate_delay = 0;
                    channel.frequency_scale = 1.0F;
                    break;
                case 0xE9:
                    channel.priority = Read8(player, script);
                    break;
                case 0xEF:
                    Read16(player, script);
                    Read8(player, script);
                    break;
                default:
                    break;
                }

                continue;
            }

            const int low = command & 0xF;
            switch (command & 0xF0)
            {
            case 0x00:
                if (low < kLayersPerChannel && channel.layers[static_cast<std::size_t>(low)] != kNoLayer)
                {
                    value = layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].finished
                                ? int8_t{1}
                                : int8_t{0};
                }
                else
                {
                    value = -1;
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
            case 0x60:
                channel.delay = static_cast<uint16_t>(low);
                running = false;
                break;
            case 0x90:
                {
                    const uint16_t target = Read16(player, script);
                    if (low < kLayersPerChannel && SetLayer(index, low))
                    {
                        layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                            target;
                    }

                    break;
                }
            case 0xA0:
                if (low < kLayersPerChannel)
                {
                    FreeLayer(index, low);
                }

                break;
            case 0xB0:
                if (value != -1 && low < kLayersPerChannel && SetLayer(index, low))
                {
                    layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                        table_entry(value);
                }

                break;
            case 0x10:
                EnableChannel(player_index, low, Read16(player, script));
                break;
            case 0x20:
                if (const int other = player.channels[static_cast<std::size_t>(low)]; other != kNoChannel)
                {
                    DisableChannel(other);
                }

                break;
            case 0x30:
                {
                    const uint8_t port = Read8(player, script);
                    if (Channel* other = ChannelOf(player_index, low); other != nullptr && port < kPorts)
                    {
                        other->ports[port] = value;
                    }

                    break;
                }
            case 0x40:
                {
                    const uint8_t port = Read8(player, script);
                    const Channel* other = ChannelOf(player_index, low);
                    value = other != nullptr && port < kPorts ? other->ports[port] : int8_t{0};
                    break;
                }
            default:
                break;
            }
        }
    }

    run_layers();
}

void EadMiddlePlayer::RunLayer(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (!layer.enabled)
    {
        return;
    }

    if (layer.delay > 1)
    {
        layer.delay--;
        if (!layer.muted && layer.delay <= layer.gate_delay)
        {
            DecayOrRelease(index, false);
            layer.muted = true;
        }

        return;
    }

    if (!layer.continuous)
    {
        DecayOrRelease(index, false);
    }

    const int old_mode = layer.portamento.mode & ~kPortamentoSpecial;
    if (old_mode == 1 || old_mode == 2)
    {
        layer.portamento.mode = 0;
    }

    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    layer.properties_need_init = true;
    uint32_t command_place = 0;
    uint8_t command = 0;
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
                const std::size_t top = (script.depth - 1U) & 3U;
                script.loops[top]--;
                if (script.loops[top] != 0)
                {
                    script.pc = script.stack[top];
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
        case 0xF4:
            {
                const auto relative = static_cast<int8_t>(Read8(player, script));
                script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                break;
            }
        case 0xC1:
            {
                const int value = Read8(player, script);
                layer.velocity_square = static_cast<float>(value * value) / VelocityDivisor();
                break;
            }
        case 0xCA:
            layer.pan = Read8(player, script);
            break;
        case 0xC2:
            layer.transpose = Read8(player, script);
            break;
        case 0xC9:
            layer.gate = Read8(player, script);
            break;
        case 0xC4:
        case 0xC5:
            layer.continuous = command == 0xC4;
            DecayOrRelease(index, false);
            break;
        case 0xC3:
            layer.default_length = static_cast<int16_t>(ReadVariable(player, script));
            break;
        case 0xC6:
            {
                const uint8_t program = Read8(player, script);
                if (program >= 0x7F)
                {
                    if (program == 0x7F)
                    {
                        layer.program = 0;
                    }
                    else
                    {
                        layer.program = program;
                        layer.instrument = nullptr;
                    }

                    if (program == 0xFF)
                    {
                        layer.adsr.release = 0;
                    }
                }
                else
                {
                    layer.program = FindProgram(channel, program, layer.instrument, layer.adsr);
                    if (layer.program == 0)
                    {
                        layer.program = 0xFF;
                    }
                }

                break;
            }
        case 0xC7:
            {
                layer.portamento.mode = Read8(player, script);
                auto note = static_cast<uint8_t>(Read8(player, script) + channel.transpose + layer.transpose +
                                                 player.transpose);
                if (note >= 0x80)
                {
                    note = 0;
                }

                layer.portamento_target = note;
                if ((layer.portamento.mode & kPortamentoSpecial) != 0)
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
        case 0xCB:
            layer.adsr.envelope = {};
            layer.adsr.envelope.player = channel.player;
            layer.adsr.envelope.offset = Read16(player, script);
            layer.adsr.release = Read8(player, script);
            break;
        case 0xCC:
            layer.ignore_drum_pan = true;
            break;
        default:
            if ((command & 0xF0) == 0xD0)
            {
                const int place = player.short_velocities;
                const int value = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                             : later_tables::kShortVelocity[command & 0xFU];
                layer.velocity_square = static_cast<float>(value * value) / VelocityDivisor();
            }
            else if ((command & 0xF0) == 0xE0)
            {
                const int place = player.short_gates;
                layer.gate = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                        : later_tables::kShortGate[command & 0xFU];
            }

            break;
        }
    }

    bool same_sound = true;
    uint8_t note = 0;
    bool is_drum = false;
    if (command == 0xC0)
    {
        layer.delay = static_cast<int16_t>(ReadVariable(player, script));
        layer.muted = true;
    }
    else
    {
        layer.muted = false;
        uint16_t delay = 0;
        if (channel.large_notes)
        {
            int velocity = 0;
            switch (command & 0xC0)
            {
            case 0x00:
                delay = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                layer.last_length = static_cast<int16_t>(delay);
                break;
            case 0x40:
                delay = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = 0;
                layer.last_length = static_cast<int16_t>(delay);
                break;
            default:
                delay = static_cast<uint16_t>(layer.last_length);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                break;
            }

            if (velocity >= 0x80)
            {
                velocity = 0x7F;
            }

            layer.velocity_square = static_cast<float>(velocity) * static_cast<float>(velocity) / VelocityDivisor();
        }
        else
        {
            switch (command & 0xC0)
            {
            case 0x00:
                delay = ReadVariable(player, script);
                layer.last_length = static_cast<int16_t>(delay);
                break;
            case 0x40:
                delay = static_cast<uint16_t>(layer.default_length);
                break;
            default:
                delay = static_cast<uint16_t>(layer.last_length);
                break;
            }
        }

        command = static_cast<uint8_t>(command & 0x3F);
        layer.delay = static_cast<int16_t>(delay);
        layer.gate_delay = static_cast<int16_t>((layer.gate * int32_t{delay}) >> 8);
        if ((player.muted && (channel.mute_behavior & kMuteStopNotes) != 0) || channel.stop_notes)
        {
            layer.muted = true;
        }
        else
        {
            int program = layer.program;
            if (program == 0xFF)
            {
                if (!channel.has_instrument)
                {
                    return;
                }

                program = channel.program;
            }

            if (program == 0)
            {
                is_drum = true;
                note = static_cast<uint8_t>(command + channel.transpose + layer.transpose);
                const EadDrum* drum = Drum(channel.bank, note);
                if (drum == nullptr)
                {
                    layer.muted = true;

                    return;
                }

                layer.adsr.envelope = BankEnvelope(channel.bank, drum->envelope);
                layer.adsr.release = drum->release_rate;
                if (!layer.ignore_drum_pan)
                {
                    layer.pan = drum->pan;
                }

                layer.sound = &drum->sound;
                layer.sound_bank = channel.bank;
                layer.frequency_scale = drum->sound.tuning;
            }
            else
            {
                note = static_cast<uint8_t>(command + player.transpose + channel.transpose + layer.transpose);
                if (note >= 0x80)
                {
                    layer.muted = true;
                }
                else
                {
                    const EadInstrument* instrument = layer.program == 0xFF ? channel.instrument : layer.instrument;
                    const auto sound_of = [](const EadInstrument& held, int key) -> const EadTunedSample*
                    {
                        if (key < held.normal_range_low)
                        {
                            return &held.low;
                        }

                        return key <= held.normal_range_high ? &held.normal : &held.high;
                    };
                    if (layer.portamento.mode != 0)
                    {
                        const int highest = layer.portamento_target < note ? note : layer.portamento_target;
                        float tuning = 1.0F;
                        if (instrument != nullptr)
                        {
                            const EadTunedSample* sound = sound_of(*instrument, highest);
                            same_sound = sound == layer.sound;
                            layer.sound = sound;
                            layer.sound_bank = channel.bank;
                            tuning = sound->tuning;
                        }
                        else
                        {
                            layer.sound = nullptr;
                        }

                        const float from_note = ead_tables::kNoteFrequencies[note] * tuning;
                        const float to_note = ead_tables::kNoteFrequencies[layer.portamento_target] * tuning;
                        const int mode = layer.portamento.mode & ~kPortamentoSpecial;
                        const float base = mode == 1 || mode == 3 || mode == 5 ? to_note : from_note;
                        layer.portamento.extent = from_note / base - 1.0F;
                        if ((layer.portamento.mode & kPortamentoSpecial) != 0)
                        {
                            layer.portamento.speed = 32512.0F * static_cast<float>(player.tempo) /
                                                     (static_cast<float>(layer.delay) * static_cast<float>(max_tempo_) *
                                                      static_cast<float>(layer.portamento_time));
                        }
                        else
                        {
                            layer.portamento.speed = 127.0F / static_cast<float>(layer.portamento_time);
                        }

                        layer.portamento.position = 0.0F;
                        layer.frequency_scale = base;
                        if (mode == 5)
                        {
                            layer.portamento_target = note;
                        }
                    }
                    else if (instrument != nullptr)
                    {
                        const EadTunedSample* sound = sound_of(*instrument, note);
                        same_sound = sound == layer.sound;
                        layer.sound = sound;
                        layer.sound_bank = channel.bank;
                        layer.frequency_scale = ead_tables::kNoteFrequencies[note] * sound->tuning;
                    }
                    else
                    {
                        layer.sound = nullptr;
                        layer.frequency_scale = ead_tables::kNoteFrequencies[note];
                    }
                }
            }
        }
    }

    if (layer.muted)
    {
        if (layer.voice != kNoVoice || layer.continuous)
        {
            DecayOrRelease(index, false);
        }

        return;
    }

    bool take_voice = false;
    if (!layer.continuous || layer.voice == kNoVoice || !layer.loaded)
    {
        take_voice = true;
    }
    else if (!same_sound)
    {
        DecayOrRelease(index, false);
        take_voice = true;
    }
    else if (index != voices_[static_cast<std::size_t>(layer.voice)].layer)
    {
        take_voice = true;
    }
    else if (layer.sound == nullptr)
    {
        Voice& voice = voices_[static_cast<std::size_t>(layer.voice)];
        const int previous = voice.harmonic;
        const int wave = layer.program == 0xFF ? channel.program : layer.program;
        const int current = BuildWave(voice, layer, wave);
        voice.position = voice.position * kSamplesInPeriod[static_cast<std::size_t>(current)] /
                         kSamplesInPeriod[static_cast<std::size_t>(previous)];
    }

    if (take_voice)
    {
        layer.voice = AllocVoice(index);
    }

    if (layer.voice != kNoVoice && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        InitVibrato(layer.voice);
    }

    if (layer.voice != kNoVoice)
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
        EadNoteStart start;
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

        start.key = note;
        start.drum = is_drum;
        start.bank = channel.bank;
        start.program = layer.program == 0xFF ? channel.program : layer.program;
        start.velocity = layer.velocity_square;
        start.pan = static_cast<float>(layer.pan) / 127.0F;
        start.length = layer.delay;
        start.gate = layer.gate_delay;
        start.offset = command_place;
        start.update = updates_;
        start.voice = layer.voice;
        on_note(start);
    }
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
        if (state == kAdsrInitial || state == kAdsrStartLoop)
        {
            adsr.index = 0;
            adsr.state = kAdsrLoop;
        }

        if (state != kAdsrFade)
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
                        adsr.delay =
                            static_cast<int16_t>(adsr.delay * updates_per_frame_ / config_.frames_per_list / 4);
                    }

                    if (adsr.delay == 0 && !config_.standard_microcode)
                    {
                        adsr.delay = 1;
                    }

                    adsr.target = config_.standard_microcode
                                      ? static_cast<float>(static_cast<double>(point.target) / 32767.0)
                                      : static_cast<float>(point.target) / 32767.0F;
                    adsr.target = adsr.target * adsr.target;
                    adsr.velocity = (adsr.target - adsr.current) / static_cast<float>(adsr.delay);
                    adsr.state = kAdsrFade;
                    adsr.index++;
                }

                break;
            }
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
        Voice& voice = voices_[static_cast<std::size_t>(i)];
        if (voice.layer != kNoLayer)
        {
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
                    continue;
                }

                stop = players_[static_cast<std::size_t>(player)].muted &&
                       (channels_[static_cast<std::size_t>(layer.channel)].mute_behavior &
                        (kMuteStopScript | kMuteStopNotes)) != 0;
            }

            if (stop)
            {
                DecayOrRelease(layer_index, true);
                Remove(i);
                PushFront(Head(voice.pool, kDecaying), i);
                voice.priority = kPriorityStopping;
            }
        }
        else if (voice.priority >= kPriorityMin)
        {
            continue;
        }

        if (voice.priority == kPriorityDisabled)
        {
            continue;
        }

        UpdateState& state = voice.state;
        if (voice.priority == kPriorityStopping || state.finished)
        {
            if (voice.adsr.state == kAdsrDisabled || state.finished)
            {
                const int wanted = voice.wanted_layer;
                DisableVoice(voice);
                Remove(i);
                voice.wanted_layer = kNoLayer;
                if (wanted == kNoLayer || layers_[static_cast<std::size_t>(wanted)].channel == kNoChannel)
                {
                    if (wanted != kNoLayer)
                    {
                        DisableVoice(voice);
                    }

                    PushBack(Head(voice.pool, kDisabled), i);
                    continue;
                }

                InitVoiceForLayer(i, wanted);
                InitVibrato(i);
                PushBack(Head(voice.pool, kActive), i);
            }
        }
        else if (voice.adsr.state == kAdsrDisabled)
        {
            DisableVoice(voice);
            Remove(i);
            PushBack(Head(voice.pool, kDisabled), i);
            continue;
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
            book_offset = state.book_offset;
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
        state.book_offset = book_offset;
    }
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

void EadMiddlePlayer::StartReverbItem(AudioMemory& memory, Reverb& reverb, int length, int update)
{
    ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample != 1 && reverb.frames_to_ignore == 0)
    {
        // The CPU thins the samples that the microcode saved in this frame's slot two frames ago.
        int from = 0;
        const auto copy = [&](int count, int ring_start)
        {
            for (int i = 0; i < count; i++)
            {
                const uint32_t at = static_cast<uint32_t>(from) * 2;
                const auto place = static_cast<uint32_t>(ring_start + i) * 2;
                WriteBe16(memory, reverb.left + place, ReadBe16(memory, item.downsample_left + at));
                WriteBe16(memory, reverb.right + place, ReadBe16(memory, item.downsample_right + at));
                from += reverb.downsample;
            }
        };
        copy(item.length_a / 2, item.start);
        copy(item.length_b / 2, 0);
    }

    const int samples = length / reverb.downsample;
    const int extra = samples + reverb.next_position - reverb.buffer_samples;
    item.start = reverb.next_position;
    if (extra < 0)
    {
        item.length_a = static_cast<int16_t>(samples * 2);
        item.length_b = 0;
        reverb.next_position += samples;
    }
    else
    {
        item.length_a = static_cast<int16_t>((samples - extra) * 2);
        item.length_b = static_cast<int16_t>(extra * 2);
        reverb.next_position = extra;
    }
}

void EadMiddlePlayer::LoadReverbSamples(AudioList& list, const Reverb& reverb, int length, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    const auto load = [&](uint32_t dmem, int offset, uint32_t size)
    {
        const auto bytes = static_cast<uint32_t>(offset) * 2;
        Load(list, reverb.left + bytes, dmem, size);
        Load(list, reverb.right + bytes, dmem + kOneChannel, size);
    };
    Clear(list, kDmemWetLeft, kTwoChannels);
    if (reverb.downsample == 1)
    {
        load(kDmemWetLeft, item.start, static_cast<uint16_t>(item.length_a));
        if (item.length_b != 0)
        {
            load(kDmemWetLeft + static_cast<uint32_t>(item.length_a), 0, static_cast<uint16_t>(item.length_b));
        }
    }
    else
    {
        const int start_pad = (item.start % 8) * 2;
        const int padded = Align16(start_pad + item.length_a);
        load(kDmemResampled, item.start - start_pad / 2, kOneChannel);
        if (item.length_b != 0)
        {
            load(kDmemResampled + static_cast<uint32_t>(padded), 0, kOneChannel - static_cast<uint32_t>(padded));
        }

        const auto pad = static_cast<uint32_t>(start_pad);
        const auto size = static_cast<uint32_t>(length) * 2;
        SetBuffer(list, kDmemResampled + pad, kDmemWetLeft, size);
        Resample(list, reverb.resample_flags, reverb.downsample_pitch, reverb.resample_left);
        SetBuffer(list, kDmemDecoded + pad, kDmemWetRight, size);
        Resample(list, reverb.resample_flags, reverb.downsample_pitch, reverb.resample_right);
    }

    Mix(list, kTwoChannels, 0x7FFF, kDmemWetLeft, kDmemLeft);
    Mix(list, kTwoChannels, 0x8000U + reverb.gain, kDmemWetLeft, kDmemWetLeft);
}

void EadMiddlePlayer::SaveReverbSamples(AudioList& list, Reverb& reverb, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample == 1)
    {
        const auto save = [&](uint32_t dmem, int offset, uint32_t size)
        {
            const auto bytes = static_cast<uint32_t>(offset) * 2;
            Save(list, dmem, reverb.left + bytes, size);
            Save(list, dmem + kOneChannel, reverb.right + bytes, size);
        };
        save(kDmemWetLeft, item.start, static_cast<uint16_t>(item.length_a));
        if (item.length_b != 0)
        {
            save(kDmemWetLeft + static_cast<uint32_t>(item.length_a), 0, static_cast<uint16_t>(item.length_b));
        }
    }
    else
    {
        Save(list, kDmemWetLeft, item.downsample_left, kTwoChannels);
        reverb.resample_flags = 0;
    }
}

void EadMiddlePlayer::BuildUpdate(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update)
{
    const auto row = voices_.size() * static_cast<std::size_t>(update);
    const auto state_of = [&](int voice) -> UpdateState&
    {
        return update_states_[row + static_cast<std::size_t>(voice)];
    };
    Clear(list, kDmemLeft, kTwoChannels);
    for (std::size_t r = 0; r < reverbs_.size(); r++)
    {
        LoadReverbSamples(list, reverbs_[r], length, update);
        for (int i = 0; i < config_.voices; i++)
        {
            UpdateState& state = state_of(i);
            if (state.enabled && state.reverb_index == r)
            {
                list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
                BuildVoice(list, memory, i, state, length, update);
            }
        }

        list.MarkVoicesEnd();
        SaveReverbSamples(list, reverbs_[r], update);
    }

    for (int i = 0; i < config_.voices; i++)
    {
        UpdateState& state = state_of(i);
        if (state.enabled && state.reverb_index >= reverbs_.size() && BankLoaded(state.sound_bank))
        {
            list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
            BuildVoice(list, memory, i, state, length, update);
        }
    }

    list.MarkChunkEnd();
    const auto size = static_cast<uint32_t>(length) * 2;
    SetBuffer(list, 0, kDmemTemp, size);
    list.commands.push_back({kOpInterleave << 24, Pair(kDmemLeft, kDmemRight)});
    Save(list, kDmemTemp, output, size * 2);
}

void EadMiddlePlayer::BuildVoice(AudioList& list, AudioMemory& memory, int voice_index, UpdateState& state, int length,
                                 int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    uint32_t flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        voice.restart = false;
        voice.position = 0;
        voice.position_fraction = 0;
        voice.current_left = 0;
        voice.current_right = 0;
        voice.previous_delay_right = 0;
        voice.previous_delay_left = 0;
    }

    const uint16_t pitch = state.pitch;
    const int parts = state.two_parts ? 2 : 1;
    const uint32_t fixed_length = static_cast<uint32_t>(int32_t{pitch} * length * 2) + voice.position_fraction;
    const auto to_load = static_cast<int32_t>(fixed_length >> 16);
    voice.position_fraction = static_cast<uint16_t>(fixed_length);
    uint32_t before_resampling = 0;
    if (state.synthetic_wave)
    {
        Load(list, state.wave_address, kDmemDecoded, kWaveSamples * 2);
        voice.position &= kWaveSamples - 1;
        const int available = kWaveSamples - voice.position;
        if (available < to_load)
        {
            const int copies = (to_load - available + kWaveSamples - 1) / kWaveSamples;
            if (copies != 0)
            {
                list.commands.push_back({Word0(kOpDuplicate, static_cast<uint32_t>(copies), kDmemDecoded),
                                         Pair(kDmemDecoded + kWaveSamples * 2, kWaveSamples * 2)});
            }
        }

        before_resampling = kDmemDecoded + static_cast<uint32_t>(voice.position) * 2;
        voice.position += to_load;
    }
    else
    {
        const EadSample* sample = SampleOf(state.sound_bank, state.sound);
        if (sample == nullptr || sample->loop == kNoObject || sample->book == kNoObject)
        {
            return;
        }

        EnsureBank(memory, state.sound_bank);
        const EadBank& bank = files_->banks[state.sound_bank];
        const BankLoop& loop = bank.loops[static_cast<std::size_t>(sample->loop)];
        const auto end_position = static_cast<int32_t>(loop.end);
        const uint32_t sample_address =
            AudioMemory::kImageBase + files_->sample_places[state.sound_bank] + sample->address;
        const uint32_t book_address = book_addresses_[state.sound_bank][static_cast<std::size_t>(sample->book)];
        uint32_t loaded_book = 0;
        int skip_bytes = 0;
        int resampled_length = 0;
        for (int part = 0; part < parts; part++)
        {
            int processed = 0;
            int written = 0;
            int wanted = to_load;
            if (parts != 1 && (to_load & 1) != 0)
            {
                wanted = (to_load & ~1) + part * 2;
            }

            if (loaded_book != book_address)
            {
                loaded_book = book_address;
                const VadpcmBook& book = bank.books[static_cast<std::size_t>(sample->book)].book;
                const auto entries = static_cast<uint32_t>(kFrameSamples * book.order * book.predictor_count);
                list.commands.push_back({(kOpLoadBook << 24) | entries, book_address + state.book_offset});
            }

            if (state.book_offset != 0)
            {
                // The engine then holds the place of another table. The next part loads the book again.
                loaded_book = 1;
            }

            while (processed != wanted)
            {
                bool note_finished = false;
                bool restart = false;
                int ignored_first = voice.position & 0xF;
                const int until_end = end_position - voice.position;
                const int to_process = wanted - processed;
                if (ignored_first == 0 && !voice.restart)
                {
                    ignored_first = kFrameSamples;
                }

                int in_first = kFrameSamples - ignored_first;
                int frames = 0;
                int to_decode = 0;
                int trailing = 0;
                if (to_process < until_end)
                {
                    frames = (to_process - in_first + kFrameSamples - 1) / kFrameSamples;
                    to_decode = frames * kFrameSamples;
                    trailing = in_first + to_decode - to_process;
                }
                else
                {
                    to_decode = until_end - in_first;
                    trailing = 0;
                    if (to_decode <= 0)
                    {
                        to_decode = 0;
                        in_first = until_end;
                    }

                    frames = (to_decode + kFrameSamples - 1) / kFrameSamples;
                    if (loop.count != 0)
                    {
                        restart = true;
                    }
                    else
                    {
                        note_finished = true;
                    }
                }

                uint32_t start_pad = 0;
                const int aligned = Align16(frames * 9 + kFrameSamples);
                const uint32_t compressed = kDmemCompressedEnd - static_cast<uint32_t>(aligned);
                if (frames != 0)
                {
                    const int frame = (voice.position - ignored_first + kFrameSamples) / kFrameSamples;
                    const uint32_t data = sample_address + static_cast<uint32_t>(frame * 9);
                    start_pad = data & 0xF;
                    Load(list, data - start_pad, compressed, static_cast<uint32_t>(aligned));
                }
                else
                {
                    to_decode = 0;
                }

                if (voice.restart)
                {
                    list.commands.push_back(
                        {kOpSetLoop << 24, loop_addresses_[state.sound_bank][static_cast<std::size_t>(sample->loop)]});
                    flags = kFlagLoop;
                    voice.restart = false;
                }

                const int this_time = to_decode + in_first - trailing;
                const int moved_to = Align16(written + kFrameSamples);
                if (processed == 0)
                {
                    SetBuffer(list, compressed + start_pad, kDmemDecoded, static_cast<uint32_t>(to_decode) * 2);
                    list.commands.push_back({Word0(kOpAdpcm, flags, 0), voice.adpcm_state});
                    skip_bytes = ignored_first * 2;
                }
                else
                {
                    SetBuffer(list, compressed + start_pad, kDmemDecoded + static_cast<uint32_t>(moved_to),
                              static_cast<uint32_t>(to_decode) * 2);
                    list.commands.push_back({Word0(kOpAdpcm, flags, 0), voice.adpcm_state});
                    Move(list,
                         kDmemDecoded + static_cast<uint32_t>(moved_to) + static_cast<uint32_t>(ignored_first) * 2,
                         kDmemDecoded + static_cast<uint32_t>(written), static_cast<uint32_t>(this_time) * 2);
                }

                processed += this_time;
                if (flags == kFlagInit)
                {
                    skip_bytes = kFrameSamples * 2;
                    written = to_decode * 2 + kFrameSamples * 2;
                }
                else if (flags == kFlagLoop)
                {
                    written = this_time * 2 + written;
                }
                else if (written != 0)
                {
                    written = this_time * 2 + written;
                }
                else
                {
                    written = (ignored_first + this_time) * 2;
                }

                flags = kFlagContinue;
                if (note_finished)
                {
                    Clear(list, kDmemDecoded + static_cast<uint32_t>(written),
                          static_cast<uint32_t>(wanted - processed) * 2);
                    state.finished = true;
                    voice.state.finished = true;
                    voice.state.enabled = false;
                    for (int later = update + 1; later < updates_per_frame_; later++)
                    {
                        UpdateState& next = update_states_[voices_.size() * static_cast<std::size_t>(later) +
                                                           static_cast<std::size_t>(voice_index)];
                        if (next.needs_init)
                        {
                            break;
                        }

                        next.enabled = false;
                    }

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
                before_resampling = kDmemDecoded + static_cast<uint32_t>(skip_bytes);
            }
            else if (part == 0)
            {
                Halve(list, static_cast<uint32_t>(Align8(wanted / 2)), kDmemDecoded + static_cast<uint32_t>(skip_bytes),
                      kDmemResampled);
                resampled_length = wanted;
                before_resampling = kDmemResampled;
                if (state.finished)
                {
                    Clear(list, before_resampling + static_cast<uint32_t>(resampled_length),
                          static_cast<uint32_t>(wanted) + 0x10);
                }
            }
            else
            {
                Halve(list, static_cast<uint32_t>(Align8(wanted / 2)), kDmemDecoded + static_cast<uint32_t>(skip_bytes),
                      static_cast<uint32_t>(resampled_length) + kDmemResampled);
            }

            if (state.finished)
            {
                break;
            }
        }
    }

    flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        state.needs_init = false;
    }

    const auto size = static_cast<uint32_t>(length) * 2;
    SetBuffer(list, before_resampling, kDmemTemp, size);
    Resample(list, flags, pitch, voice.resample_state);
    int side = 0;
    if (state.delay_right != 0 || voice.previous_delay_right != 0)
    {
        side = 1;
    }
    else if (state.delay_left != 0 || voice.previous_delay_left != 0)
    {
        side = 2;
    }

    BuildEnvelope(list, state, voice, length, side);
    if (state.headset_effects)
    {
        BuildDelay(list, state, voice, static_cast<int>(size), flags, side);
    }
}

void EadMiddlePlayer::BuildEnvelope(AudioList& list, const UpdateState& state, Voice& voice, int length, int side)
{
    const uint16_t source_left = voice.current_left;
    const uint16_t source_right = voice.current_right;
    const auto target_left = static_cast<uint16_t>(state.target_left << 4);
    const auto target_right = static_cast<uint16_t>(state.target_right << 4);
    const int groups = length >> 3;
    const auto ramp_left = static_cast<int16_t>((int32_t{target_left} - source_left) / groups);
    const auto ramp_right = static_cast<int16_t>((int32_t{target_right} - source_right) / groups);
    voice.current_left = static_cast<uint16_t>(source_left + ramp_left * groups);
    voice.current_right = static_cast<uint16_t>(source_right + ramp_right * groups);
    uint32_t dry_left = kDmemLeft;
    uint32_t dry_right = kDmemRight;
    if (state.headset_effects)
    {
        Clear(list, kDmemPanTemp, kOneChannel);
        if (side == 1)
        {
            dry_left = kDmemPanTemp;
        }
        else if (side == 2)
        {
            dry_right = kDmemPanTemp;
        }
    }

    list.commands.push_back({Word0(kOpEnvSetup1, state.reverb, ((source_left & 0xFFU) << 8) | (source_right & 0xFFU)),
                             Pair(static_cast<uint16_t>(ramp_left), static_cast<uint16_t>(ramp_right))});
    list.commands.push_back({kOpEnvSetup2 << 24, Pair(source_left, source_right)});
    const uint32_t bits = (uint32_t{state.strong_right} << 1) | uint32_t{state.strong_left};
    list.commands.push_back(
        {(kOpEnvMixer << 24) | ((static_cast<uint32_t>(length) & 0xFF) << 8) | bits,
         ((dry_left >> 4) << 24) | ((dry_right >> 4) << 16) | ((kDmemWetLeft >> 4) << 8) | (kDmemWetRight >> 4)});
}

void EadMiddlePlayer::BuildDelay(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags,
                                 int side)
{
    uint32_t target = 0;
    uint32_t shift = 0;
    uint32_t previous_shift = 0;
    if (side == 1)
    {
        target = kDmemLeft;
        shift = state.delay_right;
        voice.previous_delay_left = 0;
        previous_shift = voice.previous_delay_right;
        voice.previous_delay_right = static_cast<uint8_t>(shift);
    }
    else if (side == 2)
    {
        target = kDmemRight;
        shift = state.delay_left;
        voice.previous_delay_right = 0;
        previous_shift = voice.previous_delay_left;
        voice.previous_delay_left = static_cast<uint8_t>(shift);
    }
    else
    {
        return;
    }

    const auto bytes = static_cast<uint32_t>(size);
    const uint32_t shifted = shift + bytes - previous_shift;
    if (flags != kFlagInit)
    {
        // A small change of rate fits a change of the delay.
        if (previous_shift == 0)
        {
            Move(list, kDmemPanTemp, 0, 8);
            Clear(list, 8, 8);
            Move(list, kDmemPanTemp, 0x10, 0x10);
            Save(list, 0, voice.pan_resample_state, 0x20);
            const uint32_t divisor = bytes + shift - previous_shift + 8;
            SetBuffer(list, kDmemPanTemp + 8, 0, shifted);
            Resample(list, 0, ((bytes << 15) / divisor) & 0xFFFF, voice.pan_resample_state);
            Move(list, 0, kDmemPanTemp, shifted);
        }
        else
        {
            const uint32_t divisor = shift == 0 ? bytes - previous_shift - 4 : shifted;
            SetBuffer(list, kDmemPanTemp, 0, shifted);
            Resample(list, 0, ((bytes << 15) / divisor) & 0xFFFF, voice.pan_resample_state);
            Load(list, voice.pan_samples, kDmemPanTemp, previous_shift);
            Move(list, 0, kDmemPanTemp + previous_shift, shifted);
        }
    }
    else
    {
        // The delay only moves the samples to the right.
        Move(list, kDmemPanTemp, 0, bytes);
        Move(list, 0, kDmemPanTemp + shift, bytes);
        Clear(list, kDmemPanTemp, shift);
    }

    if (shift != 0)
    {
        Save(list, kDmemPanTemp + bytes, voice.pan_samples, shift);
    }

    Mix(list, static_cast<uint32_t>((size + 31) & ~31), 0x7FFF, kDmemPanTemp, target);
}

void EadMiddlePlayer::BuildRampTables()
{
    // The code at 0x800B7450 in Wave Race 64. An envelope's ramp over an update multiplies a volume by a factor in each
    // group of 8 samples. Each table holds a volume's root for an update of a size: 8 samples shorter, the usual size
    // and 8 samples longer.
    constexpr std::size_t kSteps = 1024;
    constexpr int kVolumeStep = 32;
    constexpr double kScale = 65536.0;
    const int groups = samples_per_update_ / 8;
    for (std::size_t table = 0; table < ramp_up_.size(); table++)
    {
        const int k = groups - 1 + static_cast<int>(table);
        ramp_up_[table].resize(kSteps);
        ramp_down_[table].resize(kSteps);
        for (std::size_t i = 0; i < kSteps; i++)
        {
            const int volume = i == 0 ? 1 : static_cast<int>(i) * kVolumeStep;
            ramp_up_[table][i] = static_cast<float>(KthRoot(volume, k));
            ramp_down_[table][i] = static_cast<float>(KthRoot(1.0 / volume, k) * kScale);
        }
    }
}

void EadMiddlePlayer::LoadReverbStandard(AudioList& list, const Reverb& reverb, int length, int update) const
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    const auto load = [&](uint32_t dmem, int offset, uint32_t size)
    {
        const auto bytes = static_cast<uint32_t>(offset) * 2;
        list.SetBuffer(0, dmem, 0, size);
        list.LoadBuffer(reverb.left + bytes);
        list.SetBuffer(0, dmem + kStdOneChannel, 0, size);
        list.LoadBuffer(reverb.right + bytes);
    };
    list.ClearBuffer(kStdWetLeft, kStdTwoChannels);
    if (reverb.downsample == 1)
    {
        load(kStdWetLeft, item.start, static_cast<uint16_t>(item.length_a));
        if (item.length_b != 0)
        {
            load(kStdWetLeft + static_cast<uint32_t>(item.length_a), 0, static_cast<uint16_t>(item.length_b));
        }
    }
    else
    {
        const int start_pad = (item.start % 8) * 2;
        const int padded = Align16(start_pad + item.length_a);
        load(kStdResampled, item.start - start_pad / 2, kStdOneChannel);
        if (item.length_b != 0)
        {
            load(kStdResampled + static_cast<uint32_t>(padded), 0, kStdOneChannel - static_cast<uint32_t>(padded));
        }

        const auto pad = static_cast<uint32_t>(start_pad);
        const auto size = static_cast<uint32_t>(length) * 2;
        list.SetBuffer(0, kStdResampled + pad, kStdWetLeft, size);
        list.Resample(reverb.resample_flags, reverb.downsample_pitch, reverb.resample_left);
        list.SetBuffer(0, kStdResampled2 + pad, kStdWetRight, size);
        list.Resample(reverb.resample_flags, reverb.downsample_pitch, reverb.resample_right);
    }

    list.SetBuffer(0, 0, 0, kStdTwoChannels);
    list.Mixer(0, 0x7FFF, kStdWetLeft, kStdLeft);
    list.Mixer(0, 0x8000U + reverb.gain, kStdWetLeft, kStdWetLeft);
}

void EadMiddlePlayer::SaveReverbStandard(AudioList& list, Reverb& reverb, int update) const
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample == 1)
    {
        const auto save = [&](uint32_t dmem, int offset, uint32_t size)
        {
            const auto bytes = static_cast<uint32_t>(offset) * 2;
            list.SetBuffer(0, 0, dmem, size);
            list.SaveBuffer(reverb.left + bytes);
            list.SetBuffer(0, 0, dmem + kStdOneChannel, size);
            list.SaveBuffer(reverb.right + bytes);
        };
        save(kStdWetLeft, item.start, static_cast<uint16_t>(item.length_a));
        if (item.length_b != 0)
        {
            save(kStdWetLeft + static_cast<uint32_t>(item.length_a), 0, static_cast<uint16_t>(item.length_b));
        }
    }
    else
    {
        list.SetBuffer(0, 0, kStdWetLeft, kStdTwoChannels);
        list.SaveBuffer(item.downsample_left);
        reverb.resample_flags = 0;
    }
}

void EadMiddlePlayer::BuildUpdateStandard(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update)
{
    // The code at 0x800B59A8 in Wave Race 64.
    const auto row = voices_.size() * static_cast<std::size_t>(update);
    const auto state_of = [&](int voice) -> UpdateState&
    {
        return update_states_[row + static_cast<std::size_t>(voice)];
    };
    list.ClearBuffer(kStdLeft, kStdTwoChannels);
    bool use_reverb = false;
    for (std::size_t r = 0; r < reverbs_.size(); r++)
    {
        use_reverb = config_.reverbs[r].in_use;
        if (use_reverb)
        {
            LoadReverbStandard(list, reverbs_[r], length, update);
        }

        for (int i = 0; i < config_.voices; i++)
        {
            UpdateState& state = state_of(i);
            if (state.enabled && state.reverb_index == r)
            {
                list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
                standard_reverb_ = use_reverb;
                BuildVoiceStandard(list, memory, i, state, length);
            }
        }

        list.MarkVoicesEnd();
        if (use_reverb)
        {
            SaveReverbStandard(list, reverbs_[r], update);
        }
    }

    for (int i = 0; i < config_.voices; i++)
    {
        UpdateState& state = state_of(i);
        if (state.enabled && state.reverb_index >= reverbs_.size() && BankLoaded(state.sound_bank))
        {
            list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
            standard_reverb_ = use_reverb;
            BuildVoiceStandard(list, memory, i, state, length);
        }
    }

    list.MarkChunkEnd();
    const auto size = static_cast<uint32_t>(length) * 2;
    list.SetBuffer(0, 0, kDmemTemp, size);
    list.Interleave(kStdLeft, kStdRight);
    list.SetBuffer(0, 0, kDmemTemp, size * 2);
    list.SaveBuffer(output);
}

void EadMiddlePlayer::BuildVoiceStandard(AudioList& list, AudioMemory& memory, int voice_index, UpdateState& state,
                                         int length)
{
    // The code at 0x800B5E70 in Wave Race 64.
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    if (!voice.state.enabled)
    {
        return;
    }

    uint8_t flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        voice.restart = false;
        voice.position = 0;
        voice.position_fraction = 0;
        voice.current_left = 1;
        voice.current_right = 1;
        voice.previous_delay_right = 0;
        voice.previous_delay_left = 0;
    }

    const uint16_t pitch = state.pitch;
    const int parts = state.two_parts ? 2 : 1;
    const uint32_t fixed_length = static_cast<uint32_t>(int32_t{pitch} * length * 2) + voice.position_fraction;
    const auto to_load = static_cast<int32_t>(fixed_length >> 16);
    voice.position_fraction = static_cast<uint16_t>(fixed_length);
    uint32_t before_resampling = 0;
    if (state.synthetic_wave)
    {
        list.SetBuffer(0, kStdDecoded, 0, kWaveSamples * 2);
        list.LoadBuffer(state.wave_address);
        voice.position &= kWaveSamples - 1;
        const int available = kWaveSamples - voice.position;
        if (available < to_load)
        {
            const int copies = (to_load - available + kWaveSamples - 1) / kWaveSamples;
            for (int i = 0; i < copies; i++)
            {
                list.DmemMove(kStdDecoded, kStdDecoded + static_cast<uint32_t>(1 + i) * kWaveSamples * 2,
                              kWaveSamples * 2);
            }
        }

        before_resampling = kStdDecoded + static_cast<uint32_t>(voice.position) * 2;
        voice.position += to_load;
    }
    else
    {
        const EadSample* sample = SampleOf(state.sound_bank, state.sound);
        if (sample == nullptr || sample->loop == kNoObject || sample->book == kNoObject)
        {
            return;
        }

        EnsureBank(memory, state.sound_bank);
        const EadBank& bank = files_->banks[state.sound_bank];
        const BankLoop& loop = bank.loops[static_cast<std::size_t>(sample->loop)];
        const auto end_position = static_cast<int32_t>(loop.end);
        const uint32_t sample_address =
            AudioMemory::kImageBase + files_->sample_places[state.sound_bank] + sample->address;
        const uint32_t book_address = book_addresses_[state.sound_bank][static_cast<std::size_t>(sample->book)];
        uint32_t loaded_book = 0;
        uint32_t skip_bytes = 0;
        uint32_t resampled_length = 0;
        for (int part = 0; part < parts; part++)
        {
            int processed = 0;
            int written = 0;
            int wanted = to_load;
            if (parts != 1 && (to_load & 1) != 0)
            {
                wanted = (to_load & ~1) + part * 2;
            }

            if (loaded_book != book_address)
            {
                loaded_book = book_address;
                const VadpcmBook& book = bank.books[static_cast<std::size_t>(sample->book)].book;
                const auto entries = static_cast<uint32_t>(kFrameSamples * book.order * book.predictor_count);
                list.LoadAdpcm(entries, book_address + state.book_offset);
            }

            if (state.book_offset != 0)
            {
                // The engine then holds the place of another table. The next part loads the book again.
                loaded_book = 1;
            }

            while (processed != wanted)
            {
                bool note_finished = false;
                bool restart = false;
                int ignored_first = voice.position & 0xF;
                const int until_end = end_position - voice.position;
                const int to_process = wanted - processed;
                if (ignored_first == 0 && !voice.restart)
                {
                    ignored_first = kFrameSamples;
                }

                int in_first = kFrameSamples - ignored_first;
                int frames = 0;
                int to_decode = 0;
                int trailing = 0;
                if (to_process < until_end)
                {
                    frames = (to_process - in_first + kFrameSamples - 1) / kFrameSamples;
                    to_decode = frames * kFrameSamples;
                    trailing = in_first + to_decode - to_process;
                }
                else
                {
                    to_decode = until_end - in_first;
                    trailing = 0;
                    if (to_decode <= 0)
                    {
                        to_decode = 0;
                        in_first = until_end;
                    }

                    frames = (to_decode + kFrameSamples - 1) / kFrameSamples;
                    if (loop.count != 0)
                    {
                        restart = true;
                    }
                    else
                    {
                        note_finished = true;
                    }
                }

                uint32_t start_pad = 0;
                if (frames != 0)
                {
                    const int frame = (voice.position - ignored_first + kFrameSamples) / kFrameSamples;
                    const uint32_t data = sample_address + static_cast<uint32_t>(frame * 9);
                    start_pad = data & 0xF;
                    list.SetBuffer(0, kStdCompressed, 0, static_cast<uint32_t>(frames * 9) + start_pad);
                    list.LoadBuffer(data - start_pad);
                }
                else
                {
                    to_decode = 0;
                }

                if (voice.restart)
                {
                    list.SetLoop(loop_addresses_[state.sound_bank][static_cast<std::size_t>(sample->loop)]);
                    flags = kFlagLoop;
                    voice.restart = false;
                }

                const int this_time = to_decode + in_first - trailing;
                const auto written_bytes = static_cast<uint32_t>(written);
                if (processed == 0)
                {
                    list.SetBuffer(0, kStdCompressed + start_pad, kStdDecoded, static_cast<uint32_t>(to_decode) * 2);
                    list.Adpcm(flags, voice.adpcm_state);
                    skip_bytes = static_cast<uint32_t>(ignored_first) * 2;
                }
                else
                {
                    const uint32_t moved_to = (written_bytes + 31) & ~31U;
                    list.SetBuffer(0, kStdCompressed + start_pad, kStdDecoded + moved_to,
                                   static_cast<uint32_t>(to_decode) * 2);
                    list.Adpcm(flags, voice.adpcm_state);
                    list.DmemMove(kStdDecoded + moved_to + static_cast<uint32_t>(ignored_first) * 2,
                                  kStdDecoded + written_bytes, static_cast<uint32_t>(this_time) * 2);
                }

                processed += this_time;
                if (flags == kFlagInit)
                {
                    skip_bytes = 0;
                    written = to_decode * 2 + written;
                }
                else if (flags == kFlagLoop)
                {
                    written = this_time * 2 + written;
                }
                else if (written != 0)
                {
                    written = this_time * 2 + written;
                }
                else
                {
                    written = (ignored_first + this_time) * 2;
                }

                flags = kFlagContinue;
                if (note_finished)
                {
                    list.ClearBuffer(kStdDecoded + static_cast<uint32_t>(written),
                                     static_cast<uint32_t>(wanted - processed) * 2);
                    state.finished = true;
                    voice.state.finished = true;
                    voice.state.enabled = false;
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
                before_resampling = kStdDecoded + skip_bytes;
            }
            else if (part == 0)
            {
                constexpr uint32_t kHalvingPitch = 0xFF60;
                const auto bytes = static_cast<uint32_t>(wanted);
                list.SetBuffer(0, kStdDecoded + skip_bytes, kStdResampled, bytes + 4);
                list.Resample(kFlagInit, kHalvingPitch, voice.halving_state);
                resampled_length = bytes + 4;
                before_resampling = kStdResampled + 4;
                if (state.finished)
                {
                    list.ClearBuffer(kStdResampled + resampled_length, bytes + 0x10);
                }
            }
            else
            {
                constexpr uint32_t kHalvingPitch = 0xFF60;
                const auto bytes = static_cast<uint32_t>(wanted);
                list.SetBuffer(0, kStdDecoded + skip_bytes, kStdResampled2, bytes + 8);
                list.Resample(kFlagInit, kHalvingPitch, voice.halving_state);
                list.DmemMove(kStdResampled2 + 4, kStdResampled + resampled_length, bytes + 4);
            }

            if (state.finished)
            {
                break;
            }
        }
    }

    flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        state.needs_init = false;
    }

    const auto size = static_cast<uint32_t>(length) * 2;
    list.SetBuffer(0, before_resampling, kDmemTemp, size);
    list.Resample(flags, pitch, voice.resample_state);
    int side = 0;
    if (state.delay_right != 0 || voice.previous_delay_right != 0)
    {
        side = 1;
    }
    else if (state.delay_left != 0 || voice.previous_delay_left != 0)
    {
        side = 2;
    }

    BuildEnvelopeStandard(list, state, voice, length, side, standard_reverb_);
    if (state.headset_effects)
    {
        BuildDelayStandard(list, state, voice, static_cast<int>(size), flags, side);
    }
}

void EadMiddlePlayer::BuildEnvelopeStandard(AudioList& list, const UpdateState& state, Voice& voice, int length,
                                            int side, bool use_reverb)
{
    // The code at 0x800B6900 in Wave Race 64.
    const uint16_t source_left = voice.current_left;
    const uint16_t source_right = voice.current_right;
    auto target_left = static_cast<uint16_t>(state.target_left << 5);
    auto target_right = static_cast<uint16_t>(state.target_right << 5);
    if (target_left == 0)
    {
        target_left = 1;
    }

    if (target_right == 0)
    {
        target_right = 1;
    }

    voice.current_left = target_left;
    voice.current_right = target_right;
    const auto size = static_cast<uint32_t>(length) * 2;
    if (state.headset_effects)
    {
        list.ClearBuffer(kStdPanTemp, kStdOneChannel);
        list.SetBuffer(0, kDmemTemp, side == 1 ? kStdPanTemp : kStdLeft, size);
        list.SetBuffer(kAudioFlagAux, side == 2 ? kStdPanTemp : kStdRight, kStdWetLeft, kStdWetRight);
    }
    else if (state.strong_right)
    {
        list.ClearBuffer(kStdStrongDry, kStdTwoChannels);
        list.SetBuffer(0, kDmemTemp, kStdStrongDry, size);
        list.SetBuffer(kAudioFlagAux, kStdRight, kStdStrongWet, kStdWetRight);
    }
    else if (state.strong_left)
    {
        list.ClearBuffer(kStdStrongDry, kStdTwoChannels);
        list.SetBuffer(0, kDmemTemp, kStdLeft, size);
        list.SetBuffer(kAudioFlagAux, kStdStrongDry, kStdWetLeft, kStdStrongWet);
    }
    else
    {
        list.SetBuffer(0, kDmemTemp, kStdLeft, size);
        list.SetBuffer(kAudioFlagAux, kStdRight, kStdWetLeft, kStdWetRight);
    }

    uint8_t flags = kFlagContinue;
    if (target_left != source_left || target_right != source_right || state.mixer_needs_init)
    {
        flags = kFlagInit;
        const std::vector<float>& up = ramp_up_[static_cast<std::size_t>(ramp_table_)];
        const std::vector<float>& down = ramp_down_[static_cast<std::size_t>(ramp_table_)];
        const float ramp_left = up[target_left >> 5U] * down[source_left >> 5U];
        const float ramp_right = up[target_right >> 5U] * down[source_right >> 5U];
        list.SetVolume(kAudioFlagVolume | kAudioFlagLeft, source_left, 0, 0);
        list.SetVolume(kAudioFlagVolume, source_right, 0, 0);
        list.commands.push_back({(uint32_t{kAudioSetVolume} << 24) | (uint32_t{kAudioFlagLeft} << 16) | target_left,
                                 static_cast<uint32_t>(static_cast<int32_t>(ramp_left))});
        list.commands.push_back({(uint32_t{kAudioSetVolume} << 24) | target_right,
                                 static_cast<uint32_t>(static_cast<int32_t>(ramp_right))});
        list.SetVolume(kAudioFlagAux, config_.dry_gain, 0, uint32_t{state.reverb} << 8);
    }

    const bool wet = use_reverb && state.reverb != 0;
    list.EnvMixer(wet ? static_cast<uint8_t>(flags | kAudioFlagAux) : flags, voice.mixer_state);
    if (state.strong_right || state.strong_left)
    {
        constexpr uint32_t kSubtract = 0x8000;
        list.SetBuffer(0, 0, 0, size);
        list.Mixer(0, kSubtract, kStdStrongDry, state.strong_right ? kStdLeft : kStdRight);
        if (wet)
        {
            list.Mixer(0, kSubtract, kStdStrongWet, state.strong_right ? kStdWetLeft : kStdWetRight);
        }
    }
}

void EadMiddlePlayer::BuildDelayStandard(AudioList& list, const UpdateState& state, Voice& voice, int size,
                                         uint32_t flags, int side)
{
    // The code at 0x800B6F18 in Wave Race 64.
    uint32_t target = 0;
    uint32_t shift = 0;
    uint32_t previous_shift = 0;
    if (side == 1)
    {
        target = kStdLeft;
        shift = state.delay_right;
        voice.previous_delay_left = 0;
        previous_shift = voice.previous_delay_right;
        voice.previous_delay_right = static_cast<uint8_t>(shift);
    }
    else if (side == 2)
    {
        target = kStdRight;
        shift = state.delay_left;
        voice.previous_delay_right = 0;
        previous_shift = voice.previous_delay_left;
        voice.previous_delay_left = static_cast<uint8_t>(shift);
    }
    else
    {
        return;
    }

    const auto bytes = static_cast<uint32_t>(size);
    const uint32_t shifted = shift + bytes - previous_shift;
    if (flags != kFlagInit)
    {
        // A small change of rate fits a change of the delay.
        if (previous_shift == 0)
        {
            list.DmemMove(kStdPanTemp, kDmemTemp, 8);
            list.ClearBuffer(8, 8);
            list.DmemMove(kStdPanTemp, kDmemTemp + 0x10, 0x10);
            list.SetBuffer(0, 0, kDmemTemp, 32);
            list.SaveBuffer(voice.pan_resample_state);
            const uint32_t divisor = bytes + shift - previous_shift + 8;
            list.SetBuffer(0, kStdPanTemp + 8, kDmemTemp, shifted);
            list.Resample(0, ((bytes << 15) / divisor) & 0xFFFF, voice.pan_resample_state);
        }
        else
        {
            const uint32_t divisor = shift == 0 ? bytes - previous_shift - 4 : shifted;
            list.SetBuffer(0, kStdPanTemp, kDmemTemp, shifted);
            list.Resample(0, ((bytes << 15) / divisor) & 0xFFFF, voice.pan_resample_state);
        }

        if (previous_shift != 0)
        {
            list.SetBuffer(0, kStdPanTemp, 0, previous_shift);
            list.LoadBuffer(voice.pan_samples);
            list.DmemMove(kDmemTemp, kStdPanTemp + previous_shift, shifted);
        }
        else
        {
            list.DmemMove(kDmemTemp, kStdPanTemp, shifted);
        }
    }
    else
    {
        // The delay only moves the samples to the right.
        list.DmemMove(kStdPanTemp, kDmemTemp, bytes);
        list.DmemMove(kDmemTemp, kStdPanTemp + shift, bytes);
        list.ClearBuffer(kStdPanTemp, shift);
    }

    if (shift != 0)
    {
        list.SetBuffer(0, 0, kStdPanTemp + bytes, shift);
        list.SaveBuffer(voice.pan_samples);
    }

    list.SetBuffer(0, 0, 0, bytes);
    list.Mixer(0, 0x7FFF, kStdPanTemp, target);
}

bool EadMiddlePlayer::Busy() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return player.enabled; }) ||
           std::ranges::any_of(voices_, [](const Voice& voice) { return voice.state.enabled; });
}

void EadMiddlePlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    for (int update = 0; update < updates_per_frame_; update++)
    {
        for (int player = 0; player < kPlayers; player++)
        {
            if (players_[static_cast<std::size_t>(player)].enabled)
            {
                RunPlayer(player);
                if (players_[static_cast<std::size_t>(player)].enabled)
                {
                    PlayerSound(player);
                }
            }
        }

        ProcessVoices();
        const std::size_t row = voices_.size() * static_cast<std::size_t>(update);
        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            Voice& voice = voices_[i];
            if (voice.state.enabled)
            {
                update_states_[row + i] = voice.state;
                voice.state.needs_init = false;
            }
            else
            {
                update_states_[row + i].enabled = false;
            }
        }

        updates_++;
    }

    list.commands.push_back({kOpSegment << 24, 0});
    uint32_t address = output;
    int remaining = samples;
    for (int left = updates_per_frame_; left > 0; left--)
    {
        int length = 0;
        if (left == 1)
        {
            length = remaining;
        }
        else if (remaining / left >= samples_per_update_max_)
        {
            length = samples_per_update_max_;
            ramp_table_ = 2;
        }
        else if (remaining / left <= samples_per_update_min_)
        {
            length = samples_per_update_min_;
            ramp_table_ = 0;
        }
        else
        {
            length = samples_per_update_;
            ramp_table_ = 1;
        }

        const int update = updates_per_frame_ - left;
        for (std::size_t r = 0; r < reverbs_.size(); r++)
        {
            if (config_.reverbs[r].in_use)
            {
                StartReverbItem(memory, reverbs_[r], length, update);
            }
        }

        if (config_.standard_microcode)
        {
            BuildUpdateStandard(list, memory, address, length, update);
        }
        else
        {
            BuildUpdate(list, memory, address, length, update);
        }

        remaining -= length;
        address += static_cast<uint32_t>(length) * 4;
    }

    for (Reverb& reverb : reverbs_)
    {
        if (reverb.frames_to_ignore != 0)
        {
            reverb.frames_to_ignore--;
        }

        reverb.current_frame ^= 1;
    }
}

} // namespace ntdlib::n64

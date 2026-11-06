// SPDX-License-Identifier: MIT

#include "ntdlib/n64/ead_later.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ead_later_tables.h"
#include "ntdlib/n64/rom_data.h"
#include "round_toward_zero.h"

namespace ntdlib::n64
{

namespace
{

namespace tables = ead_later_tables;

constexpr uint32_t kSurroundTemp = 0x4B0;
constexpr uint32_t kSmallTemp = 0x3C0;
constexpr uint32_t kSurroundStateSize = 0x100;
constexpr uint32_t kOneChannel = 0x1A0;
constexpr uint32_t kTwoChannels = 0x340;

constexpr uint32_t kFlagContinue = 0;
constexpr uint32_t kFlagInit = 1;
constexpr uint32_t kFlagLoop = 2;

constexpr int kFrameSamples = 16;
constexpr int kWaveSamples = 64;

// The microcode's opcodes.
constexpr uint32_t kOpAdpcm = 0x01;
constexpr uint32_t kOpClear = 0x02;
constexpr uint32_t kOpMultiply = 0x03;
constexpr uint32_t kOpAddMixer = 0x04;
constexpr uint32_t kOpResample = 0x05;
constexpr uint32_t kOpResampleNearest = 0x06;
constexpr uint32_t kOpFilter = 0x07;
constexpr uint32_t kOpSetBuffer = 0x08;
constexpr uint32_t kOpDuplicate = 0x09;
constexpr uint32_t kOpMove = 0x0A;
constexpr uint32_t kOpLoadBook = 0x0B;
constexpr uint32_t kOpMixer = 0x0C;
constexpr uint32_t kOpInterleave = 0x0D;
constexpr uint32_t kOpGain = 0x0E;
constexpr uint32_t kOpSetLoop = 0x0F;
constexpr uint32_t kOpHalve = 0x11;
constexpr uint32_t kOpEnvSetup1 = 0x12;
constexpr uint32_t kOpEnvMixer = 0x13;
constexpr uint32_t kOpLoad = 0x14;
constexpr uint32_t kOpSave = 0x15;
constexpr uint32_t kOpEnvSetup2 = 0x16;
constexpr uint32_t kOpDecodeBytes = 0x17;
constexpr uint32_t kOpNegate = 0x19;

// The value by which the flow commands report a script's end.
constexpr int kScriptEnd = -1;

constexpr uint8_t kMuteStopVoices = 1U << 3;
constexpr uint8_t kMuteStopLayerNotes = 1U << 4;
constexpr uint8_t kMuteSoften = 1U << 5;
constexpr uint8_t kMuteStopNotes = 1U << 6;
constexpr uint8_t kMuteStopScript = 1U << 7;

constexpr uint8_t kPortamentoSpecial = 0x80;

// A layer's flags in Majora's Mask, all set at the start. A script can clear them.
constexpr uint16_t kLayerUsesChannelReverb = 0x2000;
constexpr uint16_t kLayerFlagVolume = 0x8000;
constexpr uint16_t kLayerFlagFrequency = 0x4000;
constexpr uint16_t kLayerFlagPan = 0x0080;
constexpr uint16_t kLayerUsesChannelVibrato = 0x1000;
constexpr uint16_t kLayerUsesChannelGain = 0x0040;

constexpr int32_t kLayerVibratoRate = 0x800;

constexpr uint8_t kCodecNoDecodeSix = 6;
constexpr uint8_t kCodecRawSeven = 7;

constexpr int kLayerCountOcarina = 64;
constexpr int kLayerCountMajora = 80;
constexpr int kPoolCount = 1 + EadLaterPlayer::kPlayers + EadLaterPlayer::kPlayers * EadLaterPlayer::kChannelsPerPlayer;

constexpr int kTicksPerBeat = 48;
constexpr float kRefreshDeviation = 1.00278F;
constexpr int kRefreshRate = 60;
constexpr int kConsoleClock = 48681812;

// The number of arguments of each command from 0xB0 on, and which are 16 bits wide. The arguments of a channel's script
// take this form. The top bit is the first argument's width.
constexpr std::array<uint8_t, 80> kChannelArguments = {
    0x81, 0x00, 0x81, 0x01, 0x00, 0x00, 0x00, 0x81, 0x01, 0x01, 0x01, 0x42, 0x81, 0xC2, 0x00, 0x00, //
    0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x81, 0x81, //
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x01, 0x81, 0x01, //
    0x01, 0x03, 0x03, 0x01, 0x00, 0x01, 0x01, 0x81, 0x03, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82, //
    0x00, 0x01, 0x01, 0x01, 0x01, 0x81, 0x00, 0x00, 0x01, 0x81, 0x81, 0x81, 0x81, 0x00, 0x00, 0x00, //
};

constexpr int Align8(int value)
{
    return (value + 7) & ~7;
}

constexpr int Align16(int value)
{
    return (value + 15) & ~15;
}

constexpr int Align64(int value)
{
    return (value + 63) & ~63;
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

void Filter(AudioList& list, uint32_t flags, uint32_t size_or_buffer, uint32_t address)
{
    list.commands.push_back({Word0(kOpFilter, flags, size_or_buffer), address});
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

constexpr std::array<EadEnvelope::Point, 4> kDefaultEnvelope = {
    {{1, 32000}, {1000, 32000}, {kEnvelopeHang, 0}, {kEnvelopeDisable, 0}}};

float CalculateDecay(float inverse_scaled, float scale)
{
    return (256.0F * inverse_scaled) / scale;
}

} // namespace

std::optional<EadLaterFiles> EadLaterFiles::Read(std::span<const uint8_t> image)
{
    const std::optional<LaterSoundData> data = FindLaterSoundData(image);
    if (!data)
    {
        return std::nullopt;
    }

    const std::span<const uint8_t> code(data->code);
    const EadTable banks = EadTable::Parse(code.subspan(data->bank_table));
    const EadTable sequences = EadTable::Parse(code.subspan(data->sequence_table));
    const EadTable samples = EadTable::Parse(code.subspan(data->sample_table));

    EadLaterFiles files;
    files.sequences.resize(sequences.entries.size());
    for (std::size_t i = 0; i < sequences.entries.size(); i++)
    {
        const EadTable::Entry& entry = sequences.entries[i];
        if (entry.size != 0)
        {
            const std::span<const uint8_t> bytes = image.subspan(data->sequences + entry.address, entry.size);
            files.sequences[i].assign(bytes.begin(), bytes.end());
        }
    }

    for (std::size_t i = 0; i < sequences.entries.size(); i++)
    {
        const EadTable::Entry& entry = sequences.entries[i];
        if (entry.size == 0 && entry.address < files.sequences.size())
        {
            files.sequences[i] = files.sequences[entry.address];
        }
    }

    const auto place_of = [&](std::size_t index) -> uint32_t
    {
        if (index >= samples.entries.size())
        {
            return 0;
        }

        if (samples.entries[index].size == 0)
        {
            index = samples.entries[index].address;
        }

        return index < samples.entries.size() ? static_cast<uint32_t>(data->samples) + samples.entries[index].address
                                              : 0;
    };

    files.banks.resize(banks.entries.size());
    files.sample_places.resize(banks.entries.size());
    for (std::size_t i = 0; i < banks.entries.size(); i++)
    {
        const std::size_t real = banks.entries[i].size == 0 ? banks.entries[i].address : i;
        if (real >= banks.entries.size() || banks.entries[real].size == 0)
        {
            continue;
        }

        const EadTable::Entry& entry = banks.entries[real];
        files.banks[i] = EadBank::ParseLater(image.subspan(data->banks + entry.address, entry.size), entry.data2 >> 8,
                                             entry.data2 & 0xFF, entry.data3);
        files.sample_places[i] = {place_of(entry.data1 >> 8), place_of(entry.data1 & 0xFF)};
    }

    if (data->bank_set_table != 0)
    {
        const std::size_t sets_start = data->bank_set_table;
        files.bank_sets.resize(sequences.entries.size());
        for (std::size_t i = 0; i < sequences.entries.size(); i++)
        {
            const std::size_t place = sets_start + xsflib::LoadBe16(code.data() + sets_start + i * 2);
            if (place >= code.size())
            {
                continue;
            }

            const std::size_t count = code[place];
            if (place + 1 + count <= code.size())
            {
                files.bank_sets[i].assign(code.begin() + static_cast<std::ptrdiff_t>(place + 1),
                                          code.begin() + static_cast<std::ptrdiff_t>(place + 1 + count));
            }
        }
    }

    return files;
}

EadLaterPlayer::EadLaterPlayer(const EadLaterConfig& config, const EadLaterFiles& files, AudioMemory& memory)
    : config_(config),
      files_(&files),
      majora_(config.version != EadLaterVersion::kOcarina),
      stadium_2_(config.version == EadLaterVersion::kStadium2)
{
    // The code at 0x80001B4C and seven other places in Pokémon Stadium 2 sets the rounding mode.
    const RoundTowardZero rounding(stadium_2_);
    dmem_ = majora_ ? DmemLayout{0x3B0, 0x570, 0x5B0, 0x750, 0x930, 0x930, 0xAD0, 0x3D0, 0x710, 0xC70, 0xE10}
                    : DmemLayout{0x3C0, 0x580, 0x5C0, 0x760, 0x940, 0x940, 0xAE0, 0x3E0, 0x720, 0xC80, 0xE20};
    const int layer_count = majora_ ? kLayerCountMajora : kLayerCountOcarina;
    const int frequency = config.sampling_frequency;
    const auto dac_rate = static_cast<int>(static_cast<float>(kConsoleClock) / static_cast<float>(frequency) + 0.5F);
    ai_frequency_ = kConsoleClock / dac_rate;
    frame_target_ = Align16(frequency / kRefreshRate);
    updates_per_frame_ = (frame_target_ + 0x10) / 0xD0 + 1;
    samples_per_update_ = (frame_target_ / updates_per_frame_) & ~7;
    samples_per_update_max_ = samples_per_update_ + 8;
    samples_per_update_min_ = samples_per_update_ - 8;
    resample_rate_ = 32000.0F / static_cast<float>(frequency);
    updates_inverse_scaled_ = (1.0F / 256.0F) / static_cast<float>(updates_per_frame_);
    updates_scaled_ = static_cast<float>(updates_per_frame_) / 4.0F;
    updates_inverse_ = 1.0F / static_cast<float>(updates_per_frame_);

    const float tv_factor = 1000.0F * kRefreshDeviation / static_cast<float>(kRefreshRate);
    max_tempo_ = static_cast<int16_t>(
        static_cast<uint32_t>(static_cast<float>(updates_per_frame_) * static_cast<float>(60 * 1000 * kTicksPerBeat) /
                              static_cast<float>(kTicksPerBeat) / tv_factor));
    note_length_scale_ = static_cast<float>(kRefreshRate);
    note_length_scale_ *= static_cast<float>(updates_per_frame_);
    note_length_scale_ /= static_cast<float>(ai_frequency_);
    note_length_scale_ /= static_cast<float>(max_tempo_);

    decay_table_[255] = CalculateDecay(updates_inverse_scaled_, 0.25F);
    decay_table_[254] = CalculateDecay(updates_inverse_scaled_, 0.33F);
    decay_table_[253] = CalculateDecay(updates_inverse_scaled_, 0.5F);
    decay_table_[252] = CalculateDecay(updates_inverse_scaled_, 0.66F);
    decay_table_[251] = CalculateDecay(updates_inverse_scaled_, 0.75F);
    for (int i = 128; i < 251; i++)
    {
        decay_table_[static_cast<std::size_t>(i)] =
            CalculateDecay(updates_inverse_scaled_, static_cast<float>(251 - i));
    }

    for (int i = 16; i < 128; i++)
    {
        decay_table_[static_cast<std::size_t>(i)] =
            CalculateDecay(updates_inverse_scaled_, static_cast<float>(4 * (143 - i)));
    }

    for (int i = 1; i < 16; i++)
    {
        decay_table_[static_cast<std::size_t>(i)] =
            CalculateDecay(updates_inverse_scaled_, static_cast<float>(60 * (23 - i)));
    }

    channels_.resize(kPlayers * kChannelsPerPlayer);
    layers_.resize(static_cast<std::size_t>(layer_count));
    layer_is_free_.assign(static_cast<std::size_t>(layer_count), 1);
    free_layers_.reserve(static_cast<std::size_t>(layer_count));
    for (int i = 0; i < layer_count; i++)
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
        PushBack(Head(0, kDisabled), static_cast<int>(i));
    }

    update_states_.resize(voices_.size() * static_cast<std::size_t>(updates_per_frame_));

    for (Voice& voice : voices_)
    {
        const uint32_t base = memory.Allocate(0x1E0 + 16 + kSurroundStateSize);
        voice.buffers.surround_state = base + 0x1F0;
        voice.buffers.adpcm_state = base;
        voice.buffers.resample_state = base + 0x20;
        voice.buffers.filter_state = base + 0x40;
        voice.buffers.delay_state = base + 0xA0;
        voice.buffers.comb_state = base + 0xE0;
        voice.buffers.filter_taps = base + 0x1E0;
    }

    waves_address_ = memory.Allocate(tables::kWaves.size() * 2);
    for (std::size_t i = 0; i < tables::kWaves.size(); i++)
    {
        WriteBe16(memory, waves_address_ + static_cast<uint32_t>(i) * 2, tables::kWaves[i]);
    }

    noise_offset_ = 7 * 0x200;
    note_book_address_ = memory.Allocate(tables::kNoteBook.size() * 2);
    for (std::size_t i = 0; i < tables::kNoteBook.size(); i++)
    {
        WriteBe16(memory, note_book_address_ + static_cast<uint32_t>(i) * 2, tables::kNoteBook[i]);
    }

    const auto load_taps = [&](uint32_t address, const std::span<const int16_t> taps)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            WriteBe16(memory, address + static_cast<uint32_t>(i) * 2, taps[i]);
        }
    };

    reverbs_.resize(config.reverbs.size());
    for (std::size_t i = 0; i < reverbs_.size(); i++)
    {
        const EadLaterReverb& settings = config.reverbs[i];
        Reverb& reverb = reverbs_[i];
        reverb.downsample = settings.downsample;
        reverb.window = static_cast<uint16_t>(settings.window * 64);
        reverb.window = static_cast<uint16_t>(reverb.window / reverb.downsample);
        reverb.decay = settings.decay;
        reverb.volume = static_cast<int16_t>(settings.volume);
        reverb.second_delay = static_cast<uint16_t>(settings.second_delay * 64);
        reverb.second_gain = static_cast<int16_t>(settings.second_gain);
        reverb.leak_right_to_left = static_cast<int16_t>(settings.leak_right_to_left);
        reverb.leak_left_to_right = static_cast<int16_t>(settings.leak_left_to_right);
        reverb.send_to = settings.send_to;
        reverb.send_gain = static_cast<int16_t>(settings.send_gain);
        reverb.enabled = true;
        reverb.left = memory.Allocate(std::size_t{reverb.window} * 2);
        reverb.right = memory.Allocate(std::size_t{reverb.window} * 2);
        reverb.buffer_samples = reverb.window;
        if (reverb.downsample != 1)
        {
            reverb.downsample_pitch = static_cast<uint16_t>(0x8000 / reverb.downsample);
            for (uint32_t& state : reverb.resample_states)
            {
                state = memory.Allocate(32);
            }

            for (auto& frame : reverb.items)
            {
                for (int update = 0; update < updates_per_frame_; update++)
                {
                    const uint32_t buffer = memory.Allocate(kTwoChannels);
                    frame[static_cast<std::size_t>(update)].downsample_left = buffer;
                    frame[static_cast<std::size_t>(update)].downsample_right = buffer + kOneChannel;
                }
            }
        }

        if (settings.low_pass_left != 0)
        {
            reverb.filter_left_state = memory.Allocate(32);
            reverb.filter_left = memory.Allocate(16);
            load_taps(reverb.filter_left,
                      std::span<const int16_t>(tables::kLowPass)
                          .subspan(std::size_t{8} * static_cast<std::size_t>(settings.low_pass_left)));
        }

        if (settings.low_pass_right != 0)
        {
            reverb.filter_right_state = memory.Allocate(32);
            reverb.filter_right = memory.Allocate(16);
            load_taps(reverb.filter_right,
                      std::span<const int16_t>(tables::kLowPass)
                          .subspan(std::size_t{8} * static_cast<std::size_t>(settings.low_pass_right)));
        }
    }

    font_loaded_.assign(files.banks.size(), 0);
    book_addresses_.resize(files.banks.size());
    loop_addresses_.resize(files.banks.size());

    for (int player = 0; player < kPlayers; player++)
    {
        Player& target = players_[static_cast<std::size_t>(player)];
        target.ports.fill(-1);
        target.mute_behavior = kMuteSoften | kMuteStopNotes;
        target.fade_scale = 1.0F;
        target.bend = 1.0F;
        for (int slot = 0; slot < kChannelsPerPlayer; slot++)
        {
            channels_[static_cast<std::size_t>(player * kChannelsPerPlayer + slot)].player = player;
        }

        ResetPlayer(player);
    }
}

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

uint8_t EadLaterPlayer::PeekByte(const Player& player, uint32_t place) const
{
    return place < player.bytes.size() ? player.bytes[place] : uint8_t{0};
}

uint16_t EadLaterPlayer::Peek16(const Player& player, uint32_t place) const
{
    return static_cast<uint16_t>((uint32_t{PeekByte(player, place)} << 8) | PeekByte(player, place + 1));
}

uint8_t EadLaterPlayer::Read8(const Player& player, Script& script) const
{
    return PeekByte(player, script.pc++);
}

int16_t EadLaterPlayer::Read16(const Player& player, Script& script) const
{
    const uint16_t value = Peek16(player, script.pc);
    script.pc += 2;

    return static_cast<int16_t>(value);
}

uint16_t EadLaterPlayer::ReadVariable(const Player& player, Script& script) const
{
    const uint8_t first = Read8(player, script);
    if ((first & 0x80) == 0)
    {
        return first;
    }

    return static_cast<uint16_t>(((first & 0x7F) << 8) | Read8(player, script));
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

int EadLaterPlayer::RealFont(int font) const
{
    return font;
}

const EadInstrument* EadLaterPlayer::Instrument(int font, int program) const
{
    if (font == 0xFF || static_cast<std::size_t>(font) >= font_loaded_.size() ||
        font_loaded_[static_cast<std::size_t>(font)] == 0)
    {
        return nullptr;
    }

    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (static_cast<std::size_t>(program) >= bank.program_instruments.size())
    {
        return nullptr;
    }

    const int32_t object = bank.program_instruments[static_cast<std::size_t>(program)];

    return object == kNoObject ? nullptr : &bank.instruments[static_cast<std::size_t>(object)];
}

const EadDrum* EadLaterPlayer::Drum(int font, int note) const
{
    if (font == 0xFF || static_cast<std::size_t>(font) >= font_loaded_.size() ||
        font_loaded_[static_cast<std::size_t>(font)] == 0)
    {
        return nullptr;
    }

    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (static_cast<std::size_t>(note) >= bank.drum_notes.size())
    {
        return nullptr;
    }

    const int32_t object = bank.drum_notes[static_cast<std::size_t>(note)];

    return object == kNoObject ? nullptr : &bank.drums[static_cast<std::size_t>(object)];
}

const EadSoundEffect* EadLaterPlayer::SoundEffect(int font, int number) const
{
    if (font == 0xFF || static_cast<std::size_t>(font) >= font_loaded_.size() ||
        font_loaded_[static_cast<std::size_t>(font)] == 0)
    {
        return nullptr;
    }

    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (static_cast<std::size_t>(number) >= bank.effects.size())
    {
        return nullptr;
    }

    const EadSoundEffect& effect = bank.effects[static_cast<std::size_t>(number)];

    return effect.sound.sample == kNoObject ? nullptr : &effect;
}

const EadSample* EadLaterPlayer::SampleOf(int font, const EadTunedSample* sound) const
{
    if (sound == nullptr || sound->sample == kNoObject)
    {
        return nullptr;
    }

    return &files_->banks[static_cast<std::size_t>(font)].samples[static_cast<std::size_t>(sound->sample)];
}

uint8_t EadLaterPlayer::ChannelForm(uint8_t command) const
{
    if (majora_)
    {
        static constexpr std::array<uint8_t, 16> kEarlyForms = {0x81, 0x00, 0x81, 0x00, 0x01, 0x00, 0x42, 0x01,
                                                                0xC2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (command < 0xB0)
        {
            return kEarlyForms[static_cast<std::size_t>(command - 0xA0)];
        }

        if (command == 0xBD)
        {
            return 0x81;
        }

        if (command == 0xBE)
        {
            return 0x01;
        }
    }

    return kChannelArguments[static_cast<std::size_t>(command - 0xB0)];
}

uint16_t EadLaterPlayer::FlowArgument(const Player& player, Script& script, uint8_t command) const
{
    const uint8_t form = kChannelArguments[static_cast<std::size_t>(command - 0xB0)];
    if ((form & 3) != 1)
    {
        return 0;
    }

    return (form & 0x80) == 0 ? Read8(player, script) : static_cast<uint16_t>(Read16(player, script));
}

int EadLaterPlayer::FlowCommand(Player& player, Script& script, uint8_t command, uint16_t argument)
{
    switch (command)
    {
    case 0xFF:
        if (script.depth == 0)
        {
            return kScriptEnd;
        }

        script.depth--;
        script.pc = script.stack[script.depth & 3];
        break;
    case 0xFD:
        return ReadVariable(player, script);
    case 0xFE:
        return 1;
    case 0xFC:
        script.stack[script.depth & 3] = script.pc;
        script.depth++;
        script.pc = argument;
        break;
    case 0xF8:
        script.loops[script.depth & 3] = static_cast<uint8_t>(argument);
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
    case 0xF5:
    case 0xF9:
    case 0xFA:
    case 0xFB:
        if ((command == 0xFA && script.value != 0) || (command == 0xF9 && script.value >= 0) ||
            (command == 0xF5 && script.value < 0))
        {
            break;
        }

        script.pc = argument;
        break;
    case 0xF2:
    case 0xF3:
    case 0xF4:
        if ((command == 0xF3 && script.value != 0) || (command == 0xF2 && script.value >= 0))
        {
            break;
        }

        script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + static_cast<int8_t>(argument & 0xFF));
        break;
    default:
        break;
    }

    return 0;
}

void EadLaterPlayer::ResetPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    DisablePlayer(index);
    player.stop_script = false;
    player.delay = 0;
    player.state = 1;
    player.fade_updates = 0;
    player.fade_updates_next = 0;
    player.tempo_count = 0;
    player.tempo = 120 * kTicksPerBeat;
    player.tempo_change = 0;
    player.transpose = 0;
    player.policy = 0;
    player.short_velocities = -1;
    player.short_gates = -1;
    player.script_counter = 0;
    player.fade_volume = 1.0F;
    player.fade_step = 0.0F;
    player.volume = 0.0F;
    player.mute_scale = 0.5F;
    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        InitChannel(index * kChannelsPerPlayer + slot);
    }
}

void EadLaterPlayer::DisableChannelLayers(int index)
{
    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        DisableChannel(index * kChannelsPerPlayer + slot);
    }
}

void EadLaterPlayer::ReleaseNotesOfFont(int font)
{
    for (Voice& voice : voices_)
    {
        if (voice.font == font && voice.priority != 0 && voice.adsr.phase == AdsrPhase::kDecay)
        {
            voice.priority = 1;
            voice.adsr.fade_step = updates_inverse_;
            voice.adsr.release = true;
        }
    }
}

void EadLaterPlayer::DisablePlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    const bool was_finished = player.finished;
    DisableChannelLayers(index);
    ClearPool(PlayerPool(index));
    if (!player.enabled)
    {
        return;
    }

    player.enabled = false;
    player.finished = true;
    if (was_finished && !majora_)
    {
        ReleaseNotesOfFont(player.default_font);
    }
}

void EadLaterPlayer::InitChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel.enabled = false;
    channel.finished = false;
    channel.stop_script = false;
    channel.muted = false;
    channel.has_instrument = false;
    channel.stereo_effects = false;
    channel.transpose = 0;
    channel.large_notes = false;
    channel.book_offset = 0;
    channel.stereo = 0;
    channel.change_frequency = true;
    channel.change_volume = true;
    channel.change_pan = true;
    channel.script.depth = 0;
    channel.new_pan = 0x40;
    channel.pan_weight = 0x80;
    channel.velocity_random = 0;
    channel.gate_random = 0;
    channel.reverb_index = 0;
    channel.reverb = 0;
    channel.gain = 0;
    channel.priority = 3;
    channel.release_priority = 1;
    channel.delay = 0;
    channel.adsr.envelope = {};
    channel.adsr.release = 0xF0;
    channel.adsr.sustain = 0;
    channel.vibrato_rate_target = 0x800;
    channel.vibrato_rate_start = 0x800;
    channel.vibrato_depth_target = 0;
    channel.vibrato_depth_start = 0;
    channel.vibrato_rate_delay = 0;
    channel.vibrato_depth_delay = 0;
    channel.vibrato_delay = 0;
    channel.filter = -1;
    channel.comb_gain = 0;
    channel.comb_size = 0;
    channel.surround = 0xFF;
    channel.start_sample = 0;
    channel.volume = 1.0F;
    channel.volume_scale = 1.0F;
    channel.frequency_scale = 1.0F;
    channel.ports.fill(-1);
}

void EadLaterPlayer::DisableChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    for (int slot = 0; slot < kLayersPerChannel; slot++)
    {
        FreeLayer(index, slot);
    }

    ClearPool(ChannelPool(index));
    channel.enabled = false;
    channel.finished = true;
}

void EadLaterPlayer::EnableChannel(int player, int slot, uint32_t script)
{
    const int index = player * kChannelsPerPlayer + slot;
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel.enabled = true;
    channel.finished = false;
    channel.script.depth = 0;
    channel.script.pc = script;
    channel.delay = 0;
    for (int i = 0; i < kLayersPerChannel; i++)
    {
        if (channel.layers[static_cast<std::size_t>(i)] != kNoLayer)
        {
            FreeLayer(index, i);
        }
    }
}

bool EadLaterPlayer::SetLayer(int channel_index, int slot)
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
        DecayOrRelease(held, AdsrPhase::kDecay);
    }

    Layer& layer = layers_[static_cast<std::size_t>(held)];
    layer.channel = channel_index;
    layer.adsr = channel.adsr;
    layer.adsr.release = 0;
    layer.enabled = true;
    layer.finished = false;
    layer.muted = false;
    layer.continuous = false;
    layer.loaded = false;
    layer.ignore_drum_pan = false;
    layer.continuous_started = false;
    layer.properties_need_init = false;
    layer.stereo = 0;
    layer.reverb = channel.reverb;
    layer.surround = 0x80;
    layer.flags = 0xFFFF;
    layer.portamento.mode = 0;
    layer.script.depth = 0;
    layer.gate = 0x80;
    layer.pan = 0x40;
    layer.transpose = 0;
    layer.delay = 0;
    layer.gate_delay = 0;
    layer.delay_copy = 0;
    layer.voice = -1;
    layer.instrument = nullptr;
    layer.frequency_scale = 1.0F;
    layer.bend = 1.0F;
    layer.velocity_varied = 0.0F;
    layer.program = 0xFF;

    return true;
}

void EadLaterPlayer::DisableLayer(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    if (players_[static_cast<std::size_t>(channel.player)].finished)
    {
        DecayOrRelease(index, AdsrPhase::kRelease);
    }
    else
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }

    layer.enabled = false;
    layer.finished = true;
}

void EadLaterPlayer::FreeLayer(int channel_index, int slot)
{
    int& held = channels_[static_cast<std::size_t>(channel_index)].layers[static_cast<std::size_t>(slot)];
    if (held == kNoLayer)
    {
        return;
    }

    if (layer_is_free_[static_cast<std::size_t>(held)] == 0)
    {
        free_layers_.push_back(held);
        layer_is_free_[static_cast<std::size_t>(held)] = 1;
    }

    DisableLayer(held);
    held = kNoLayer;
}

void EadLaterPlayer::SetPriorities(Channel& channel, uint8_t priorities)
{
    if ((priorities & 0xF) != 0)
    {
        channel.priority = priorities & 0xF;
    }

    if ((priorities >> 4) != 0)
    {
        channel.release_priority = priorities >> 4;
    }
}

uint8_t EadLaterPlayer::FindProgram(Channel& channel, uint8_t program, const EadInstrument*& out, AdsrSettings& adsr)
{
    const EadInstrument* instrument = Instrument(channel.font, program);
    if (instrument == nullptr)
    {
        out = nullptr;

        return 0;
    }

    const EadBank& bank = files_->banks[channel.font];
    adsr.envelope = {};
    adsr.envelope.bank =
        instrument->envelope == kNoObject ? nullptr : &bank.envelopes[static_cast<std::size_t>(instrument->envelope)];
    adsr.release = instrument->release_rate;
    out = instrument;

    // Programs 0 and 1 stand for the drums and the sound effects. An instrument's number therefore moves up by 2.
    return static_cast<uint8_t>(program + 2);
}

void EadLaterPlayer::SetProgram(int index, uint8_t program)
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
    else if (program == 0x7E)
    {
        channel.program = 1;
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

void EadLaterPlayer::LoadFilter(int player_index, uint32_t place, int low_pass, int high_pass)
{
    Player& player = players_[static_cast<std::size_t>(player_index)];
    const auto write = [&](std::size_t i, int value)
    {
        const uint32_t at = place + static_cast<uint32_t>(i) * 2;
        if (at + 1 < player.bytes.size())
        {
            player.bytes[at] = static_cast<uint8_t>(static_cast<uint32_t>(value) >> 8);
            player.bytes[at + 1] = static_cast<uint8_t>(value);
        }
    };

    for (std::size_t i = 0; i < 8; i++)
    {
        const int low = tables::kLowPass[static_cast<std::size_t>(low_pass) * 8 + i];
        if (high_pass == 0)
        {
            write(i, low);
        }
        else
        {
            const int high = tables::kHighPass[static_cast<std::size_t>(high_pass - 1) * 8 + i];
            write(i, low_pass == 0 ? high : (low + high) / 2);
        }
    }
}

void EadLaterPlayer::StartSlowLoad(SlowLoad load)
{
    load.active = true;
    slow_loads_[static_cast<std::size_t>(next_slow_load_)] = load;
    next_slow_load_ ^= 1;
}

void EadLaterPlayer::StartSampleLoad(int player, int channel, int port, int font, int number)
{
    int8_t* status = channels_[static_cast<std::size_t>(channel)].ports.data() + port;
    const EadTunedSample* sound = nullptr;
    if (number < 0x80)
    {
        const EadInstrument* instrument = Instrument(font, number);
        sound = instrument != nullptr ? &instrument->normal : nullptr;
    }
    else if (number < 0x100)
    {
        const EadDrum* drum = Drum(font, number - 0x80);
        sound = drum != nullptr ? &drum->sound : nullptr;
    }
    else
    {
        const EadSoundEffect* effect = SoundEffect(font, number - 0x100);
        sound = effect != nullptr ? &effect->sound : nullptr;
    }

    const EadSample* sample = SampleOf(font, sound);
    if (sample == nullptr)
    {
        *status = 0;

        return;
    }

    if (std::ranges::find(samples_in_memory_, sample) != samples_in_memory_.end())
    {
        *status = 2;

        return;
    }

    samples_in_memory_.push_back(sample);
    SlowLoad load;
    load.bytes_left = Align16(static_cast<int>(sample->size));
    load.player = player;
    load.channel = channel;
    load.port = port;
    StartSlowLoad(load);
}

void EadLaterPlayer::RunSlowLoads()
{
    for (SlowLoad& load : slow_loads_)
    {
        if (!load.active)
        {
            continue;
        }

        if (load.bytes_left == 0)
        {
            load.active = false;
            int8_t& status =
                load.channel >= 0
                    ? channels_[static_cast<std::size_t>(load.channel)].ports[static_cast<std::size_t>(load.port)]
                    : players_[static_cast<std::size_t>(load.player)].ports[static_cast<std::size_t>(load.port)];
            status = 1;
            if (load.sequence >= 0)
            {
                Player& player = players_[static_cast<std::size_t>(load.player)];
                const std::vector<uint8_t>& source = files_->sequences[static_cast<std::size_t>(load.sequence)];
                for (std::size_t i = 0; i < source.size() && load.copy_to + i < player.bytes.size(); i++)
                {
                    player.bytes[load.copy_to + i] = source[i];
                }
            }
        }
        else
        {
            load.bytes_left = load.bytes_left < 0x400 ? 0 : load.bytes_left - 0x400;
        }
    }

    for (const auto& [player, port] : pending_script_loads_)
    {
        players_[static_cast<std::size_t>(player)].ports[static_cast<std::size_t>(port)] = 0;
    }

    pending_script_loads_.clear();
}

void EadLaterPlayer::Start(int index, int sequence)
{
    const RoundTowardZero rounding(stadium_2_);
    if (static_cast<std::size_t>(sequence) >= files_->sequences.size() || index >= config_.players)
    {
        return;
    }

    DisablePlayer(index);
    int font = 0xFF;
    if (static_cast<std::size_t>(sequence) < files_->bank_sets.size())
    {
        for (const uint8_t bank : files_->bank_sets[static_cast<std::size_t>(sequence)])
        {
            font = bank;
            if (bank < font_loaded_.size())
            {
                font_loaded_[bank] = 1;
            }
        }
    }

    ResetPlayer(index);
    Player& player = players_[static_cast<std::size_t>(index)];
    player.sequence = static_cast<uint8_t>(sequence);
    player.default_font = static_cast<uint8_t>(font);
    player.bytes = files_->sequences[static_cast<std::size_t>(sequence)];
    player.enabled = true;
    player.script.pc = 0;
    player.script.depth = 0;
    player.delay = 0;
    player.finished = false;
    player.skip_ticks = 0;
}

void EadLaterPlayer::Stop(int index)
{
    players_[static_cast<std::size_t>(index)].finished = true;
    DisablePlayer(index);
}

bool EadLaterPlayer::Enabled(int index) const
{
    return players_[static_cast<std::size_t>(index)].enabled;
}

void EadLaterPlayer::SetVolumeScale(int index, float scale)
{
    const RoundTowardZero rounding(stadium_2_);
    Player& player = players_[static_cast<std::size_t>(index)];
    player.fade_scale = scale;
    player.recalculate_volume = true;
}

void EadLaterPlayer::FadeOut(int index, int updates)
{
    const RoundTowardZero rounding(stadium_2_);
    Player& player = players_[static_cast<std::size_t>(index)];
    if (updates == 0)
    {
        updates = 1;
    }

    player.state = 2;
    player.fade_updates = static_cast<uint16_t>(updates);
    player.fade_step = -(player.fade_volume / static_cast<float>(updates));
}

void EadLaterPlayer::SetMuted(int index, bool muted)
{
    players_[static_cast<std::size_t>(index)].muted = muted;
    players_[static_cast<std::size_t>(index)].recalculate_volume = true;
}

void EadLaterPlayer::SetChannelVolumeScale(int player, int channel, float scale)
{
    Channel& target = channels_[static_cast<std::size_t>(player * kChannelsPerPlayer + channel)];
    target.volume_scale = scale;
    target.change_volume = true;
}

void EadLaterPlayer::SetChannelFrequency(int player, int channel, float scale)
{
    Channel& target = channels_[static_cast<std::size_t>(player * kChannelsPerPlayer + channel)];
    target.frequency_scale = scale;
    target.change_frequency = true;
}

void EadLaterPlayer::SetChannelPan(int player, int channel, uint8_t pan)
{
    Channel& target = channels_[static_cast<std::size_t>(player * kChannelsPerPlayer + channel)];
    target.new_pan = pan;
    target.change_pan = true;
}

void EadLaterPlayer::WritePlayerPort(int player, int port, int8_t value)
{
    players_[static_cast<std::size_t>(player)].ports[static_cast<std::size_t>(port)] = value;
}

void EadLaterPlayer::WriteChannelPort(int player, int channel, int port, int8_t value)
{
    channels_[static_cast<std::size_t>(player * kChannelsPerPlayer + channel)].ports[static_cast<std::size_t>(port)] =
        value;
}

void EadLaterPlayer::SetRandom(uint32_t value)
{
    random_ = value;
    random_set_ = true;
}

int EadLaterPlayer::UpdatesPerFrame() const
{
    return updates_per_frame_;
}

int EadLaterPlayer::TempoLimit() const
{
    return max_tempo_;
}

uint64_t EadLaterPlayer::UpdateCount() const
{
    return updates_;
}

void EadLaterPlayer::RunPlayer(int index)
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

    player.script_counter++;
    if (majora_)
    {
        const int step = std::min<int>(player.tempo + static_cast<int16_t>(player.tempo_change), max_tempo_);
        player.tempo_count = static_cast<uint16_t>(player.tempo_count + step);
    }
    else
    {
        player.tempo_count = static_cast<uint16_t>(player.tempo_count + player.tempo);
        player.tempo_count = static_cast<uint16_t>(player.tempo_count + static_cast<int16_t>(player.tempo_change));
    }

    if (static_cast<int32_t>(player.tempo_count) < max_tempo_)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count - static_cast<uint16_t>(max_tempo_));
    if (player.stop_script)
    {
        return;
    }

    if (player.delay > 1)
    {
        player.delay--;
    }
    else
    {
        player.recalculate_volume = true;
        Script& script = player.script;
        bool running = true;
        while (running)
        {
            uint8_t command = Read8(player, script);
            if (command >= 0xF2)
            {
                const uint16_t argument = FlowArgument(player, script, command);
                const int delay = FlowCommand(player, script, command, argument);
                if (delay != 0)
                {
                    if (delay == kScriptEnd)
                    {
                        DisablePlayer(index);
                    }
                    else
                    {
                        player.delay = static_cast<uint16_t>(delay);
                    }

                    running = false;
                }

                continue;
            }

            if (command >= 0xC0)
            {
                switch (command)
                {
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
                case 0xDD:
                    player.tempo = static_cast<uint16_t>(Read8(player, script) * kTicksPerBeat);
                    if (player.tempo > static_cast<uint16_t>(max_tempo_))
                    {
                        player.tempo = static_cast<uint16_t>(max_tempo_);
                    }

                    if (static_cast<int16_t>(player.tempo) <= 0)
                    {
                        player.tempo = 1;
                    }

                    break;
                case 0xDC:
                    player.tempo_change =
                        static_cast<uint16_t>(static_cast<int8_t>(Read8(player, script)) * kTicksPerBeat);
                    break;
                case 0xDA:
                    {
                        const uint8_t mode = Read8(player, script);
                        const auto updates = static_cast<uint16_t>(Read16(player, script));
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
                        const int value = Read8(player, script);
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
                                player.fade_step = (static_cast<float>(value) / 127.0F - player.fade_volume) /
                                                   static_cast<float>(int32_t{player.fade_updates_next});
                            }
                            else
                            {
                                player.fade_volume = static_cast<float>(value) / 127.0F;
                            }
                        }

                        break;
                    }
                case 0xD9:
                    player.fade_scale = static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
                    break;
                case 0xD7:
                    {
                        uint32_t mask = static_cast<uint16_t>(Read16(player, script));
                        for (int slot = 0; slot < kChannelsPerPlayer; slot++)
                        {
                            if ((mask & 1) != 0)
                            {
                                Channel& channel =
                                    channels_[static_cast<std::size_t>(index * kChannelsPerPlayer + slot)];
                                channel.font = player.default_font;
                                channel.mute_behavior = player.mute_behavior;
                                channel.policy = player.policy;
                            }

                            mask >>= 1;
                        }

                        break;
                    }
                case 0xD6:
                    Read16(player, script);
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
                case 0xD1:
                    player.short_gates = static_cast<uint16_t>(Read16(player, script));
                    break;
                case 0xD2:
                    player.short_velocities = static_cast<uint16_t>(Read16(player, script));
                    break;
                case 0xD0:
                    player.policy = Read8(player, script);
                    break;
                case 0xCE:
                    {
                        const uint8_t range = Read8(player, script);
                        script.value = range == 0 ? static_cast<int8_t>((random_ >> 2) & 0xFF)
                                                  : static_cast<int8_t>((random_ >> 2) % range);
                        break;
                    }
                case 0xCD:
                    {
                        const auto table = static_cast<uint16_t>(Read16(player, script));
                        if (script.value != -1 && script.depth != 3)
                        {
                            const uint32_t place = static_cast<uint32_t>(table + (script.value << 1));
                            script.stack[script.depth & 3] = script.pc;
                            script.depth++;
                            script.pc = Peek16(player, place);
                        }

                        break;
                    }
                case 0xCC:
                    script.value = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xC9:
                    script.value = static_cast<int8_t>(script.value & Read8(player, script));
                    break;
                case 0xC8:
                    script.value = static_cast<int8_t>(script.value - Read8(player, script));
                    break;
                case 0xC7:
                    {
                        const uint8_t add = Read8(player, script);
                        const auto place = static_cast<uint16_t>(Read16(player, script));
                        if (place < player.bytes.size())
                        {
                            player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(script.value) + add);
                        }

                        break;
                    }
                case 0xC6:
                    player.stop_script = true;

                    return;
                case 0xC5:
                    player.script_counter = static_cast<uint16_t>(Read16(player, script));
                    break;
                case 0xEF:
                    Read16(player, script);
                    Read8(player, script);
                    break;
                case 0xC2:
                    if (majora_)
                    {
                        const auto table = static_cast<uint16_t>(Read16(player, script));
                        if (script.value != -1)
                        {
                            script.pc = Peek16(player, table + static_cast<uint32_t>(script.value * 2));
                        }
                    }

                    break;
                case 0xC3:
                    if (majora_)
                    {
                        const auto table = static_cast<uint16_t>(Read16(player, script));
                        if (script.value != -1)
                        {
                            uint32_t bits = Peek16(player, table + static_cast<uint32_t>(script.value * 2));
                            for (int slot = 0; slot < kChannelsPerPlayer; slot++)
                            {
                                channels_[static_cast<std::size_t>(index * kChannelsPerPlayer + slot)].muted =
                                    (bits & 1) != 0;
                                bits >>= 1;
                            }
                        }
                    }

                    break;
                case 0xC4:
                    {
                        uint8_t target = Read8(player, script);
                        if (target == 0xFF)
                        {
                            target = static_cast<uint8_t>(index);
                            if (majora_ && player.state == 2)
                            {
                                break;
                            }
                        }

                        const uint8_t sequence = Read8(player, script);
                        Start(target, sequence);
                        if (target == static_cast<uint8_t>(index))
                        {
                            return;
                        }

                        break;
                    }
                default:
                    break;
                }

                continue;
            }

            const int low = command & 0xF;
            switch (command & 0xF0)
            {
            case 0x00:
                script.value = static_cast<int8_t>(
                    channels_[static_cast<std::size_t>(index * kChannelsPerPlayer + low)].enabled ? 0 : 1);
                break;
            case 0x50:
                script.value = static_cast<int8_t>(script.value - player.ports[static_cast<std::size_t>(low & 7)]);
                break;
            case 0x70:
                player.ports[static_cast<std::size_t>(low & 7)] = script.value;
                break;
            case 0x80:
                script.value = player.ports[static_cast<std::size_t>(low & 7)];
                if (low < 2)
                {
                    player.ports[static_cast<std::size_t>(low)] = -1;
                }

                break;
            case 0x40:
                DisableChannel(index * kChannelsPerPlayer + low);
                break;
            case 0x90:
                EnableChannel(index, low, static_cast<uint16_t>(Read16(player, script)));
                break;
            case 0xA0:
                {
                    const int16_t relative = Read16(player, script);
                    EnableChannel(index, low, static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative));
                    break;
                }
            case 0xB0:
                {
                    const uint8_t sequence = Read8(player, script);
                    const auto place = static_cast<uint16_t>(Read16(player, script));
                    if (sequence >= files_->sequences.size())
                    {
                        player.ports[static_cast<std::size_t>(low & 7)] = 0;
                        break;
                    }

                    SlowLoad load;
                    load.bytes_left = Align16(static_cast<int>(files_->sequences[sequence].size()));
                    load.player = index;
                    load.port = low & 7;
                    load.sequence = sequence;
                    load.copy_to = place;
                    StartSlowLoad(load);
                    break;
                }
            case 0x60:
                {
                    const uint8_t table = Read8(player, script);
                    const uint8_t id = Read8(player, script);
                    if (table == 1 && id < font_loaded_.size())
                    {
                        font_loaded_[id] = 1;
                    }

                    pending_script_loads_.push_back({static_cast<int8_t>(index), static_cast<int8_t>(low & 7)});
                    break;
                }
            default:
                break;
            }
        }
    }

    if (!player.enabled)
    {
        return;
    }

    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        const int channel = index * kChannelsPerPlayer + slot;
        if (channels_[static_cast<std::size_t>(channel)].enabled)
        {
            RunChannel(channel);
        }
    }
}

void EadLaterPlayer::RunChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    const auto run_layers = [&]
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

    if (player.muted && (channel.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    if (channel.delay >= 2)
    {
        channel.delay--;
        run_layers();

        return;
    }

    Script& script = channel.script;
    const auto slot_channel = [&](int slot) -> Channel&
    {
        return channels_[static_cast<std::size_t>(channel.player * kChannelsPerPlayer + slot)];
    };
    while (true)
    {
        uint8_t command = Read8(player, script);
        if (command >= (majora_ ? 0xA0 : 0xB0))
        {
            const uint8_t form = ChannelForm(command);
            const int count = form & 3;
            std::array<uint32_t, 3> args = {};
            uint8_t widths = form;
            for (int i = 0; i < count; i++)
            {
                args[static_cast<std::size_t>(i)] = (widths & 0x80) == 0
                                                        ? uint32_t{Read8(player, script)}
                                                        : static_cast<uint32_t>(int32_t{Read16(player, script)});
                widths = static_cast<uint8_t>(widths << 1);
            }

            if (command >= 0xF2)
            {
                const int delay = FlowCommand(player, script, command, static_cast<uint16_t>(args[0]));
                if (delay != 0)
                {
                    if (delay == kScriptEnd)
                    {
                        DisableChannel(index);
                    }
                    else
                    {
                        channel.delay = static_cast<uint16_t>(delay);
                    }

                    break;
                }

                continue;
            }

            const auto arg8 = static_cast<uint8_t>(args[0]);
            const auto arg16 = static_cast<uint16_t>(args[0]);
            const auto arg_signed = static_cast<int8_t>(args[0]);
            const auto random_pointer = [&]()
            {
                const uint32_t value = args[0] == 0 ? random_ & 0xFFFF : random_ % args[0];
                const auto sum = static_cast<uint16_t>(value + args[1]);
                channel.pointer = static_cast<uint16_t>(((sum / 0x100 + 0x80) << 8) | (sum % 0x100));
            };
            bool leave = false;
            switch (command)
            {
            case 0xEA:
                channel.stop_script = true;
                leave = true;
                break;
            case 0xF1:
                ClearPool(ChannelPool(index));
                FillPool(ChannelPool(index), arg8);
                break;
            case 0xF0:
                ClearPool(ChannelPool(index));
                break;
            case 0xC2:
                channel.dynamic_table = arg16;
                break;
            case 0xC5:
                if (script.value != -1)
                {
                    const uint32_t place = channel.dynamic_table + static_cast<uint32_t>(script.value * 2);
                    if (majora_)
                    {
                        script.pc = Peek16(player, place);
                    }
                    else
                    {
                        channel.dynamic_table = Peek16(player, place);
                    }
                }

                break;
            case 0xEB:
            case 0xC6:
                {
                    uint8_t font = arg8;
                    if (player.default_font != 0xFF &&
                        static_cast<std::size_t>(player.sequence) < files_->bank_sets.size())
                    {
                        // The number counts back from the end of the sequence's list of banks.
                        const std::vector<uint8_t>& set = files_->bank_sets[player.sequence];
                        const int place = static_cast<int>(set.size()) - 1 - arg8;
                        font = place >= 0 && static_cast<std::size_t>(place) < set.size()
                                   ? set[static_cast<std::size_t>(place)]
                                   : uint8_t{0};
                    }

                    if (font < font_loaded_.size() && font_loaded_[font] != 0)
                    {
                        channel.font = font;
                    }

                    if (command == 0xEB)
                    {
                        SetProgram(index, static_cast<uint8_t>(args[1]));
                    }

                    break;
                }
            case 0xC1:
                SetProgram(index, arg8);
                break;
            case 0xC3:
                channel.large_notes = false;
                break;
            case 0xC4:
                channel.large_notes = true;
                break;
            case 0xDF:
                channel.volume = static_cast<float>(int32_t{arg8}) / 127.0F;
                channel.change_volume = true;
                break;
            case 0xE0:
                channel.volume_scale = static_cast<float>(int32_t{arg8}) / 128.0F;
                channel.change_volume = true;
                break;
            case 0xDE:
                channel.frequency_scale = static_cast<float>(int32_t{arg16}) / 32768.0F;
                channel.change_frequency = true;
                break;
            case 0xD3:
                channel.frequency_scale = tables::kBendOctave[static_cast<uint8_t>(arg8 + 0x80)];
                channel.change_frequency = true;
                break;
            case 0xEE:
                channel.frequency_scale = tables::kBendFine[static_cast<uint8_t>(arg8 + 0x80)];
                channel.change_frequency = true;
                break;
            case 0xDD:
                channel.new_pan = arg8;
                channel.change_pan = true;
                break;
            case 0xDC:
                channel.pan_weight = arg8;
                channel.change_pan = true;
                break;
            case 0xDB:
                channel.transpose = arg_signed;
                break;
            case 0xDA:
                channel.adsr.envelope = {};
                channel.adsr.envelope.player = channel.player;
                channel.adsr.envelope.offset = arg16;
                break;
            case 0xD9:
                channel.adsr.release = arg8;
                break;
            case 0xD8:
                channel.vibrato_depth_target = static_cast<uint16_t>(arg8 * 8);
                channel.vibrato_depth_start = 0;
                channel.vibrato_depth_delay = 0;
                break;
            case 0xD7:
                channel.vibrato_rate_delay = 0;
                channel.vibrato_rate_target = static_cast<uint16_t>(arg8 * 32);
                channel.vibrato_rate_start = static_cast<uint16_t>(arg8 * 32);
                break;
            case 0xE2:
                channel.vibrato_depth_start = static_cast<uint16_t>(arg8 * 8);
                channel.vibrato_depth_target = static_cast<uint16_t>((args[1] & 0xFF) * 8);
                channel.vibrato_depth_delay = static_cast<uint16_t>((args[2] & 0xFF) * 16);
                break;
            case 0xE1:
                channel.vibrato_rate_start = static_cast<uint16_t>(arg8 * 32);
                channel.vibrato_rate_target = static_cast<uint16_t>((args[1] & 0xFF) * 32);
                channel.vibrato_rate_delay = static_cast<uint16_t>((args[2] & 0xFF) * 16);
                break;
            case 0xE3:
                channel.vibrato_delay = static_cast<uint16_t>(arg8 * 16);
                break;
            case 0xD4:
                channel.reverb = arg8;
                break;
            case 0xC7:
                {
                    const auto place = static_cast<uint16_t>(args[1]);
                    if (place < player.bytes.size())
                    {
                        player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(script.value) + arg8);
                    }

                    break;
                }
            case 0xC8:
                script.value = static_cast<int8_t>(script.value - arg_signed);
                break;
            case 0xCC:
                script.value = arg_signed;
                break;
            case 0xC9:
                script.value = static_cast<int8_t>(script.value & arg_signed);
                break;
            case 0xCD:
                DisableChannel(channel.player * kChannelsPerPlayer + (arg8 & 0xF));
                break;
            case 0xCA:
                channel.mute_behavior = arg8;
                channel.change_volume = true;
                break;
            case 0xCB:
                script.value = static_cast<int8_t>(
                    PeekByte(player, static_cast<uint32_t>(static_cast<int32_t>(arg16) + script.value)));
                break;
            case 0xCE:
                channel.pointer = arg16;
                break;
            case 0xCF:
                if (uint32_t{arg16} + 1 < player.bytes.size())
                {
                    player.bytes[arg16] = static_cast<uint8_t>(channel.pointer >> 8);
                    player.bytes[arg16 + 1U] = static_cast<uint8_t>(channel.pointer);
                }

                break;
            case 0xD0:
                channel.stereo_effects = (arg8 & 0x80) != 0;
                channel.stereo = arg8 & 0x7F;
                break;
            case 0xD1:
                channel.policy = arg8;
                break;
            case 0xD2:
                channel.adsr.sustain = arg8;
                break;
            case 0xE5:
                channel.reverb_index = arg8;
                break;
            case 0xE4:
                if (script.value != -1)
                {
                    const uint32_t place = channel.dynamic_table + static_cast<uint32_t>(script.value * 2);
                    script.stack[script.depth & 3] = script.pc;
                    script.depth++;
                    script.pc = Peek16(player, place);
                }

                break;
            case 0xE6:
                channel.book_offset = arg8;
                break;
            case 0xE7:
                channel.mute_behavior = PeekByte(player, arg16);
                channel.policy = PeekByte(player, arg16 + 1U);
                SetPriorities(channel, PeekByte(player, arg16 + 2U));
                channel.transpose = static_cast<int8_t>(PeekByte(player, arg16 + 3U));
                channel.new_pan = PeekByte(player, arg16 + 4U);
                channel.pan_weight = PeekByte(player, arg16 + 5U);
                channel.reverb = PeekByte(player, arg16 + 6U);
                channel.reverb_index = PeekByte(player, arg16 + 7U);
                channel.change_pan = true;
                break;
            case 0xE8:
                channel.mute_behavior = arg8;
                channel.policy = static_cast<uint8_t>(args[1]);
                SetPriorities(channel, static_cast<uint8_t>(args[2]));
                channel.transpose = static_cast<int8_t>(Read8(player, script));
                channel.new_pan = Read8(player, script);
                channel.pan_weight = Read8(player, script);
                channel.reverb = Read8(player, script);
                channel.reverb_index = Read8(player, script);
                channel.change_pan = true;
                break;
            case 0xA0:
            case 0xA1:
            case 0xA2:
            case 0xA3:
                // These read and write a state that the game's logic gives only its channels of sound effects.
                break;
            case 0xA4:
                channel.surround = arg8;
                break;
            case 0xA5:
                script.value = static_cast<int8_t>(script.value + index % kChannelsPerPlayer);
                break;
            case 0xA6:
                {
                    const uint32_t place =
                        static_cast<uint16_t>(args[1]) + static_cast<uint32_t>(index % kChannelsPerPlayer);
                    if (place < player.bytes.size())
                    {
                        player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(script.value) + arg8);
                    }

                    break;
                }
            case 0xA7:
                {
                    const int value = script.value;
                    auto shifted =
                        static_cast<uint8_t>((arg8 & 0x80) == 0 ? value << (arg8 & 0xF) : value >> (arg8 & 0xF));
                    if ((arg8 & 0x40) != 0)
                    {
                        shifted = static_cast<uint8_t>((shifted & 0x7F) | (value & 0x80));
                    }

                    script.value = static_cast<int8_t>(shifted);
                    break;
                }
            case 0xA8:
                random_pointer();
                break;
            case 0xBE:
                break;
            case 0xEC:
                channel.start_sample = 0;
                channel.vibrato_depth_target = 0;
                channel.vibrato_depth_start = 0;
                channel.vibrato_depth_delay = 0;
                channel.vibrato_rate_target = 0;
                channel.vibrato_rate_start = 0;
                channel.vibrato_rate_delay = 0;
                channel.filter = -1;
                channel.gain = 0;
                channel.adsr.sustain = 0;
                channel.velocity_random = 0;
                channel.gate_random = 0;
                channel.comb_size = 0;
                channel.comb_gain = 0;
                channel.book_offset = 0;
                channel.frequency_scale = 1.0F;
                break;
            case 0xE9:
                SetPriorities(channel, arg8);
                break;
            case 0xED:
                channel.gain = arg8;
                break;
            case 0xB0:
                channel.filter = arg16;
                break;
            case 0xB1:
                channel.filter = -1;
                break;
            case 0xB3:
                if (channel.filter != -1)
                {
                    LoadFilter(channel.player, static_cast<uint32_t>(channel.filter), (arg8 >> 4) & 0xF, arg8 & 0xF);
                }

                break;
            case 0xB2:
                channel.pointer = Peek16(player, static_cast<uint32_t>(static_cast<int32_t>(arg16) + script.value));
                break;
            case 0xB4:
                channel.dynamic_table = channel.pointer;
                break;
            case 0xB5:
                channel.pointer =
                    Peek16(player, channel.dynamic_table + static_cast<uint32_t>(int32_t{script.value} * 2));
                break;
            case 0xB6:
                script.value = static_cast<int8_t>(
                    PeekByte(player, channel.dynamic_table + static_cast<uint32_t>(int32_t{script.value})));
                break;
            case 0xB7:
                channel.pointer = static_cast<uint16_t>(args[0] == 0 ? random_ & 0xFFFF : random_ % args[0]);
                break;
            case 0xB8:
                script.value = static_cast<int8_t>(args[0] == 0 ? random_ & 0xFFFF : random_ % args[0]);
                break;
            case 0xBD:
                if (majora_)
                {
                    channel.start_sample = arg16;
                }
                else
                {
                    random_pointer();
                }

                break;
            case 0xB9:
                channel.velocity_random = arg8;
                break;
            case 0xBA:
                channel.gate_random = arg8;
                break;
            case 0xBB:
                channel.comb_size = arg8;
                channel.comb_gain = static_cast<uint16_t>(args[1]);
                break;
            case 0xBC:
                channel.pointer = static_cast<uint16_t>(channel.pointer + args[0]);
                break;
            default:
                break;
            }

            if (leave)
            {
                break;
            }

            continue;
        }

        if (command >= 0x70)
        {
            int low = command & 0x7;
            const int group = command & 0xF8;
            if (group != 0x70 && low >= (stadium_2_ ? kLayersPerChannel : 4))
            {
                low = 0;
            }

            switch (group)
            {
            case 0x80:
                {
                    const int layer = channel.layers[static_cast<std::size_t>(low)];
                    script.value = layer != kNoLayer
                                       ? static_cast<int8_t>(layers_[static_cast<std::size_t>(layer)].finished)
                                       : int8_t{-1};
                    break;
                }
            case 0x88:
                {
                    const auto place = static_cast<uint16_t>(Read16(player, script));
                    if (SetLayer(index, low))
                    {
                        layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                            place;
                    }

                    break;
                }
            case 0x90:
                FreeLayer(index, low);
                break;
            case 0x98:
                if (script.value != -1 && SetLayer(index, low))
                {
                    const uint32_t place = channel.dynamic_table + static_cast<uint32_t>(script.value * 2);
                    layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                        Peek16(player, place);
                }

                break;
            case 0x70:
                channel.ports[static_cast<std::size_t>(low)] = script.value;
                break;
            case 0x78:
                {
                    const int16_t relative = Read16(player, script);
                    if (SetLayer(index, low))
                    {
                        layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                            static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                    }

                    break;
                }
            default:
                break;
            }

            continue;
        }

        const int low = command & 0xF;
        bool leave = false;
        switch (command & 0xF0)
        {
        case 0x00:
            channel.delay = static_cast<uint16_t>(low);
            leave = true;
            break;
        case 0x10:
            if (low < 8)
            {
                channel.ports[static_cast<std::size_t>(low)] = -1;
                StartSampleLoad(channel.player, index, low, channel.font, script.value);
            }
            else
            {
                channel.ports[static_cast<std::size_t>(low - 8)] = -1;
                StartSampleLoad(channel.player, index, low - 8, channel.font, channel.pointer + 0x100);
            }

            break;
        case 0x60:
            script.value = channel.ports[static_cast<std::size_t>(low & 7)];
            if (low < 2)
            {
                channel.ports[static_cast<std::size_t>(low)] = -1;
            }

            break;
        case 0x50:
            script.value = static_cast<int8_t>(script.value - channel.ports[static_cast<std::size_t>(low & 7)]);
            break;
        case 0x20:
            EnableChannel(channel.player, low, static_cast<uint16_t>(Read16(player, script)));
            break;
        case 0x30:
            {
                const uint8_t port = Read8(player, script);
                slot_channel(low).ports[port & 7U] = script.value;
                break;
            }
        case 0x40:
            {
                const uint8_t port = Read8(player, script);
                script.value = slot_channel(low).ports[port & 7U];
                break;
            }
        default:
            break;
        }

        if (leave)
        {
            break;
        }
    }

    run_layers();
}

void EadLaterPlayer::RunLayer(int index)
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
            DecayOrRelease(index, AdsrPhase::kDecay);
            layer.muted = true;
        }

        return;
    }

    if (!layer.continuous)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }
    else if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].wanted_layer == index)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }

    const int mode = layer.portamento.mode & ~kPortamentoSpecial;
    if (mode == 1 || mode == 2)
    {
        layer.portamento.mode = 0;
    }

    layer.properties_need_init = true;
    int command = LayerCommands(index);
    if (command == kScriptEnd)
    {
        return;
    }

    command = LayerNote(index, command);
    while (majora_ && command == kScriptEnd && layer.delay == 0)
    {
        command = LayerCommands(index);
        if (command == kScriptEnd)
        {
            return;
        }

        command = LayerNote(index, command);
    }

    int same_sound = kScriptEnd;
    if (command != kScriptEnd)
    {
        same_sound = LayerSound(index, command);
    }

    if (same_sound != kScriptEnd)
    {
        LayerVoice(index, same_sound != 0);
    }

    if (layer.muted && (layer.voice != -1 || layer.continuous))
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }
}

int EadLaterPlayer::LayerCommands(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    while (true)
    {
        layer.command_place = script.pc;
        uint8_t command = Read8(player, script);
        if (command <= 0xC0)
        {
            return command;
        }

        if (command >= 0xF2)
        {
            const uint16_t argument = FlowArgument(player, script, command);
            if (FlowCommand(player, script, command, argument) == 0)
            {
                continue;
            }

            DisableLayer(index);

            return kScriptEnd;
        }

        switch (command)
        {
        case 0xC1:
            {
                const int value = Read8(player, script);
                layer.velocity_square = static_cast<float>(value * value) / (127.0F * 127.0F);
                break;
            }
        case 0xCA:
            layer.pan = Read8(player, script);
            break;
        case 0xC9:
            layer.gate = Read8(player, script);
            break;
        case 0xC2:
            layer.transpose = Read8(player, script);
            break;
        case 0xC4:
        case 0xC5:
            layer.continuous = command == 0xC4;
            layer.continuous_started = false;
            DecayOrRelease(index, AdsrPhase::kDecay);
            break;
        case 0xC3:
            layer.default_length = static_cast<int16_t>(ReadVariable(player, script));
            break;
        case 0xC6:
            {
                const uint8_t program = Read8(player, script);
                if (program >= 0x7E)
                {
                    if (program == 0x7E)
                    {
                        layer.program = 1;
                    }
                    else if (program == 0x7F)
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
                auto note = Read8(player, script);
                note = static_cast<uint8_t>(note + channel.transpose);
                note = static_cast<uint8_t>(note + layer.transpose);
                note = static_cast<uint8_t>(note + player.transpose);
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
            layer.adsr.envelope.offset = static_cast<uint16_t>(Read16(player, script));
            layer.adsr.release = Read8(player, script);
            break;
        case 0xCF:
            layer.adsr.release = Read8(player, script);
            break;
        case 0xCC:
            layer.ignore_drum_pan = true;
            break;
        case 0xCD:
            layer.stereo = Read8(player, script);
            break;
        case 0xCE:
            layer.bend = tables::kBendFine[static_cast<uint8_t>(Read8(player, script) + 0x80)];
            break;
        case 0xF0:
            if (majora_)
            {
                layer.flags = static_cast<uint16_t>(layer.flags & ~static_cast<uint16_t>(Read16(player, script)));
            }

            break;
        case 0xF1:
            if (majora_)
            {
                layer.surround = Read8(player, script);
            }

            break;
        default:
            if ((command & 0xF0) == 0xD0)
            {
                const int place = player.short_velocities;
                const int value = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                             : tables::kShortVelocity[command & 0xFU];
                layer.velocity_square = static_cast<float>(value * value) / (127.0F * 127.0F);
            }
            else if ((command & 0xF0) == 0xE0)
            {
                const int place = player.short_gates;
                layer.gate = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                        : tables::kShortGate[command & 0xFU];
            }

            break;
        }
    }
}

int EadLaterPlayer::LayerNote(int index, int command)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    if (command == 0xC0)
    {
        layer.delay = static_cast<int16_t>(ReadVariable(player, script));
        layer.muted = true;
        layer.continuous_started = false;

        return kScriptEnd;
    }

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

        if (velocity > 0x7F)
        {
            velocity = 0x7F;
        }

        const auto value = static_cast<float>(velocity);
        layer.velocity_square = value * value / (127.0F * 127.0F);
        command -= command & 0xC0;
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

        command -= command & 0xC0;
    }

    if (channel.velocity_random != 0)
    {
        float change = layer.velocity_square * static_cast<float>(random_ % channel.velocity_random) / 100.0F;
        if ((random_ & 0x8000) != 0)
        {
            change = -change;
        }

        layer.velocity_varied = layer.velocity_square + change;
        if (layer.velocity_varied < 0.0F)
        {
            layer.velocity_varied = 0.0F;
        }
        else if (layer.velocity_varied > 1.0F)
        {
            layer.velocity_varied = 1.0F;
        }
    }
    else
    {
        layer.velocity_varied = layer.velocity_square;
    }

    layer.delay = static_cast<int16_t>(delay);
    layer.gate_delay = static_cast<int16_t>((layer.gate * int32_t{delay}) >> 8);
    if (channel.gate_random != 0)
    {
        // The engine takes the velocity's range here. The model avoids a division by zero when a channel has no range.
        const uint32_t range = channel.velocity_random != 0 ? channel.velocity_random : 1U;
        int change = (layer.gate_delay * static_cast<int>(random_ % range)) / 100;
        if ((random_ & 0x4000) != 0)
        {
            change = -change;
        }

        layer.gate_delay = static_cast<int16_t>(layer.gate_delay + change);
        if (layer.gate_delay < 0)
        {
            layer.gate_delay = 0;
        }
        else if (layer.gate_delay > layer.delay)
        {
            layer.gate_delay = layer.delay;
        }
    }

    if ((player.muted && (channel.mute_behavior & (kMuteStopNotes | kMuteStopLayerNotes)) != 0) || channel.muted)
    {
        layer.muted = true;

        return kScriptEnd;
    }

    if (player.skip_ticks != 0)
    {
        layer.muted = true;

        return kScriptEnd;
    }

    return command;
}

int EadLaterPlayer::LayerSound(int index, int command)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    bool same_sound = true;
    auto semitone = static_cast<uint8_t>(command);
    int program = layer.program;
    if (program == 0xFF)
    {
        if (!channel.has_instrument)
        {
            return kScriptEnd;
        }

        program = channel.program;
    }

    if (program == 0)
    {
        semitone = static_cast<uint8_t>(semitone + channel.transpose + layer.transpose);
        layer.semitone = semitone;
        const EadDrum* drum = Drum(channel.font, semitone);
        if (drum == nullptr)
        {
            layer.muted = true;
            layer.delay_copy = layer.delay;

            return kScriptEnd;
        }

        layer.adsr.envelope = {};
        layer.adsr.envelope.bank =
            drum->envelope == kNoObject
                ? nullptr
                : &files_->banks[channel.font].envelopes[static_cast<std::size_t>(drum->envelope)];
        layer.adsr.release = drum->release_rate;
        if (!layer.ignore_drum_pan)
        {
            layer.pan = drum->pan;
        }

        layer.sound = &drum->sound;
        layer.sound_font = channel.font;
        layer.frequency_scale = drum->sound.tuning;
    }
    else if (program == 1)
    {
        layer.semitone = semitone;
        const int number = (layer.transpose << 6) + semitone;
        const EadSoundEffect* effect = SoundEffect(channel.font, number);
        if (effect == nullptr)
        {
            layer.muted = true;
            layer.delay_copy = static_cast<int16_t>(layer.delay + 1);

            return kScriptEnd;
        }

        layer.sound = &effect->sound;
        layer.sound_font = channel.font;
        layer.frequency_scale = effect->sound.tuning;
    }
    else
    {
        semitone = static_cast<uint8_t>(semitone + player.transpose + channel.transpose + layer.transpose);
        layer.semitone = semitone;
        if (semitone >= 0x80)
        {
            layer.muted = true;

            return kScriptEnd;
        }

        const EadInstrument* instrument = layer.program == 0xFF ? channel.instrument : layer.instrument;
        const auto sound_of = [](const EadInstrument& held, int note) -> const EadTunedSample*
        {
            if (note < held.normal_range_low)
            {
                return &held.low;
            }

            return note <= held.normal_range_high ? &held.normal : &held.high;
        };
        const int note_mode = layer.portamento.mode & ~kPortamentoSpecial;
        if (layer.portamento.mode != 0)
        {
            const int highest = semitone > layer.portamento_target ? semitone : layer.portamento_target;
            float tuning = 1.0F;
            if (instrument != nullptr)
            {
                const EadTunedSample* sound = sound_of(*instrument, highest);
                same_sound = layer.sound == sound;
                layer.sound = sound;
                layer.sound_font = channel.font;
                tuning = sound->tuning;
            }
            else
            {
                layer.sound = nullptr;
            }

            const float from_note = tables::kPitchFrequencies[semitone] * tuning;
            const float to_note = tables::kPitchFrequencies[layer.portamento_target] * tuning;
            float base = from_note;
            float other = from_note;
            switch (note_mode)
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

            layer.portamento.extent = (other / base) - 1.0F;
            int speed = 0;
            if ((layer.portamento.mode & kPortamentoSpecial) != 0)
            {
                speed = int32_t{player.tempo} * 0x8000 / max_tempo_;
                if (layer.delay != 0)
                {
                    speed = speed * 0x100 / (layer.delay * int32_t{layer.portamento_time});
                }
            }
            else
            {
                const int denominator = int32_t{layer.portamento_time} * updates_per_frame_ / 3;
                speed = denominator != 0 ? 0x20000 / denominator : 0x7FFF;
            }

            speed = std::clamp(speed, 1, 0x7FFF);
            layer.portamento.speed = static_cast<uint16_t>(speed);
            layer.portamento.position = 0;
            layer.frequency_scale = base;
            if (note_mode == 5)
            {
                layer.portamento_target = semitone;
            }
        }
        else if (instrument != nullptr)
        {
            const EadTunedSample* sound = sound_of(*instrument, semitone);
            same_sound = sound == layer.sound;
            layer.sound = sound;
            layer.sound_font = channel.font;
            layer.frequency_scale = tables::kPitchFrequencies[semitone] * sound->tuning;
        }
        else
        {
            layer.sound = nullptr;
            layer.frequency_scale = tables::kPitchFrequencies[semitone];
        }
    }

    layer.delay_copy = layer.delay;
    layer.frequency_scale *= layer.bend;
    if (layer.delay == 0)
    {
        float time = 0.0F;
        const EadSample* sample = SampleOf(layer.sound_font, layer.sound);
        if (sample != nullptr && sample->loop != kNoObject)
        {
            time =
                static_cast<float>(files_->banks[layer.sound_font].loops[static_cast<std::size_t>(sample->loop)].end);
        }

        time *= static_cast<float>(player.tempo);
        time *= note_length_scale_;
        time /= layer.frequency_scale;
        if (time > 32766.0F)
        {
            time = 32766.0F;
        }

        layer.gate_delay = 0;
        layer.delay = static_cast<int16_t>(static_cast<uint16_t>(static_cast<int32_t>(time)) + 1);
        if (layer.portamento.mode != 0 && (layer.portamento.mode & kPortamentoSpecial) != 0)
        {
            int speed = int32_t{player.tempo} * 0x8000 / max_tempo_;
            const int denominator = layer.delay * int32_t{layer.portamento_time};
            speed = denominator != 0 ? speed * 0x100 / denominator : 0x7FFF;
            layer.portamento.speed = static_cast<uint16_t>(std::clamp(speed, 1, 0x7FFF));
        }
    }

    return same_sound ? 1 : 0;
}

void EadLaterPlayer::LayerVoice(int index, bool same_sound)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const EadSample* sample = SampleOf(layer.sound_font, layer.sound);
    if (!majora_ && !layer.muted && sample != nullptr && sample->codec == kEadCodecS16InMemory)
    {
        layer.muted = true;

        return;
    }

    if (layer.continuous && layer.continuous_started)
    {
        return;
    }

    bool started = false;
    if (layer.continuous && layer.voice != -1 && layer.loaded && same_sound &&
        voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        if (layer.sound == nullptr)
        {
            InitWave(voices_[static_cast<std::size_t>(layer.voice)], layer);
        }
    }
    else
    {
        if (!same_sound)
        {
            DecayOrRelease(index, AdsrPhase::kDecay);
        }

        layer.voice = AllocVoice(index);
        if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
        {
            InitVibrato(layer.voice);
        }

        started = true;
    }

    if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        Voice& voice = voices_[static_cast<std::size_t>(layer.voice)];
        voice.portamento_scale = 1.0F;
        voice.portamento = layer.portamento;
    }

    if (layer.voice != -1)
    {
        voices_[static_cast<std::size_t>(layer.voice)].track = layer.channel;
    }

    if (started && on_note)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
        EadNoteStart start;
        start.player = channel.player;
        start.channel = layer.channel - channel.player * kChannelsPerPlayer;
        for (int i = 0; i < kLayersPerChannel; i++)
        {
            if (channel.layers[static_cast<std::size_t>(i)] == index)
            {
                start.layer = i;
            }
        }

        start.key = layer.semitone;
        const int program = layer.program == 0xFF ? channel.program : layer.program;
        start.drum = program == 0;
        start.bank = channel.font;
        start.program = static_cast<int16_t>(program);
        start.velocity = layer.velocity_varied;
        start.pan = static_cast<float>(layer.pan) / 128.0F;
        start.length = layer.delay;
        start.gate = layer.gate_delay;
        start.offset = layer.command_place;
        start.update = updates_;
        start.voice = layer.voice;
        on_note(start);
    }
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
            // Stadium 2's code at 0x8001B7A0 to 0x8001B8BC. Each of three flags of the layer lets the channel's value
            // reach it. With the pan's flag clear, the code writes the layer's pan to the note's velocity.
            const bool all = layer.properties_need_init;
            layer.properties_need_init = false;
            if (all || channel.change_frequency)
            {
                layer.note_frequency = (layer.flags & kLayerFlagFrequency) != 0 ? layer.frequency_scale * frequency
                                                                                : layer.frequency_scale;
            }

            if (all || channel.change_volume || recalculate_volume)
            {
                layer.note_velocity = (layer.flags & kLayerFlagVolume) != 0
                                          ? layer.velocity_varied * channel.applied_volume
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

            continue;
        }

        if (layer.properties_need_init)
        {
            layer.note_frequency = layer.frequency_scale * frequency;
            layer.note_velocity = layer.velocity_varied * channel.applied_volume;
            layer.note_pan = note_pan;
            layer.properties_need_init = false;
        }
        else
        {
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
    }

    channel.change_frequency = false;
    channel.change_volume = false;
    channel.change_pan = false;
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

            if (adsr.phase != AdsrPhase::kFade)
            {
                break;
            }

            [[fallthrough]];
        }
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
        adsr.current -= adsr.fade_step;
        if (adsr.sustain != 0.0F && phase == AdsrPhase::kDecay)
        {
            if (adsr.current < adsr.sustain)
            {
                adsr.current = adsr.sustain;
                adsr.delay = 128;
                adsr.phase = AdsrPhase::kSustain;
            }

            break;
        }

        if (adsr.current < 0.00001F)
        {
            adsr.current = 0.0F;
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
    Attributes& attributes = voice.attributes;
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

void EadLaterPlayer::ProcessVoices(int state_offset)
{
    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        Voice& voice = voices_[i];
        const int index = static_cast<int>(i);
        UpdateState& recorded = update_states_[static_cast<std::size_t>(state_offset) + i];
        if (voice.layer != kNoLayer)
        {
            Layer& layer = layers_[static_cast<std::size_t>(voice.layer)];
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
                if (voice.wanted_layer != kNoLayer)
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
                else
                {
                    if (voice.layer != kNoLayer)
                    {
                        layers_[static_cast<std::size_t>(voice.layer)].continuous_started = true;
                    }

                    DisableVoice(voice);
                    Remove(index);
                    PushBack(Head(voice.pool, kDisabled), index);
                    continue;
                }
            }
        }
        else if (voice.adsr.phase == AdsrPhase::kDisabled)
        {
            if (voice.layer != kNoLayer)
            {
                layers_[static_cast<std::size_t>(voice.layer)].continuous_started = true;
            }

            DisableVoice(voice);
            Remove(index);
            PushBack(Head(voice.pool, kDisabled), index);
            continue;
        }

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
            if (players_[static_cast<std::size_t>(channel.player)].muted &&
                (channel.mute_behavior & kMuteStopVoices) != 0)
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

void EadLaterPlayer::StartReverbItems(AudioMemory& memory, int length, int update, int reverb_index)
{
    Reverb& reverb = reverbs_[static_cast<std::size_t>(reverb_index)];
    ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (!majora_ && reverb.downsample >= 2 && reverb.frames_to_ignore == 0)
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
    const int start = reverb.next_position;
    if (extra < 0)
    {
        item.length_a = static_cast<int16_t>(samples * 2);
        item.length_b = 0;
        item.start = reverb.next_position;
        reverb.next_position += samples;
    }
    else
    {
        item.length_a = static_cast<int16_t>((samples - extra) * 2);
        item.length_b = static_cast<int16_t>(extra * 2);
        item.start = reverb.next_position;
        reverb.next_position = extra;
    }

    if (reverb.second_delay != 0)
    {
        int second = reverb.second_delay + start;
        if (second >= reverb.buffer_samples)
        {
            second -= reverb.buffer_samples;
        }

        ReverbItem& later = reverb.second_items[reverb.current_frame][static_cast<std::size_t>(update)];
        const int later_extra = second + samples - reverb.buffer_samples;
        later.start = second;
        if (later_extra < 0)
        {
            later.length_a = static_cast<int16_t>(samples * 2);
            later.length_b = 0;
        }
        else
        {
            later.length_a = static_cast<int16_t>((samples - later_extra) * 2);
            later.length_b = static_cast<int16_t>(later_extra * 2);
        }
    }
}

void EadLaterPlayer::LoadReverbSamples(AudioList& list, const Reverb& reverb, int length, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    const auto load_part = [&](uint32_t dmem, uint32_t start, uint32_t size)
    {
        Load(list, reverb.left + start * 2, dmem, size);
        Load(list, reverb.right + start * 2, dmem + kOneChannel, size);
    };
    if (reverb.downsample == 1)
    {
        load_part(dmem_.wet_left, static_cast<uint32_t>(item.start), static_cast<uint32_t>(item.length_a));
        if (item.length_b != 0)
        {
            load_part(dmem_.wet_left + static_cast<uint32_t>(item.length_a), 0, static_cast<uint32_t>(item.length_b));
        }

        return;
    }

    const int offset_a = (item.start & 7) * 2;
    const int offset_b = Align16(offset_a + item.length_a);
    load_part(dmem_.wet_temp, static_cast<uint32_t>(item.start - offset_a / 2), kOneChannel);
    if (item.length_b != 0)
    {
        load_part(dmem_.wet_temp + static_cast<uint32_t>(offset_b), 0, kOneChannel - static_cast<uint32_t>(offset_b));
    }

    SetBuffer(list, dmem_.wet_temp + static_cast<uint32_t>(offset_a), dmem_.wet_left,
              static_cast<uint32_t>(length) * 2);
    Resample(list, reverb.resample_flags, reverb.downsample_pitch, reverb.resample_states[0]);
    SetBuffer(list, dmem_.wet_temp + kOneChannel + static_cast<uint32_t>(offset_a), dmem_.wet_right,
              static_cast<uint32_t>(length) * 2);
    Resample(list, reverb.resample_flags, reverb.downsample_pitch, reverb.resample_states[1]);
}

void EadLaterPlayer::SaveReverbSamples(AudioList& list, Reverb& reverb, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample == 1)
    {
        const auto save_part = [&](uint32_t dmem, uint32_t start, uint32_t size)
        {
            Save(list, dmem, reverb.left + start * 2, size);
            Save(list, dmem + kOneChannel, reverb.right + start * 2, size);
        };
        save_part(dmem_.wet_left, static_cast<uint32_t>(item.start), static_cast<uint32_t>(item.length_a));
        if (item.length_b != 0)
        {
            save_part(dmem_.wet_left + static_cast<uint32_t>(item.length_a), 0, static_cast<uint32_t>(item.length_b));
        }
    }
    else if (majora_)
    {
        int rate = reverb.downsample;
        int count = 13 * kFrameSamples;
        while (rate >= 2)
        {
            list.commands.push_back(
                {(kOpHalve << 24) | static_cast<uint32_t>(count), Pair(dmem_.wet_left, dmem_.wet_left)});
            list.commands.push_back(
                {(kOpHalve << 24) | static_cast<uint32_t>(count), Pair(dmem_.wet_right, dmem_.wet_right)});
            rate >>= 1;
            count >>= 1;
        }

        const auto first = static_cast<uint32_t>(item.length_a);
        const auto second = static_cast<uint32_t>(item.length_b);
        const auto place = static_cast<uint32_t>(item.start) * 2;
        if (first != 0)
        {
            SaveAligned(list, dmem_.wet_left, first, reverb.left + place);
            SaveAligned(list, dmem_.wet_right, first, reverb.right + place);
        }

        if (second != 0)
        {
            SaveAligned(list, dmem_.wet_left + first, second, reverb.left);
            SaveAligned(list, dmem_.wet_right + first, second, reverb.right);
        }
    }
    else
    {
        Save(list, dmem_.wet_left, item.downsample_left, kTwoChannels);
    }

    reverb.resample_flags = 0;
}

void EadLaterPlayer::SaveAligned(AudioList& list, uint32_t dmem, uint32_t size, uint32_t address)
{
    const uint32_t end_extra = (address + size) & 0xF;
    if (end_extra != 0)
    {
        Load(list, address + size - end_extra, dmem_.temp, 0x10);
        Move(list, dmem, kSmallTemp, size);
        Move(list, dmem_.temp + end_extra, size + kSmallTemp, 0x10 - end_extra);
        size += 0x10 - end_extra;
        dmem = kSmallTemp;
    }

    const uint32_t start_extra = address & 0xF;
    if (start_extra != 0)
    {
        Load(list, address - start_extra, dmem_.temp, 0x10);
        Move(list, dmem, start_extra + dmem_.temp, size);
        size += start_extra;
        dmem = dmem_.temp;
    }

    Save(list, dmem, address - start_extra, size);
}

void EadLaterPlayer::SendReverb(AudioList& list, const Reverb& reverb, int update)
{
    if (static_cast<std::size_t>(reverb.send_to) >= reverbs_.size())
    {
        return;
    }

    const Reverb& target = reverbs_[static_cast<std::size_t>(reverb.send_to)];
    if (target.downsample != 1)
    {
        return;
    }

    const ReverbItem& item = target.items[target.current_frame][static_cast<std::size_t>(update)];
    const auto load_part = [&](uint32_t dmem, uint32_t start, uint32_t size)
    {
        Load(list, target.left + start * 2, dmem, size);
        Load(list, target.right + start * 2, dmem + kOneChannel, size);
    };
    const auto save_part = [&](uint32_t dmem, uint32_t start, uint32_t size)
    {
        Save(list, dmem, target.left + start * 2, size);
        Save(list, dmem + kOneChannel, target.right + start * 2, size);
    };
    load_part(dmem_.wet_temp, static_cast<uint32_t>(item.start), static_cast<uint32_t>(item.length_a));
    if (item.length_b != 0)
    {
        load_part(dmem_.wet_temp + static_cast<uint32_t>(item.length_a), 0, static_cast<uint32_t>(item.length_b));
    }

    Mix(list, kTwoChannels, static_cast<uint16_t>(reverb.send_gain), dmem_.wet_left, dmem_.wet_temp);
    save_part(dmem_.wet_temp, static_cast<uint32_t>(item.start), static_cast<uint32_t>(item.length_a));
    if (item.length_b != 0)
    {
        save_part(dmem_.wet_temp + static_cast<uint32_t>(item.length_a), 0, static_cast<uint32_t>(item.length_b));
    }
}

void EadLaterPlayer::BuildUpdate(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update)
{
    const std::size_t base = voices_.size() * static_cast<std::size_t>(update);
    std::vector<int> order;
    order.reserve(voices_.size());
    const int reverb_count = static_cast<int>(reverbs_.size());
    if (reverb_count == 0)
    {
        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            if (update_states_[base + i].enabled)
            {
                order.push_back(static_cast<int>(i));
            }
        }
    }
    else
    {
        for (int reverb = 0; reverb < reverb_count; reverb++)
        {
            for (std::size_t i = 0; i < voices_.size(); i++)
            {
                const UpdateState& state = update_states_[base + i];
                if (state.enabled && state.reverb_index == reverb)
                {
                    order.push_back(static_cast<int>(i));
                }
            }
        }

        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            const UpdateState& state = update_states_[base + i];
            if (state.enabled && state.reverb_index >= reverb_count)
            {
                order.push_back(static_cast<int>(i));
            }
        }
    }

    Clear(list, dmem_.left, kTwoChannels);
    std::size_t at = 0;
    for (int reverb_index = 0; reverb_index < reverb_count; reverb_index++)
    {
        Reverb& reverb = reverbs_[static_cast<std::size_t>(reverb_index)];
        const bool use_reverb = reverb.enabled;
        const bool second = reverb.second_delay != 0;
        const auto save_second = [&]
        {
            const ReverbItem& item = reverb.second_items[reverb.current_frame][static_cast<std::size_t>(update)];
            const auto save_part = [&](uint32_t dmem, uint32_t start, uint32_t size)
            {
                Save(list, dmem, reverb.left + start * 2, size);
                Save(list, dmem + kOneChannel, reverb.right + start * 2, size);
            };
            save_part(dmem_.wet_left, static_cast<uint32_t>(item.start), static_cast<uint32_t>(item.length_a));
            if (item.length_b != 0)
            {
                save_part(dmem_.wet_left + static_cast<uint32_t>(item.length_a), 0,
                          static_cast<uint32_t>(item.length_b));
            }
        };
        if (use_reverb)
        {
            LoadReverbSamples(list, reverb, length, update);
            Mix(list, kTwoChannels, static_cast<uint16_t>(reverb.volume), dmem_.wet_left, dmem_.left);
            if (second)
            {
                Move(list, dmem_.wet_left, dmem_.wet_temp, kTwoChannels);
            }

            Mix(list, kTwoChannels, (reverb.decay + 0x8000U) & 0xFFFF, dmem_.wet_left, dmem_.wet_left);
            if ((reverb.leak_right_to_left != 0 || reverb.leak_left_to_right != 0) &&
                config_.output != EadLaterOutput::kMono)
            {
                Move(list, dmem_.wet_left, dmem_.wet_scratch, kOneChannel);
                Mix(list, kOneChannel, static_cast<uint16_t>(reverb.leak_right_to_left), dmem_.wet_right,
                    dmem_.wet_left);
                Mix(list, kOneChannel, static_cast<uint16_t>(reverb.leak_left_to_right), dmem_.wet_scratch,
                    dmem_.wet_right);
            }

            if (second)
            {
                if (majora_ && reverb.send_to != -1)
                {
                    SendReverb(list, reverb, update);
                }

                SaveReverbSamples(list, reverb, update);
                if (!majora_ && reverb.send_to != -1)
                {
                    SendReverb(list, reverb, update);
                }

                if (reverb.downsample == 1)
                {
                    const ReverbItem& item =
                        reverb.second_items[reverb.current_frame][static_cast<std::size_t>(update)];
                    const auto load_part = [&](uint32_t dmem, uint32_t start, uint32_t size)
                    {
                        Load(list, reverb.left + start * 2, dmem, size);
                        Load(list, reverb.right + start * 2, dmem + kOneChannel, size);
                    };
                    load_part(dmem_.wet_left, static_cast<uint32_t>(item.start), static_cast<uint32_t>(item.length_a));
                    if (item.length_b != 0)
                    {
                        load_part(dmem_.wet_left + static_cast<uint32_t>(item.length_a), 0,
                                  static_cast<uint32_t>(item.length_b));
                    }
                }

                Mix(list, kTwoChannels, static_cast<uint16_t>(reverb.second_gain), dmem_.wet_temp, dmem_.wet_left);
            }
        }

        while (at < order.size())
        {
            const int voice = order[at];
            UpdateState& state = update_states_[base + static_cast<std::size_t>(voice)];
            if (state.reverb_index != reverb_index)
            {
                break;
            }

            list.MarkVoice(voices_[static_cast<std::size_t>(voice)].track);
            BuildVoice(list, memory, voice, state, length, update);
            at++;
        }

        list.MarkVoicesEnd();
        if (use_reverb)
        {
            const uint32_t size = static_cast<uint32_t>(length) * 2;
            if (reverb.filter_left != 0)
            {
                Filter(list, 2, size, reverb.filter_left);
                Filter(list, reverb.resample_flags, dmem_.wet_left, reverb.filter_left_state);
            }

            if (reverb.filter_right != 0)
            {
                Filter(list, 2, size, reverb.filter_right);
                Filter(list, reverb.resample_flags, dmem_.wet_right, reverb.filter_right_state);
            }

            if (second)
            {
                save_second();
            }
            else
            {
                if (majora_ && reverb.send_to != -1)
                {
                    SendReverb(list, reverb, update);
                }

                SaveReverbSamples(list, reverb, update);
                if (!majora_ && reverb.send_to != -1)
                {
                    SendReverb(list, reverb, update);
                }
            }
        }
    }

    while (at < order.size())
    {
        const int voice = order[at];
        list.MarkVoice(voices_[static_cast<std::size_t>(voice)].track);
        BuildVoice(list, memory, voice, update_states_[base + static_cast<std::size_t>(voice)], length, update);
        at++;
    }

    list.MarkChunkEnd();
    list.commands.push_back(
        {Word0(kOpInterleave, (static_cast<uint32_t>(length) * 2) >> 4, dmem_.temp), Pair(dmem_.left, dmem_.right)});
    Save(list, dmem_.temp, output, static_cast<uint32_t>(length) * 4);
}

void EadLaterPlayer::BuildWaveLoad(AudioList& list, const UpdateState& state, Voice& voice, int to_load)
{
    if (state.book_offset != 0)
    {
        // Noise: the engine reads on through memory from the end of its last wave.
        Load(list, waves_address_ + noise_offset_, dmem_.decoded, static_cast<uint32_t>(Align16(to_load * 2)));
        noise_offset_ += static_cast<uint32_t>(to_load) * 2;

        return;
    }

    Load(list, state.wave_address, dmem_.decoded, kWaveSamples * 2);
    int position = voice.position;
    if (state.harmonics != 0)
    {
        static constexpr std::array<int, 4> kSamplesInPeriod = {kWaveSamples, kWaveSamples / 2, kWaveSamples / 4,
                                                                kWaveSamples / 8};
        position = position * kSamplesInPeriod[state.harmonics >> 2U] / kSamplesInPeriod[state.harmonics & 3U];
    }

    position = static_cast<int>(static_cast<uint32_t>(position) % kWaveSamples);
    const int available = kWaveSamples - position;
    if (to_load > available)
    {
        const int copies = (to_load - available + kWaveSamples - 1) / kWaveSamples;
        if (copies != 0)
        {
            list.commands.push_back({Word0(kOpDuplicate, static_cast<uint32_t>(copies), dmem_.decoded),
                                     Pair(dmem_.decoded + kWaveSamples * 2, 0x80)});
        }
    }

    voice.position = position;
}

void EadLaterPlayer::BuildVoice(AudioList& list, AudioMemory& memory, int voice_index, UpdateState& state, int length,
                                int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    const int book_offset = state.book_offset;
    bool finished = state.finished;
    uint32_t flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        voice.restart = false;
        voice.position = static_cast<int32_t>(voice.start_position);
        voice.position_fraction = 0;
        if (voice.adsr.start_at_target)
        {
            // The code at 0x8000E810 in Pokémon Stadium 2. A note with a skip point has no ramp from silence.
            voice.current_left = static_cast<int16_t>(state.target_left << 4);
            voice.current_right = static_cast<int16_t>(state.target_right << 4);
        }
        else
        {
            voice.current_left = 0;
            voice.current_right = 0;
        }

        voice.previous_delay_left = 0;
        voice.previous_delay_right = 0;
        voice.reverb = state.reverb;
        voice.parts = 0;
        voice.stop_loop = false;
        voice.comb_needs_init = true;
        voice.state.finished = false;
        finished = false;
    }

    const uint16_t pitch = state.pitch;
    const int parts = state.two_parts ? 2 : 1;
    const uint32_t fixed_length = static_cast<uint32_t>(int32_t{pitch} * length * 2) + voice.position_fraction;
    const auto to_load = static_cast<int32_t>(fixed_length >> 16);
    int skip_bytes = 0;
    voice.position_fraction = static_cast<uint16_t>(fixed_length);
    voice.parts = static_cast<uint8_t>(parts);
    uint32_t before_resampling = 0;
    if (state.synthetic_wave)
    {
        BuildWaveLoad(list, state, voice, to_load);
        before_resampling = dmem_.decoded + static_cast<uint32_t>(voice.position) * 2;
        voice.position += to_load;
    }
    else
    {
        const EadSample* sample = SampleOf(state.sound_font, state.sound);
        if (sample == nullptr || sample->loop == kNoObject)
        {
            return;
        }

        EnsureBank(memory, state.sound_font);
        const EadBank& bank = files_->banks[state.sound_font];
        const BankLoop& loop = bank.loops[static_cast<std::size_t>(sample->loop)];
        if (majora_ && voice.release_kind != 0)
        {
            voice.stop_loop = true;
        }

        const bool ends_early = majora_ && loop.count == 2 && voice.stop_loop;
        const auto end_position = static_cast<int32_t>(ends_early ? loop.sample_end : loop.end);
        const uint32_t place = sample->medium < 2 ? files_->sample_places[state.sound_font][sample->medium] : 0;
        const uint32_t sample_address = AudioMemory::kImageBase + place + sample->address;
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

            if ((sample->codec == kEadCodecAdpcm || sample->codec == kEadCodecSmallAdpcm) && sample->book != kNoObject)
            {
                const uint32_t book_address = book_addresses_[state.sound_font][static_cast<std::size_t>(sample->book)];
                if (current_book_ != book_address)
                {
                    current_book_ = book_offset == 1 ? note_book_address_ + 2 : book_address;
                    const VadpcmBook& book = bank.books[static_cast<std::size_t>(sample->book)].book;
                    const auto entries = static_cast<uint32_t>(kFrameSamples * book.order * book.predictor_count);
                    list.commands.push_back({(kOpLoadBook << 24) | entries, current_book_});
                }
            }

            while (processed != wanted)
            {
                bool note_finished = false;
                bool restart = false;
                int moved_to = 0;
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
                    if (loop.count != 0 && !ends_early)
                    {
                        restart = true;
                    }
                    else
                    {
                        note_finished = true;
                    }
                }

                int frame_size = 9;
                bool decodes = true;
                if (sample->codec == kEadCodecSmallAdpcm)
                {
                    frame_size = 5;
                }
                else if (sample->codec == kEadCodecS8)
                {
                    frame_size = 16;
                }
                else if (majora_ && sample->codec == kCodecRawSeven)
                {
                    frame_size = 4;
                }
                else if (majora_ && sample->codec == kEadCodecReverb)
                {
                    note_finished = true;
                    decodes = false;
                }
                else if (sample->codec == kEadCodecS16 || sample->codec == kEadCodecS16InMemory ||
                         (majora_ && sample->codec == kCodecNoDecodeSix))
                {
                    Clear(list, dmem_.decoded, static_cast<uint32_t>(wanted + kFrameSamples) * 2);
                    flags = kFlagContinue;
                    skip_bytes = 0;
                    processed = wanted;
                    written = wanted;
                    decodes = false;
                }

                if (decodes)
                {
                    uint32_t start_pad = 0;
                    if (frames != 0)
                    {
                        const int frame = (voice.position + kFrameSamples - ignored_first) / kFrameSamples;
                        const uint32_t data = sample_address + static_cast<uint32_t>(frame * frame_size);
                        start_pad = data & 0xF;
                        const int aligned = Align16(frames * frame_size + kFrameSamples);
                        Load(list, data - start_pad, dmem_.compressed_end - static_cast<uint32_t>(aligned),
                             static_cast<uint32_t>(aligned));
                    }
                    else
                    {
                        to_decode = 0;
                        start_pad = 0;
                    }

                    if (voice.restart)
                    {
                        list.commands.push_back(
                            {kOpSetLoop << 24,
                             loop_addresses_[state.sound_font][static_cast<std::size_t>(sample->loop)]});
                        flags = kFlagLoop;
                        voice.restart = false;
                    }

                    const int this_time = to_decode + in_first - trailing;
                    if (processed == 0)
                    {
                        skip_bytes = ignored_first * 2;
                    }
                    else
                    {
                        moved_to = Align16(written + 16);
                    }

                    const int aligned = Align16(frames * frame_size + kFrameSamples);
                    const uint32_t compressed = dmem_.compressed_end - static_cast<uint32_t>(aligned);
                    if (sample->codec == kEadCodecAdpcm || sample->codec == kEadCodecSmallAdpcm)
                    {
                        SetBuffer(list, compressed + start_pad, dmem_.decoded + static_cast<uint32_t>(moved_to),
                                  static_cast<uint32_t>(to_decode) * 2);
                        const uint32_t codec_flags = sample->codec == kEadCodecSmallAdpcm ? flags | 4U : flags;
                        list.commands.push_back({Word0(kOpAdpcm, codec_flags, 0), voice.buffers.adpcm_state});
                    }
                    else if (sample->codec == kEadCodecS8)
                    {
                        SetBuffer(list, compressed + start_pad, dmem_.decoded + static_cast<uint32_t>(moved_to),
                                  static_cast<uint32_t>(to_decode) * 2);
                        list.commands.push_back({Word0(kOpDecodeBytes, flags, 0), voice.buffers.adpcm_state});
                    }

                    if (processed != 0)
                    {
                        Move(list,
                             dmem_.decoded + static_cast<uint32_t>(moved_to) + static_cast<uint32_t>(ignored_first) * 2,
                             dmem_.decoded + static_cast<uint32_t>(written), static_cast<uint32_t>(this_time) * 2);
                    }

                    processed += this_time;
                    if (flags == kFlagInit)
                    {
                        skip_bytes = kFrameSamples * 2;
                        written = (to_decode + kFrameSamples) * 2;
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
                }

                if (note_finished)
                {
                    if (!majora_ || stadium_2_ || wanted != processed)
                    {
                        Clear(list, dmem_.decoded + static_cast<uint32_t>(written),
                              static_cast<uint32_t>(wanted - processed) * 2);
                    }

                    finished = true;
                    voice.state.finished = true;
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
                before_resampling = dmem_.decoded + static_cast<uint32_t>(skip_bytes);
            }
            else if (part == 0)
            {
                list.commands.push_back(
                    {(kOpHalve << 24) | static_cast<uint32_t>(Align8(wanted / 2) & 0xFFFF),
                     Pair(dmem_.decoded + static_cast<uint32_t>(skip_bytes), dmem_.temp + kFrameSamples * 2)});
                resampled_length = wanted;
                before_resampling = dmem_.temp + kFrameSamples * 2;
                if (finished)
                {
                    Clear(list, before_resampling + static_cast<uint32_t>(resampled_length),
                          static_cast<uint32_t>(wanted + kFrameSamples));
                }
            }
            else
            {
                list.commands.push_back(
                    {(kOpHalve << 24) | static_cast<uint32_t>(Align8(wanted / 2) & 0xFFFF),
                     Pair(dmem_.decoded + static_cast<uint32_t>(skip_bytes),
                          dmem_.temp + kFrameSamples * 2 + static_cast<uint32_t>(resampled_length))});
            }

            if (finished)
            {
                break;
            }
        }
    }

    flags = kFlagContinue;
    if (state.needs_init)
    {
        state.needs_init = false;
        flags = kFlagInit;
    }

    const uint32_t bytes = static_cast<uint32_t>(length) * 2;
    if (pitch == 0)
    {
        Clear(list, dmem_.temp, bytes);
    }
    else
    {
        SetBuffer(list, before_resampling, dmem_.temp, bytes);
        Resample(list, flags, pitch, voice.buffers.resample_state);
    }

    if (book_offset == 3)
    {
        list.commands.push_back({Word0(kOpNegate, 0, bytes), Pair(dmem_.temp, dmem_.temp)});
    }

    if (book_offset == 2)
    {
        list.commands.push_back({Word0(kOpMultiply, 0, bytes), Pair(dmem_.temp, dmem_.temp)});
    }

    int gain = state.gain;
    if (gain != 0)
    {
        if (gain < 0x10)
        {
            gain = 0x10;
        }

        list.commands.push_back(
            {Word0(kOpGain, static_cast<uint32_t>(gain), static_cast<uint32_t>(length + kFrameSamples) * 2),
             Pair(dmem_.temp, 0)});
    }

    if (state.filter)
    {
        const std::array<int16_t, 8> taps = FilterTaps(voice, *state.filter);
        for (std::size_t i = 0; i < taps.size(); i++)
        {
            WriteBe16(memory, voice.buffers.filter_taps + static_cast<uint32_t>(i) * 2, taps[i]);
        }

        Filter(list, 2, bytes, voice.buffers.filter_taps);
        Filter(list, flags, dmem_.temp, voice.buffers.filter_state);
    }

    const uint32_t comb_size = state.comb_size;
    if (comb_size != 0 && state.comb_gain != 0)
    {
        Move(list, dmem_.temp, dmem_.comb_temp, bytes);
        const uint32_t comb_place = dmem_.comb_temp - comb_size;
        if (voice.comb_needs_init)
        {
            Clear(list, comb_place, comb_size);
            voice.comb_needs_init = false;
        }
        else
        {
            Load(list, voice.buffers.comb_state, comb_place, comb_size);
        }

        Save(list, dmem_.temp + bytes - comb_size, voice.buffers.comb_state, comb_size);
        Mix(list, bytes, state.comb_gain, dmem_.comb_temp, comb_place);
        Move(list, comb_place, dmem_.temp, bytes);
    }
    else
    {
        voice.comb_needs_init = true;
    }

    int delay_side = 0;
    if (state.delay_left != 0 || voice.previous_delay_left != 0)
    {
        delay_side = 1;
    }
    else if (state.delay_right != 0 || voice.previous_delay_right != 0)
    {
        delay_side = 2;
    }

    if (majora_ && config_.output == EadLaterOutput::kSurroundEffects)
    {
        state.target_left = static_cast<uint16_t>(state.target_left >> 1);
        state.target_right = static_cast<uint16_t>(state.target_right >> 1);
        if (state.surround != 0xFF)
        {
            BuildSurround(list, state, voice, length, flags);
        }
    }

    BuildEnvelope(list, state, voice, length, delay_side);
    if (state.use_delay)
    {
        if ((flags & kFlagInit) == 0)
        {
            flags = kFlagContinue;
        }

        BuildDelay(list, state, voice, static_cast<int>(bytes), flags, delay_side);
    }
}

void EadLaterPlayer::BuildSurround(AudioList& list, UpdateState& state, Voice& voice, int length, uint32_t flags)
{
    const auto bytes = static_cast<uint32_t>(length) * 2;
    Move(list, dmem_.temp, dmem_.delay_temp, bytes);
    const uint16_t dry = voice.surround_gain;
    if (flags == kFlagInit)
    {
        Clear(list, kSurroundTemp, kSurroundStateSize);
        voice.surround_gain = 0;
    }
    else
    {
        Load(list, voice.buffers.surround_state, kSurroundTemp, kSurroundStateSize);
        Mix(list, bytes, dry, kSurroundTemp, dmem_.left);
        Mix(list, bytes, dry ^ 0xFFFFU, kSurroundTemp, dmem_.right);
        const int wet = (dry * voice.reverb) >> 7;
        Mix(list, bytes, static_cast<uint16_t>(wet), kSurroundTemp, dmem_.wet_left);
        Mix(list, bytes, static_cast<uint16_t>(wet ^ 0xFFFF), kSurroundTemp, dmem_.wet_right);
    }

    Save(list, kSurroundTemp + bytes, voice.buffers.surround_state, kSurroundStateSize);
    float decay = static_cast<float>(state.target_left + state.target_right) * (1.0F / 0x2000);
    decay = std::min(decay, 1.0F);
    decay *= tables::kDefaultPanVolume[static_cast<std::size_t>(127 - state.surround)];
    voice.surround_gain = static_cast<uint16_t>((decay * 0x7FFF + static_cast<float>(voice.surround_gain)) / 2);
    Move(list, dmem_.delay_temp, dmem_.temp, bytes);
}

void EadLaterPlayer::BuildEnvelope(AudioList& list, const UpdateState& state, Voice& voice, int length, int delay_side)
{
    const auto current_left = static_cast<uint16_t>(voice.current_left);
    const auto current_right = static_cast<uint16_t>(voice.current_right);
    auto target_left = static_cast<uint16_t>(state.target_left << 4);
    auto target_right = static_cast<uint16_t>(state.target_right << 4);
    if (majora_ && config_.output == EadLaterOutput::kSurroundEffects && state.surround != 0xFF)
    {
        const float scale = tables::kDefaultPanVolume[state.surround];
        target_left = static_cast<uint16_t>(static_cast<float>(target_left) * scale);
        target_right = static_cast<uint16_t>(static_cast<float>(target_right) * scale);
    }

    const int groups = length >> 3;
    const int16_t ramp_left =
        target_left != current_left ? static_cast<int16_t>((int32_t{target_left} - current_left) / groups) : int16_t{0};
    const int16_t ramp_right = target_right != current_right
                                   ? static_cast<int16_t>((int32_t{target_right} - current_right) / groups)
                                   : int16_t{0};
    const int source_reverb = voice.reverb;
    const int wet = source_reverb & 0x7F;
    int16_t ramp_reverb = 0;
    if (source_reverb != state.reverb)
    {
        ramp_reverb = static_cast<int16_t>((((state.reverb & 0x7F) - wet) << 9) / groups);
        voice.reverb = state.reverb;
    }

    voice.current_left = static_cast<int16_t>(current_left + ramp_left * groups);
    voice.current_right = static_cast<int16_t>(current_right + ramp_right * groups);

    uint32_t destinations =
        (dmem_.left >> 4 << 24) | (dmem_.right >> 4 << 16) | (dmem_.wet_left >> 4 << 8) | (dmem_.wet_right >> 4);
    if (state.use_delay)
    {
        Clear(list, dmem_.delay_temp, kOneChannel);
        if (delay_side == 1)
        {
            destinations = (dmem_.delay_temp >> 4 << 24) | (dmem_.right >> 4 << 16) | (dmem_.wet_left >> 4 << 8) |
                           (dmem_.wet_right >> 4);
        }
        else if (delay_side == 2)
        {
            destinations = (dmem_.left >> 4 << 24) | (dmem_.delay_temp >> 4 << 16) | (dmem_.wet_left >> 4 << 8) |
                           (dmem_.wet_right >> 4);
        }
    }

    list.commands.push_back({Word0(kOpEnvSetup1, static_cast<uint32_t>(wet * 2), static_cast<uint16_t>(ramp_reverb)),
                             Pair(static_cast<uint16_t>(ramp_left), static_cast<uint16_t>(ramp_right))});
    list.commands.push_back({kOpEnvSetup2 << 24, Pair(current_left, current_right)});
    const uint32_t bits = ((static_cast<uint32_t>(source_reverb) & 0x80) >> 7 << 4) |
                          (uint32_t{state.stereo_effects} << 3) | (uint32_t{state.headset_effects} << 2) |
                          (uint32_t{state.strong_right} << 1) | uint32_t{state.strong_left};
    list.commands.push_back(
        {(kOpEnvMixer << 24) | ((dmem_.temp >> 4) << 16) | ((static_cast<uint32_t>(length) & 0xFF) << 8) | bits,
         destinations});
}

void EadLaterPlayer::BuildDelay(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags,
                                int delay_side)
{
    uint32_t destination = 0;
    int delay = 0;
    int previous = 0;
    if (delay_side == 1)
    {
        destination = dmem_.left;
        delay = state.delay_left;
        previous = voice.previous_delay_left;
        voice.previous_delay_right = 0;
        voice.previous_delay_left = state.delay_left;
    }
    else if (delay_side == 2)
    {
        destination = dmem_.right;
        delay = state.delay_right;
        previous = voice.previous_delay_right;
        voice.previous_delay_right = state.delay_right;
        voice.previous_delay_left = 0;
    }
    else
    {
        return;
    }

    const auto usize = static_cast<uint32_t>(size);
    const auto udelay = static_cast<uint32_t>(delay);
    const auto uprevious = static_cast<uint32_t>(previous);
    if (flags != kFlagInit)
    {
        if (delay != previous)
        {
            const uint32_t pitch = static_cast<uint32_t>(((size << 15) / 2 - 1) / ((size + delay - previous - 2) / 2));
            SetBuffer(list, dmem_.delay_temp, dmem_.temp, usize + udelay - uprevious);
            list.commands.push_back({Word0(kOpResampleNearest, 0, pitch), 0});
        }
        else
        {
            Move(list, dmem_.delay_temp, dmem_.temp, usize);
        }

        if (previous != 0)
        {
            Load(list, voice.buffers.delay_state, dmem_.delay_temp, static_cast<uint32_t>(Align16(previous)));
            Move(list, dmem_.temp, dmem_.delay_temp + uprevious, usize + udelay - uprevious);
        }
        else
        {
            Move(list, dmem_.temp, dmem_.delay_temp, usize + udelay);
        }
    }
    else
    {
        Move(list, dmem_.delay_temp, dmem_.temp, usize);
        if (!majora_ || delay != 0)
        {
            Clear(list, dmem_.delay_temp, udelay);
        }

        Move(list, dmem_.temp, dmem_.delay_temp + udelay, usize);
    }

    if (delay != 0)
    {
        Save(list, dmem_.delay_temp + usize, voice.buffers.delay_state, static_cast<uint32_t>(Align16(delay)));
    }

    list.commands.push_back(
        {Word0(kOpAddMixer, static_cast<uint32_t>(Align64(size)) >> 4, 0x7FFF), Pair(dmem_.delay_temp, destination)});
}

bool EadLaterPlayer::Busy() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return player.enabled; }) ||
           std::ranges::any_of(voices_, [](const Voice& voice) { return voice.state.enabled; });
}

void EadLaterPlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    const RoundTowardZero rounding(stadium_2_);
    frames_++;
    RunSlowLoads();
    for (int left = updates_per_frame_; left > 0; left--)
    {
        // Each update's records take their row, from the frame's first update to its last.
        const int update = updates_per_frame_ - left;
        const std::size_t base = voices_.size() * static_cast<std::size_t>(update);
        for (int player = 0; player < config_.players; player++)
        {
            if (players_[static_cast<std::size_t>(player)].enabled)
            {
                RunPlayer(player);
                PlayerSound(player);
            }
        }

        ProcessVoices(static_cast<int>(base));
        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            Voice& voice = voices_[i];
            if (voice.state.enabled)
            {
                voice.state.needs_init = false;
            }
            else
            {
                update_states_[base + i].enabled = false;
            }

            voice.state.harmonics = 0;
        }

        updates_++;
    }

    uint32_t address = output;
    int remaining = samples;
    current_book_ = 0;
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
        }
        else if (samples_per_update_min_ >= remaining / left)
        {
            length = samples_per_update_min_;
        }
        else
        {
            length = samples_per_update_;
        }

        const int update = updates_per_frame_ - left;
        for (std::size_t i = 0; i < reverbs_.size(); i++)
        {
            if (reverbs_[i].enabled)
            {
                StartReverbItems(memory, length, update, static_cast<int>(i));
            }
        }

        BuildUpdate(list, memory, address, length, update);
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

    if (!random_set_)
    {
        random_ = (random_ + static_cast<uint32_t>(frames_)) * 0x9E3779B1U;
    }
}

} // namespace ntdlib::n64

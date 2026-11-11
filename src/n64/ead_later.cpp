// SPDX-License-Identifier: MIT

// The model of the later EAD engine: the sound files and the players. ead_later_scripts.cpp contains the scripts of the
// players and channels, ead_later_layers.cpp the scripts of the layers, ead_later_voices.cpp the voices, and
// ead_later_synthesis.cpp the audio lists.

#include "ntdlib/n64/ead_later.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/bytes.h"

#include "ead_later_constants.h"
#include "ead_later_tables.h"
#include "ead_lists.h"
#include "ntdlib/n64/rom_data.h"
#include "round_toward_zero.h"

namespace ntdlib::n64
{

namespace
{

using ead_later_constants::kMuteSoften;
using ead_later_constants::kMuteStopNotes;
using ead_later_constants::kOneChannel;
using ead_later_constants::kSurroundStateSize;
using ead_later_constants::kTicksPerBeat;
using ead_later_constants::kTwoChannels;
using ead_lists::Align16;
using ead_lists::WriteBe16;

namespace tables = ead_later_tables;

constexpr int kLayerCountOcarina = 64;
constexpr int kLayerCountMajora = 80;
constexpr int kPoolCount = 1 + EadLaterPlayer::kPlayers + EadLaterPlayer::kPlayers * EadLaterPlayer::kChannelsPerPlayer;

constexpr float kRefreshDeviation = 1.00278F;
constexpr int kRefreshRate = 60;
constexpr int kConsoleClock = 48681812;

float CalculateDecay(float inverse_scaled, float scale)
{
    return (256.0F * inverse_scaled) / scale;
}

// Writes the eight taps of a filter to the memory.
void LoadTaps(AudioMemory& memory, uint32_t address, const std::span<const int16_t> taps)
{
    for (std::size_t i = 0; i < 8; i++)
    {
        WriteBe16(memory, address + static_cast<uint32_t>(i) * 2, taps[i]);
    }
}

// Reads the sequences that the table lists. An entry with no size copies the sequence that its address names.
void ReadSequences(std::span<const uint8_t> image, const LaterSoundData& data, const EadTable& sequences,
                   EadLaterFiles& files)
{
    files.sequences.resize(sequences.entries.size());
    for (std::size_t i = 0; i < sequences.entries.size(); i++)
    {
        const EadTable::Entry& entry = sequences.entries[i];
        if (entry.size != 0)
        {
            const std::span<const uint8_t> bytes = image.subspan(data.sequences + entry.address, entry.size);
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
}

// Returns the place in the image of a table of samples, or 0 for an index outside the table.
uint32_t SamplePlace(const EadTable& samples, std::size_t samples_start, std::size_t index)
{
    if (index >= samples.entries.size())
    {
        return 0;
    }

    if (samples.entries[index].size == 0)
    {
        index = samples.entries[index].address;
    }

    return index < samples.entries.size() ? static_cast<uint32_t>(samples_start) + samples.entries[index].address : 0;
}

// Reads the banks that the table lists and the places of their tables of samples.
void ReadBanks(std::span<const uint8_t> image, const LaterSoundData& data, const EadTable& banks,
               const EadTable& samples, EadLaterFiles& files)
{
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
        files.banks[i] = EadBank::ParseLater(image.subspan(data.banks + entry.address, entry.size), entry.data2 >> 8,
                                             entry.data2 & 0xFF, entry.data3);
        files.sample_places[i] = {SamplePlace(samples, data.samples, entry.data1 >> 8),
                                  SamplePlace(samples, data.samples, entry.data1 & 0xFF)};
    }
}

// Reads the banks of each sequence from the table that starts at `sets_start` in the code.
void ReadBankSets(std::span<const uint8_t> code, std::size_t sets_start, std::size_t sequence_count,
                  EadLaterFiles& files)
{
    files.bank_sets.resize(sequence_count);
    for (std::size_t i = 0; i < sequence_count; i++)
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
    ReadSequences(image, *data, sequences, files);
    ReadBanks(image, *data, banks, samples, files);
    if (data->bank_set_table != 0)
    {
        ReadBankSets(code, data->bank_set_table, sequences.entries.size(), files);
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
    SetRates(config.sampling_frequency);
    SetDecayTable();

    channels_.resize(kPlayers * kChannelsPerPlayer);
    InitLayers();
    InitVoices(config.voices, memory);
    LoadTables(memory);

    reverbs_.resize(config.reverbs.size());
    for (std::size_t i = 0; i < reverbs_.size(); i++)
    {
        InitReverb(reverbs_[i], config.reverbs[i], memory);
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

// Sets the output rate, the number and sizes of a frame's updates, and the tempo limit for a sampling frequency.
void EadLaterPlayer::SetRates(int frequency)
{
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
}

// Fills the table of envelope decays for the number of updates in a frame.
void EadLaterPlayer::SetDecayTable()
{
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
}

// Sets up the layers, all of them free.
void EadLaterPlayer::InitLayers()
{
    const int layer_count = majora_ ? kLayerCountMajora : kLayerCountOcarina;
    layers_.resize(static_cast<std::size_t>(layer_count));
    layer_is_free_.assign(static_cast<std::size_t>(layer_count), 1);
    free_layers_.reserve(static_cast<std::size_t>(layer_count));
    for (int i = 0; i < layer_count; i++)
    {
        free_layers_.push_back(i);
    }
}

// Sets up the voices, the voice lists with every voice disabled, and the buffers of each voice in the memory.
void EadLaterPlayer::InitVoices(int count, AudioMemory& memory)
{
    voices_.resize(static_cast<std::size_t>(count));
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
}

// Copies the tables of waves and of the note book into the memory.
void EadLaterPlayer::LoadTables(AudioMemory& memory)
{
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
}

// Sets up a reverb from its settings and allocates its buffers and filters in the memory.
void EadLaterPlayer::InitReverb(Reverb& reverb, const EadLaterReverb& settings, AudioMemory& memory)
{
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
        LoadTaps(memory, reverb.filter_left,
                 std::span<const int16_t>(tables::kLowPass)
                     .subspan(std::size_t{8} * static_cast<std::size_t>(settings.low_pass_left)));
    }

    if (settings.low_pass_right != 0)
    {
        reverb.filter_right_state = memory.Allocate(32);
        reverb.filter_right = memory.Allocate(16);
        LoadTaps(memory, reverb.filter_right,
                 std::span<const int16_t>(tables::kLowPass)
                     .subspan(std::size_t{8} * static_cast<std::size_t>(settings.low_pass_right)));
    }
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

} // namespace ntdlib::n64

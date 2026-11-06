// SPDX-License-Identifier: MIT

#include "ntdlib/n64/sbn_player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "xsflib/bytes.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/sbn.h"

namespace ntdlib::n64
{

namespace
{

constexpr int kChunk = SbnPlayer::kChunkSamples;
constexpr int kChunkBytes = kChunk * 2;
constexpr int kDecoderOut = 0x170;
constexpr int kDecoderIn = 0;
constexpr int kTemp = 0;
constexpr uint32_t kMainLeft = 0x4E0;
constexpr uint32_t kMainRight = 0x650;
constexpr uint32_t kAuxLeft = 0x7C0;
constexpr uint32_t kAuxRight = 0x930;
constexpr uint32_t kOpLoad = 0x04000000;
constexpr uint32_t kOpSave = 0x06000000;
constexpr uint32_t kOpMix = 0x0C000000;
constexpr uint32_t kOpMove = 0x0A000000;
constexpr uint32_t kAddressMask = 0xFFFFFF;
constexpr uint32_t kDmemMask = 0xFFF;
constexpr int kFrameMicroseconds = 5750;
constexpr int kFxLength = 14 * kChunk;
constexpr float kUnityPitch = 32768.0F;
constexpr float kMaxRatio = 1.99996F;
constexpr int kFrameSamples = 16;
constexpr int kFrameBytes = 9;
constexpr int kUpdateScale = 1000;
constexpr int kTempoScale = 100;
constexpr int kDefaultBpm = 156;
constexpr int32_t kDefaultStep = 156250;
constexpr uint32_t kDefaultTicksPerBeat = 48;
constexpr uint32_t kFramesPerMinute = kUpdateScale * (60 * 32000) / kChunk;
constexpr uint8_t kEnvMax = 0x80;
constexpr uint8_t kEndCommand = 0xFF;

constexpr uint8_t kSyncAll = 1U << 0;
constexpr uint8_t kSyncParams = 1U << 1;
constexpr uint8_t kSyncPitch = 1U << 2;
constexpr uint8_t kSyncPanMix = 1U << 3;
constexpr uint8_t kEnvHandledChange = 1U << 0;
constexpr uint8_t kEnvKeyReleased = 1U << 1;
constexpr uint8_t kEnvReleasing = 1U << 2;
constexpr uint8_t kEnvVolumeChanged = 1U << 3;

constexpr uint8_t kStateIdle = 0;
constexpr uint8_t kStateActive = 1;
constexpr uint8_t kStateFetch = 2;
constexpr uint8_t kStateInit = 3;
constexpr uint8_t kStateStop = 4;

constexpr uint8_t kDelayNone = 0;
constexpr uint8_t kDelayLeft = 1;
constexpr uint8_t kDelayRight = 2;

// The arguments of commands 0xE0 to 0xFF, from the same table of the game's code as the reader's.
constexpr std::array<uint8_t, 32> kArgumentCounts = {2, 1, 1, 1, 4, 3, 2, 0, 2, 1, 1, 1, 1, 1, 1, 2,
                                                     3, 1, 1, 0, 2, 1, 3, 1, 0, 0, 0, 0, 3, 3, 3, 3};

// The volume's gain for each pan, as in the compact driver: cosines from 0 to a quarter turn, at full scale 32767.
constexpr std::array<int16_t, 128> kEqualPower = {
    32767, 32764, 32757, 32744, 32727, 32704, 32677, 32644, 32607, 32564, 32517, 32464, 32407, 32344, 32277, 32205,
    32127, 32045, 31958, 31866, 31770, 31668, 31561, 31450, 31334, 31213, 31087, 30957, 30822, 30682, 30537, 30388,
    30234, 30075, 29912, 29744, 29572, 29395, 29214, 29028, 28838, 28643, 28444, 28241, 28033, 27821, 27605, 27385,
    27160, 26931, 26698, 26461, 26220, 25975, 25726, 25473, 25216, 24956, 24691, 24423, 24151, 23875, 23596, 23313,
    23026, 22736, 22442, 22145, 21845, 21541, 21234, 20924, 20610, 20294, 19974, 19651, 19325, 18997, 18665, 18331,
    17993, 17653, 17310, 16965, 16617, 16266, 15913, 15558, 15200, 14840, 14477, 14113, 13746, 13377, 13006, 12633,
    12258, 11881, 11503, 11122, 10740, 10357, 9971,  9584,  9196,  8806,  8415,  8023,  7630,  7235,  6839,  6442,
    6044,  5646,  5246,  4845,  4444,  4042,  3640,  3237,  2833,  2429,  2025,  1620,  1216,  810,   405,   0};

struct EffectSection
{
    int input;
    int output;
    int feed_back;
    int feed_forward;
    int gain;
    int rate;
    int depth;
    int cutoff;
};

struct EffectPreset
{
    int length;
    std::vector<EffectSection> sections;
};

// The presets of the game's code, in units of chunks.
const EffectPreset& PresetOf(uint8_t type)
{
    static const EffectPreset kNone = {0, {}};
    static const EffectPreset kSmallRoom = {
        11, {{0, 9, 9830, -9830, 0, 0, 0, 0}, {3, 7, 3276, -3276, 0x3FFF, 0, 0, 0}, {0, 10, 5000, 0, 0, 0, 0, 0x5000}}};
    static const EffectPreset kBigRoom = {14,
                                          {{0, 9, 9830, -9830, 0, 0, 0, 0},
                                           {2, 6, 3276, -3276, 0x3FFF, 0, 0, 0},
                                           {9, 12, 3276, -3276, 0x3FFF, 0, 0, 0},
                                           {0, 13, 6000, 0, 0, 0, 0, 0x5000}}};
    static const EffectPreset kEcho = {14, {{0, 13, 20000, 0, 0x7FFF, 0, 0, 0x7FFF}}};
    static const EffectPreset kChorus = {3, {{0, 1, 16384, 0, 0x7FFF, 7600, 700, 0}}};
    static const EffectPreset kFlange = {3, {{0, 1, 0, 0x5FFF, 0x7FFF, 380, 500, 0}}};
    switch (type)
    {
    case 1:
        return kSmallRoom;
    case 2:
        return kBigRoom;
    case 3:
        return kChorus;
    case 4:
        return kFlange;
    case 5:
        return kEcho;
    case 10:
        return kBigRoom;
    default:
        return kNone;
    }
}

int16_t Ramp(int16_t volume, int32_t samples, int16_t rate_high, uint16_t rate_low)
{
    const int32_t steps = samples >> 3;
    if (steps == 0)
    {
        return volume;
    }

    int32_t low = static_cast<int32_t>(rate_low) * steps;
    low >>= 16;
    low += static_cast<int32_t>(rate_high) * steps;

    return static_cast<int16_t>(volume + low);
}

int16_t Rate(double volume, double target, int32_t count, uint16_t& rate_low)
{
    if (count == 0)
    {
        if (target >= volume)
        {
            rate_low = 0xFFFF;

            return 0x7FFF;
        }

        rate_low = 0;

        return 0;
    }

    const double inverse = 1.0 / static_cast<double>(count);
    target = std::max(target, 1.0);
    if (volume <= 0.0)
    {
        volume = 1.0;
    }

    const double step = (target - volume) * inverse * 8.0;
    const auto whole = static_cast<int16_t>(static_cast<int32_t>(step));
    const double above = step - static_cast<double>(whole) + 1.0;
    const auto carry = static_cast<int16_t>(static_cast<int32_t>(above));
    rate_low = static_cast<uint16_t>(static_cast<uint32_t>((above - static_cast<double>(carry)) * 65535.0));

    return static_cast<int16_t>(whole - 1 + carry);
}

int32_t Multiply4(int32_t a, int32_t b, int32_t c, int32_t d)
{
    return (((a * b * c) >> 7 >> 7) * d) >> 7;
}

int32_t GetDelta(int32_t microseconds)
{
    return (microseconds / kFrameMicroseconds) * kChunk;
}

int32_t Volume8To32(uint8_t value)
{
    const int32_t volume = value & 0x7F;

    return volume != 0 ? volume << 24 : 0;
}

uint8_t RandomPan(uint32_t seed, uint8_t pan, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t parity = ((value >> 7) + (value >> 2)) & 1;
    const int32_t random = ((value >> 8) & 0x3F) + ((value << 4) & 0xC0);
    int32_t result = parity != 0 ? pan + ((amplitude * random) >> 8) : pan - ((amplitude * random) >> 8);
    result = std::clamp(result, 0, 0x7F);

    return static_cast<uint8_t>(result);
}

int16_t RandomPitch(uint32_t seed, int32_t pitch, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t parity = ((value >> 4) + (value >> 1)) & 1;
    const int32_t random = ((value >> 6) & 0xF) + ((value << 2) & 0xF0);

    return static_cast<int16_t>(parity != 0 ? pitch + ((amplitude * 5 * random) >> 8)
                                            : pitch - ((amplitude * 5 * random) >> 8));
}

uint8_t RandomVolume(uint32_t seed, uint8_t volume, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t random = ((value >> 8) & 0x1F) + (value & 0xE0);

    return static_cast<uint8_t>(volume * (0x8000 - amplitude * random));
}

uint8_t RandomReverb(uint32_t seed, uint8_t reverb, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t random = ((value >> 7) & 7) + ((value << 3) & 0xF8);

    return static_cast<uint8_t>(reverb * (0x8000 - amplitude * random));
}

} // namespace

SbnPlayer::SbnPlayer(const SbnPlayerConfig& config, const SbnArchive& archive, AudioMemory& memory)
    : config_(config), archive_(&archive)
{
    Wave fallback;
    fallback.key_base = 4800;
    fallback.pitch_ratio = 0.5F;
    waves_.push_back(fallback);
    default_wave_ = 0;
    for (auto& set : bank_sets_)
    {
        for (auto& bank : set)
        {
            bank.fill(default_wave_);
        }
    }

    for (Voice& voice : voices_)
    {
        voice.decoder_state = memory.Allocate(32);
        voice.decoder_loop_state = memory.Allocate(32);
        voice.resampler_state = memory.Allocate(32);
        voice.mixer_state = memory.Allocate(80);
    }

    dry_accumulator_ = memory.Allocate(kChunkBytes * 2);
    wet_accumulator_ = memory.Allocate(kChunkBytes * 2);
    delay_main_ = memory.Allocate(4 * kChunk * 2);
    delay_aux_ = memory.Allocate(4 * kChunk * 2);
    for (Bus& bus : buses_)
    {
        for (Effect& effect : bus.effects)
        {
            effect.base = memory.Allocate(kFxLength * 2);
            for (Delay& delay : effect.delays)
            {
                delay.resampler_state = memory.Allocate(32);
                delay.filter.coefficients_address = memory.Allocate(32);
                delay.filter.state = memory.Allocate(32);
            }
        }
    }

    build_memory_ = &memory;
    const auto& others = archive.OtherFiles();
    if (others.size() > 1)
    {
        const std::vector<BgmDrum> drums = ParsePerFile(archive.File(others[1]));
        for (std::size_t i = 0; i < global_drums_.size() && i < drums.size(); i++)
        {
            global_drums_[i] = drums[i];
        }
    }

    if (others.size() > 2)
    {
        const std::vector<BgmInstrument> instruments = ParsePrgFile(archive.File(others[2]));
        for (std::size_t i = 0; i < global_instruments_.size() && i < instruments.size(); i++)
        {
            global_instruments_[i] = instruments[i];
        }
    }

    for (const SbnArchive::InitBank& bank : archive.InitBanks())
    {
        LoadBank(bank.file, bank.bank, bank.bank_set);
    }

    InitPlayer(players_[0], 1, 0, {0, 3, -1, -1});
    InitPlayer(players_[1], 2, 2, {2, -1, -1, -1});
}

int SbnPlayer::AddWave(const BkFile& file, std::span<const uint8_t> bytes, uint32_t file_place,
                       const BkInstrument& instrument)
{
    (void)file;
    Wave wave;
    wave.data = instrument.wave != 0 ? AudioMemory::kImageBase + file_place + instrument.wave : 0;
    wave.length = instrument.wave_size;
    wave.book = AudioMemory::kImageBase + file_place + instrument.book;
    wave.book_size = instrument.book_size;
    wave.loop_state = instrument.loop_state != 0 ? AudioMemory::kImageBase + file_place + instrument.loop_state : 0;
    wave.loop_start = instrument.loop_start;
    wave.loop_end = instrument.loop_end;
    wave.loop_count = instrument.loop_count;
    wave.key_base = instrument.key_base;
    wave.type = instrument.type;
    wave.pitch_ratio = static_cast<float>(instrument.sample_rate) / static_cast<float>(config_.output_rate);
    wave.file = bytes;
    wave.envelopes = instrument.envelopes;
    waves_.push_back(wave);

    return static_cast<int>(waves_.size()) - 1;
}

void SbnPlayer::LoadBank(uint16_t file, int bank, int bank_set)
{
    const std::span<const uint8_t> bytes = archive_->File(file);
    const BkFile parsed = BkFile::Parse(bytes);
    const uint32_t place = config_.archive_place + archive_->Entries()[file].offset;
    auto& slots = bank_sets_[static_cast<std::size_t>(bank_set)][static_cast<std::size_t>(bank)];
    for (std::size_t i = 0; i < parsed.slots.size(); i++)
    {
        const int32_t index = parsed.slots[i];
        slots[i] = index >= 0 ? AddWave(parsed, bytes, place, parsed.instruments[static_cast<std::size_t>(index)])
                              : default_wave_;
    }
}

int SbnPlayer::WaveOf(int bank, int patch, Envelope& envelope) const
{
    // The bank's high nibble picks the set. The patch holds the set's bank and the bank's instrument.
    const auto set = static_cast<std::size_t>((bank & 0x70) >> 4);
    const auto group = static_cast<std::size_t>((patch >> 4) & 0xF);
    const auto slot = static_cast<std::size_t>(patch & 0xF);
    const int index = bank_sets_[set][group][slot];
    const Wave& wave = waves_[static_cast<std::size_t>(index)];
    const uint32_t which = static_cast<uint32_t>(bank) & 3;
    if (wave.envelopes != 0 && !wave.file.empty() && which < wave.file[wave.envelopes])
    {
        const uint8_t* const entry = wave.file.data() + wave.envelopes + 4 + which * 4;
        envelope.press = {wave.file, -1, 0, wave.envelopes + xsflib::LoadBe16(entry)};
        envelope.release = {wave.file, -1, 0, wave.envelopes + xsflib::LoadBe16(entry + 2)};
    }
    else
    {
        envelope.press = {{}, -1, 0, 0};
        envelope.release = {{}, -1, 0, 4};
    }

    return index;
}

uint8_t SbnPlayer::EnvelopeByte(const EnvelopePlace& place) const
{
    if (place.custom_player >= 0)
    {
        const auto& custom = players_[static_cast<std::size_t>(place.custom_player)]
                                 .custom_envelopes[static_cast<std::size_t>(place.custom)];
        const uint16_t value = custom[place.at / 2];

        return static_cast<uint8_t>((place.at & 1) == 0 ? value >> 8 : value);
    }

    const std::span<const uint8_t> bytes = place.bytes.empty() ? config_.tables.default_envelope : place.bytes;

    return place.at < bytes.size() ? bytes[place.at] : kEndCommand;
}

uint8_t SbnPlayer::ReadEnvelope(EnvelopePlace& place) const
{
    const uint8_t value = EnvelopeByte(place);
    place.at++;

    return value;
}

float SbnPlayer::PitchRatio(int32_t tuning) const
{
    const std::vector<float>& table = config_.tables.tune_scaling;
    if (tuning >= 0)
    {
        return table[static_cast<std::size_t>(tuning & 0x7F)] *
               table[static_cast<std::size_t>(((tuning & 0xF80) >> 7) + 128)];
    }

    tuning = -tuning;

    return table[static_cast<std::size_t>((tuning & 0x7F) + 160)] *
           table[static_cast<std::size_t>(((tuning & 0x3F80) >> 7) + 288)];
}

void SbnPlayer::InitPlayer(Player& player, int priority, int bus, std::array<int8_t, 4> effects)
{
    SetTickResolution(player, kDefaultStep, kDefaultTicksPerBeat);
    player.bus_volume = 0x8000;
    player.master_tempo = kDefaultBpm * kTempoScale;
    player.master_volume = 0x7F << 24;
    player.priority = static_cast<uint8_t>(priority);
    player.bus = static_cast<uint8_t>(bus);
    player.effect_indices = effects;
    player.polyphony_counts = {0, 1, 0, 0, 0, 2, 3, 4};
    player.fade.envelope_volume = 0x7FFF << 16;
    player.fade.envelope_target = 0x7FFF;
    player.state = kStateIdle;
}

void SbnPlayer::SetTickResolution(Player& player, int32_t step, uint32_t ticks_per_beat)
{
    uint32_t frames_per_tick = kFramesPerMinute / ticks_per_beat;
    frames_per_tick = std::clamp<uint32_t>(frames_per_tick, 80000, 500000);
    if (frames_per_tick < static_cast<uint32_t>(step))
    {
        step = static_cast<int32_t>(frames_per_tick);
    }

    player.update_step = step;
    player.tick_interval = static_cast<int32_t>(frames_per_tick);
    player.update_counter = static_cast<int32_t>(frames_per_tick);
    player.max_tempo = frames_per_tick / kUpdateScale;
}

bool SbnPlayer::LoadSong(int player, int song)
{
    const auto& songs = archive_->Songs();
    if (song < 0 || static_cast<std::size_t>(song) >= songs.size())
    {
        return false;
    }

    // The list ends with an entry that names no file.
    const SbnArchive::Song& entry = songs[static_cast<std::size_t>(song)];
    if (entry.bgm >= archive_->Entries().size())
    {
        return false;
    }

    Player& held = players_[static_cast<std::size_t>(player)];
    held.file = archive_->File(entry.bgm);
    held.parsed = std::make_shared<const BgmFile>(BgmFile::Parse(held.file));
    held.song = song;
    for (std::size_t i = 0; i < entry.banks.size(); i++)
    {
        if (entry.banks[i] != 0 && archive_->Entries()[entry.banks[i]].format == kSbnFormatBank)
        {
            LoadBank(entry.banks[i], static_cast<int>(i), 0);
        }
    }

    return true;
}

void SbnPlayer::StartSong(int player, int variation, int duration, int start_volume, int final_volume)
{
    Player& held = players_[static_cast<std::size_t>(player)];
    if (duration != 0)
    {
        duration = std::clamp(duration, 250, 10000);
    }

    start_volume = std::min(start_volume, 0x7F);
    const int first = start_volume != 0 ? (start_volume << 8) | 0xFF : 0;
    final_volume = std::min(final_volume, 0x7F);
    const int last = final_volume != 0 ? (final_volume << 8) | 0xFF : 0x7FFF;
    Fade& fade = held.fade;
    fade.base_volume = first << 16;
    fade.base_target = static_cast<int16_t>(last);
    if (duration != 0)
    {
        fade.base_ticks = static_cast<uint16_t>((duration * 1000) / kFrameMicroseconds);
        fade.base_step = ((last << 16) - fade.base_volume) / fade.base_ticks;
    }
    else
    {
        fade.base_ticks = 1;
        fade.base_step = 0;
    }

    fade.envelope_target = 0x7FFF;
    fade.envelope_ticks = 1;
    SetTickResolution(held, kDefaultStep, config_.tables.ticks_per_beat[held.parsed->timing_preset & 7]);
    if (variation < 0 || variation > 3 || held.parsed->CompositionOffset(variation) == 0)
    {
        variation = 0;
    }

    held.composition_start = held.parsed->CompositionOffset(variation);
    held.composition = held.composition_start;
    held.active = true;
    Initialize(player);
}

void SbnPlayer::Stop(int player)
{
    UpdateStop(player);
}

bool SbnPlayer::Playing(int player) const
{
    return players_[static_cast<std::size_t>(player)].active;
}

bool SbnPlayer::Busy() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return player.active; }) ||
           std::ranges::any_of(voices_, [](const Voice& voice) { return voice.playing; });
}

void SbnPlayer::SetProximityMix(int player, uint32_t mix)
{
    Player& held = players_[static_cast<std::size_t>(player)];
    held.proximity_id = static_cast<int32_t>(mix & 0xFF);
    held.proximity_volume = static_cast<uint8_t>((mix >> 24) & 0xFF);
}

void SbnPlayer::UpdateBusVolumes(Player& player)
{
    const auto volume = static_cast<uint16_t>(((static_cast<uint32_t>(player.fade.base_volume) >> 16) *
                                               (static_cast<uint32_t>(player.fade.envelope_volume) >> 16)) >>
                                              15);
    for (const int8_t bus : player.effect_indices)
    {
        if (bus < 0)
        {
            return;
        }

        SetBusVolume(bus, volume, player.bus_volume);
    }
}

void SbnPlayer::SetBusVolume(int bus, uint16_t volume, int32_t bus_volume)
{
    buses_[static_cast<std::size_t>(bus)].gain =
        static_cast<uint16_t>((static_cast<uint32_t>(volume) * static_cast<uint32_t>(bus_volume)) / 0x8000 & 0x7FFF);
}

void SbnPlayer::UpdateFade(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.fade.base_ticks--;
    if (player.fade.base_ticks != 0)
    {
        player.fade.base_volume += player.fade.base_step;
    }
    else
    {
        player.fade.base_volume = static_cast<int32_t>(player.fade.base_target) << 16;
        if (player.fade.base_volume == 0)
        {
            UpdateStop(index);
        }
    }

    UpdateBusVolumes(player);
}

void SbnPlayer::RunChunkClients()
{
    BeginChunk();
    for (const int index : {1, 0})
    {
        Player& player = players_[static_cast<std::size_t>(index)];
        if (index == 0 && player.fade.envelope_ticks != 0)
        {
            player.fade.envelope_ticks--;
            if (player.fade.envelope_ticks != 0)
            {
                player.fade.envelope_volume += player.fade.envelope_step;
            }
            else
            {
                player.fade.envelope_step = 0;
                player.fade.envelope_volume = static_cast<int32_t>(player.fade.envelope_target) << 16;
            }

            if (player.fade.base_ticks == 0)
            {
                UpdateBusVolumes(player);
            }
            else
            {
                UpdateFade(index);
            }
        }
        else if (player.fade.base_ticks != 0)
        {
            UpdateFade(index);
        }

        if (player.song >= 0)
        {
            player.counter++;
        }

        player.update_counter -= player.update_step;
        if (player.update_counter <= 0)
        {
            player.update_counter += player.tick_interval;
            RunPlayer(index);
        }
    }

    UpdateVoices();
}

void SbnPlayer::BeginChunk()
{
    if (request_delay_pending_)
    {
        if (request_delay_side_ == kDelayLeft || request_delay_side_ == kDelayRight)
        {
            delay_count_ = std::min<int32_t>(request_delay_time_, 4);
            if (request_delay_time_ < 2)
            {
                delayed_channel_ = kDelayNone;
            }

            std::vector<uint8_t> zeros(4 * kChunk * 2, 0);
            build_memory_->Write(delay_main_, zeros.data(), zeros.size());
            build_memory_->Write(delay_aux_, zeros.data(), zeros.size());
            delayed_bus_ = request_delay_bus_;
            delayed_channel_ = request_delay_side_;
        }
        else
        {
            delayed_bus_ = 0;
            delayed_channel_ = kDelayNone;
        }

        delay_counter_ = 0;
        request_delay_pending_ = false;
    }

    for (const int bus : {0, 1, 2, 3})
    {
        EffectChange& change = effect_changes_[static_cast<std::size_t>(bus)];
        if (change.changed)
        {
            Bus& held = buses_[static_cast<std::size_t>(bus)];
            held.effect_type = change.type;
            LoadEffect(held, held.effects[0], change.type);
            LoadEffect(held, held.effects[1], change.type);
            change.changed = false;
        }
    }

    for (int i = 0; i < kVoices; i++)
    {
        Voice& voice = voices_[static_cast<std::size_t>(i)];
        const uint8_t flags = voice.sync_flags;
        if (voice.done_pending)
        {
            SynthStopVoice(i);
            voice.done_pending = false;
            voice.has_commands = false;
            voice.priority = 0;
        }

        if ((flags & kSyncAll) != 0)
        {
            StartEnvelope(voice);
            SynthStartVoice(i);
            voice.priority = voice.client_priority;
        }
        else
        {
            if ((flags & kSyncPitch) != 0)
            {
                voice.ratio = voice.pitch_ratio;
            }

            if ((flags & kSyncParams) != 0)
            {
                SynthSetMixer(i, voice.volume, voice.delta, voice.pan, voice.reverb);
            }
            else if ((flags & kSyncPanMix) != 0)
            {
                SynthSetPanAndMix(i, voice.pan, voice.reverb);
            }
        }

        voice.sync_flags = 0;
    }
}

void SbnPlayer::RunPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.random_1 = (player.random_1 & 0xFFFF) + (player.counter & 0xFFFF) + (player.frames & 0xFFFF);
    player.random_2 = (player.random_2 & 0xFFFF) + ((player.counter << 4) & 0xFFFF) + ((player.frames >> 4) & 0xFFFF);
    bool more = true;
    while (more)
    {
        switch (player.state)
        {
        case kStateActive:
            if (!player.paused)
            {
                UpdatePlaying(index);
                more = player.state != kStateActive;
            }
            else
            {
                more = false;
            }

            break;
        case kStateFetch:
            ReadComposition(player);
            break;
        case kStateInit:
            Initialize(index);
            more = false;
            break;
        case kStateStop:
            UpdateStop(index);
            break;
        default:
            more = false;
            break;
        }
    }
}

void SbnPlayer::Initialize(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kTracks; i++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(i)];
        const uint8_t keep_first = track.first_voice;
        const uint8_t keep_last = track.last_voice;
        track = Track{};
        track.first_voice = keep_first;
        track.last_voice = keep_last;
        track.instrument_volume = 0x7FFF << 16;
        track.proximity_volume = 0x7FFF << 16;
        track.bus = player.bus;
    }

    for (Note& note : player.notes)
    {
        note = Note{};
    }

    ResetAllVoices(player);
    player.master_tempo = kDefaultBpm * kTempoScale;
    player.master_tempo_bpm = kDefaultBpm;
    player.master_volume = 0x7F << 24;
    player.tempo_target = 0;
    player.pitch_shift = 0;
    player.detune = 0;
    player.volume_ticks = 0;
    player.volume_target = 0;
    player.volume_step = 0;
    player.proximity_value = 0;
    player.proximity_id = 0;
    player.proximity_volume = 0;
    player.loop_ends = {};
    player.loop_counters = {};
    player.loop_depth = 0;
    player.condition_flags = 0;
    player.first_link_mute = true;
    player.custom_writing = 0;
    player.playback_rate = 1.0F;
    for (std::size_t i = 0; i < player.custom_envelopes.size(); i++)
    {
        player.custom_envelopes[i].fill(0xFF00);
        player.custom_write[i] = 0;
    }

    player.effect_values = {};
    player.paused = false;
    player.counter = 0;
    player.loop_labels.fill(player.composition);
    for (uint32_t at = player.composition; at + 4 <= player.file.size(); at += 4)
    {
        const uint32_t command = xsflib::LoadBe32(player.file.data() + at);
        if (command == 0)
        {
            break;
        }

        if ((command >> 28) == 3)
        {
            player.loop_labels[command & 0x1F] = at + 4;
        }
    }

    player.state = kStateFetch;
}

void SbnPlayer::ReadComposition(Player& player)
{
    player.tempo_step = 0;
    player.tempo_ticks = 0;
    bool reading = true;
    while (reading)
    {
        const uint32_t command = xsflib::LoadBe32(player.file.data() + player.composition);
        player.composition += 4;
        if (command == 0)
        {
            player.state = kStateStop;
            break;
        }

        switch (command >> 28)
        {
        case 1:
            LoadPhrase(player, command);
            player.state = kStateActive;
            reading = false;
            break;
        case 3:
            break;
        case 5:
            EndCompositionLoop(player, command);
            break;
        case 6:
            if ((player.condition_flags & 1) == 0)
            {
                EndCompositionLoop(player, command);
            }

            break;
        case 7:
            if ((player.condition_flags & 1) != 0)
            {
                EndCompositionLoop(player, command);
            }

            break;
        default:
            reading = false;
            break;
        }
    }
}

void SbnPlayer::EndCompositionLoop(Player& player, uint32_t command)
{
    const uint32_t label = command & 0x1F;
    const auto count = static_cast<uint8_t>((command >> 5) & 0x7F);
    uint32_t depth = player.loop_depth;
    if (player.loop_ends[depth] != 0)
    {
        if (player.loop_ends[depth] == player.composition)
        {
            if (player.loop_counters[depth] != 0)
            {
                player.loop_counters[depth]--;
                if (player.loop_counters[depth] == 0)
                {
                    player.loop_ends[depth] = 0;
                    if (depth > 0)
                    {
                        depth--;
                    }
                }
                else
                {
                    player.composition = player.loop_labels[label];
                }
            }
            else
            {
                player.composition = player.loop_labels[label];
            }
        }
        else if (depth < 3)
        {
            depth++;
            player.loop_ends[depth] = player.composition;
            player.loop_counters[depth] = count;
            player.composition = player.loop_labels[label];
        }
    }
    else
    {
        player.loop_ends[depth] = player.composition;
        player.loop_counters[depth] = count;
        player.composition = player.loop_labels[label];
    }

    player.loop_depth = depth;
}

void SbnPlayer::LoadPhrase(Player& player, uint32_t command)
{
    uint8_t next_voice = 0;
    bool found_link = false;
    player.phrase = player.composition_start + ((command & 0xFFFF) << 2);
    for (int i = 0; i < kTracks; i++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(i)];
        const uint32_t info = xsflib::LoadBe32(player.file.data() + player.phrase + static_cast<uint32_t>(i) * 4);
        const uint32_t offset = info >> 16;
        track.position = offset;
        if (offset == 0)
        {
            continue;
        }

        if ((info & 0x100) != 0)
        {
            track.position = 0;
            continue;
        }

        track.polyphony = static_cast<uint8_t>((info >> 13) & 7);
        track.drums = ((info >> 7) & 1) != 0;
        const auto linked = static_cast<uint8_t>((info >> 9) & 0xF);
        track.linked_track = 0;
        if (linked != 0)
        {
            if (linked - 1 < i)
            {
                const Track& other = player.tracks[static_cast<std::size_t>(linked - 1)];
                track.voice_count = other.voice_count;
                track.first_voice = other.first_voice;
                track.last_voice = other.last_voice;
                track.position = offset + player.phrase;
                track.delay = 1;
                track.linked_track = linked;
                if (player.first_link_mute)
                {
                    track.muted = true;
                }

                found_link = true;
            }
            else
            {
                track.position = 0;
            }
        }
        else
        {
            const uint8_t count = player.polyphony_counts[track.polyphony];
            track.voice_count = count;
            track.first_voice = next_voice;
            next_voice = static_cast<uint8_t>(next_voice + count);
            track.last_voice = next_voice;
            track.position = offset + player.phrase;
            track.delay = 1;
        }
    }

    if (found_link)
    {
        player.first_link_mute = false;
    }
}

void SbnPlayer::UpdateStop(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.paused = false;
    player.song = -1;
    player.active = false;
    for (Track& track : player.tracks)
    {
        track.position = 0;
    }

    ResetAllVoices(player);
    player.state = kStateIdle;
    player.update_step = kDefaultStep;
}

uint8_t SbnPlayer::ReadTrackByte(Player& player, Track& track)
{
    const uint8_t value = track.position < player.file.size() ? player.file[track.position] : uint8_t{0};
    track.position++;
    if (track.detour_length != 0)
    {
        track.detour_length--;
        if (track.detour_length == 0)
        {
            track.position = track.saved_position;
        }
    }

    return value;
}

void SbnPlayer::UpdatePlaying(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    bool fading = false;
    bool finished = false;
    if (player.tempo_ticks != 0)
    {
        player.tempo_ticks--;
        if (player.tempo_ticks == 0)
        {
            player.master_tempo = player.tempo_target;
            player.tempo_target = 0;
            player.tempo_step = 0;
        }
        else
        {
            player.master_tempo += player.tempo_step;
        }

        player.update_step =
            static_cast<int32_t>(kUpdateScale * static_cast<uint32_t>(player.master_tempo) / kTempoScale);
    }

    if (player.volume_ticks != 0)
    {
        player.volume_ticks--;
        if (player.volume_ticks == 0)
        {
            player.master_volume = player.volume_target;
            player.volume_target = 0;
            player.volume_step = 0;
        }
        else
        {
            player.master_volume += player.volume_step;
        }

        fading = true;
    }

    player.volume_changed = false;
    for (int t = 0; t < kTracks; t++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(t)];
        if (track.position == 0)
        {
            continue;
        }

        track.changed_pan = false;
        track.changed_reverb = false;
        track.changed_tune = false;
        track.changed_volume = fading || player.volume_changed;
        if (track.volume_ticks != 0)
        {
            track.volume_ticks--;
            track.instrument_volume =
                track.volume_ticks == 0 ? track.volume_target : track.instrument_volume + track.volume_step;
            track.changed_volume = true;
        }

        if (track.proximity_ticks != 0)
        {
            track.proximity_ticks--;
            track.proximity_volume = track.proximity_ticks == 0 ? track.proximity_target << 16
                                                                : track.proximity_volume + track.proximity_step;
            track.changed_volume = true;
        }

        track.delay--;
        if (track.delay <= 0)
        {
            uint8_t search_from = track.first_voice;
            while (track.delay == 0)
            {
                const uint32_t command_place = track.position;
                const uint8_t opcode = ReadTrackByte(player, track);
                if (opcode < 0x80)
                {
                    if (opcode == 0)
                    {
                        if (track.previous_position != 0)
                        {
                            track.position = track.previous_position;
                            track.previous_position = 0;
                        }
                        else
                        {
                            finished = true;
                            break;
                        }
                    }
                    else if (opcode >= 0x78)
                    {
                        track.delay = ((opcode & 7) << 8) + ReadTrackByte(player, track) + 0x78;
                    }
                    else
                    {
                        track.delay = opcode;
                    }
                }
                else if (opcode >= 0xD4)
                {
                    RunCommand(index, track, opcode);
                }
                else
                {
                    const int32_t key = opcode & 0x7F;
                    const uint8_t velocity = ReadTrackByte(player, track);
                    int32_t length = ReadTrackByte(player, track);
                    if (length >= 0xC0)
                    {
                        length = ((length & 0x3F) << 8) + ReadTrackByte(player, track) + 0xC0;
                    }

                    bool acquired = false;
                    uint8_t voice_index = 0;
                    if (!track.muted)
                    {
                        for (voice_index = search_from; voice_index < track.last_voice; voice_index++)
                        {
                            search_from++;
                            if (voices_[voice_index].priority == 0)
                            {
                                acquired = true;
                                break;
                            }
                        }

                        if (!acquired)
                        {
                            if (track.polyphony >= 5)
                            {
                                for (voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
                                {
                                    if (voices_[voice_index].priority < player.priority)
                                    {
                                        ResetVoice(voice_index);
                                        acquired = true;
                                        break;
                                    }
                                }

                                if (!acquired)
                                {
                                    for (voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
                                    {
                                        if (voices_[voice_index].priority == player.priority &&
                                            player.notes[voice_index].length == 0)
                                        {
                                            ResetVoice(voice_index);
                                            acquired = true;
                                            break;
                                        }
                                    }
                                }

                                if (!acquired)
                                {
                                    int32_t shortest = 0xFFFF;
                                    for (uint8_t other = track.first_voice; other < track.last_voice; other++)
                                    {
                                        const Note& held = player.notes[other];
                                        if (voices_[other].priority == player.priority && !held.pending_tick &&
                                            held.length < shortest)
                                        {
                                            shortest = held.length;
                                            voice_index = other;
                                            acquired = true;
                                        }
                                    }

                                    if (acquired)
                                    {
                                        player.notes[voice_index].length = 0;
                                        ResetVoice(voice_index);
                                    }
                                }
                            }
                            else
                            {
                                voice_index = track.first_voice;
                                player.notes[voice_index].length = 0;
                                if (voices_[voice_index].priority <= player.priority)
                                {
                                    ResetVoice(voice_index);
                                    acquired = true;
                                }
                            }
                        }
                    }

                    if (acquired)
                    {
                        Voice& voice = voices_[voice_index];
                        Note& note = player.notes[voice_index];
                        note.tremolo_depth = 0;
                        note.velocity = velocity > 0 ? velocity + 1 : 0;
                        note.length = length;
                        const int32_t level = (((player.master_volume >> 21) * (track.instrument_volume >> 21) *
                                                (track.proximity_volume >> 21)) >>
                                               20);
                        if (track.drums)
                        {
                            const BgmDrum* drum = nullptr;
                            if (key < 72)
                            {
                                drum = &global_drums_[static_cast<std::size_t>(key)];
                            }
                            else if (!player.parsed->drums.empty())
                            {
                                const auto number = static_cast<std::size_t>(key - 72);
                                drum = number < player.parsed->drums.size() ? &player.parsed->drums[number]
                                                                            : &player.parsed->drums[0];
                            }
                            else
                            {
                                drum = &global_drums_[0];
                            }

                            note.wave = WaveOf(drum->bank_patch >> 8, drum->bank_patch & 0xFF, voice.envelope);
                            const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
                            note.volume =
                                drum->random_volume != 0
                                    ? note.velocity * RandomVolume(player.random_1, drum->volume, drum->random_volume)
                                    : note.velocity * drum->volume;
                            voice.client_volume = static_cast<int16_t>((level * (track.volume * note.volume)) >> 16);
                            note.detune = drum->key_base + track.coarse_detune + track.fine_detune - wave.key_base;
                            int32_t tune = note.detune + track.detune + player.detune;
                            if (drum->random_tune != 0)
                            {
                                note.rand_detune = RandomPitch(player.random_1, tune, drum->random_tune);
                                tune = note.rand_detune;
                            }

                            note.pitch_ratio = PitchRatio(tune) * wave.pitch_ratio;
                            voice.pan =
                                drum->random_pan != 0
                                    ? RandomPan(player.random_1, static_cast<uint8_t>(drum->pan), drum->random_pan)
                                    : static_cast<uint8_t>(drum->pan);
                            voice.reverb = drum->random_reverb != 0
                                               ? RandomReverb(player.random_1, drum->reverb, drum->random_reverb)
                                               : drum->reverb;
                        }
                        else
                        {
                            note.volume = (level * (track.volume * note.velocity)) >> 9;
                            voice.client_volume = static_cast<int16_t>(note.volume);
                            note.wave = track.wave >= 0 ? track.wave : default_wave_;
                            const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
                            note.detune = key * 100 + track.coarse_detune + player.pitch_shift + track.fine_detune -
                                          wave.key_base;
                            note.pitch_ratio =
                                PitchRatio(note.detune + track.detune + player.detune) * wave.pitch_ratio;
                            voice.pan = track.random_pan != 0 ? RandomPan(player.random_1, track.pan, track.random_pan)
                                                              : track.pan;
                            voice.reverb = track.reverb;
                            if (track.press_override != 0)
                            {
                                voice.envelope.press = {{}, index, track.press_override - 1, 0};
                            }
                            else
                            {
                                voice.envelope.press = track.envelope.press;
                            }

                            voice.envelope.release = track.envelope.release;
                        }

                        voice.wave = note.wave;
                        voice.pitch_ratio = note.pitch_ratio;
                        voice.bus = track.bus;
                        voice.track = t;
                        if (note.length > 1)
                        {
                            note.pending_tick = true;
                            note.tremolo_depth = track.tremolo_depth;
                            note.tremolo_phase = 0;
                            note.tremolo_delay = track.tremolo_delay;
                            voice.sync_flags = kSyncAll;
                            voice.priority = player.priority;
                            voice.client_priority = voice.priority;
                        }

                        voice.host_track = index * kTracks + t;
                        if (on_note)
                        {
                            on_note({index, t, static_cast<uint8_t>(key), velocity, length, track.drums, command_place,
                                     voice_index});
                        }
                    }
                }
            }
        }

        for (uint8_t voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
        {
            if (track.muted)
            {
                continue;
            }

            Voice& voice = voices_[voice_index];
            if (voice.priority != player.priority)
            {
                continue;
            }

            Note& note = player.notes[voice_index];
            if (!note.pending_tick)
            {
                if (note.length > 0)
                {
                    note.length--;
                    if (note.length == 0)
                    {
                        voice.envelope_flags |= kEnvKeyReleased;
                    }
                }

                const int32_t level = (((player.master_volume >> 21) * (track.instrument_volume >> 21) *
                                        (track.proximity_volume >> 21)) >>
                                       20);
                const auto new_pitch = [&](int32_t tuning)
                {
                    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
                    note.pitch_ratio = PitchRatio(tuning) * wave.pitch_ratio;
                    if (voice.pitch_ratio != note.pitch_ratio)
                    {
                        voice.pitch_ratio = note.pitch_ratio;
                        voice.sync_flags |= kSyncPitch;
                    }
                };

                if (track.drums)
                {
                    if (track.changed_tune || player.detune != 0)
                    {
                        new_pitch((note.detune + note.rand_detune) + track.detune + player.detune);
                    }

                    if (track.changed_volume)
                    {
                        voice.client_volume = static_cast<int16_t>((level * (track.volume * note.volume)) >> 16);
                        voice.envelope_flags |= kEnvVolumeChanged;
                    }
                }
                else
                {
                    if (note.tremolo_depth != 0)
                    {
                        if (note.tremolo_delay != 0)
                        {
                            note.tremolo_delay--;
                        }
                        else
                        {
                            note.tremolo_phase = static_cast<uint8_t>(note.tremolo_phase + track.tremolo_rate);
                            const auto shape = static_cast<uint8_t>((note.tremolo_phase << 2) + 3);
                            const uint8_t quadrant = note.tremolo_phase >> 6;
                            int32_t detune =
                                (quadrant == 1 || quadrant == 3) ? static_cast<uint8_t>(~shape) + 1 : shape;
                            detune = (detune * track.tremolo_depth) >> 8;
                            if (quadrant >= 2)
                            {
                                detune = -detune;
                            }

                            new_pitch(detune + note.detune + track.detune + player.detune);
                        }
                    }
                    else if (track.changed_tune || player.detune != 0)
                    {
                        new_pitch(note.detune + track.detune + player.detune);
                    }

                    if (track.changed_volume)
                    {
                        note.volume = (level * (track.volume * note.velocity)) >> 9;
                        voice.client_volume = static_cast<int16_t>(note.volume);
                        voice.envelope_flags |= kEnvVolumeChanged;
                        voice.pan = track.pan;
                        voice.reverb = track.reverb;
                    }
                    else if (track.changed_pan || track.changed_reverb)
                    {
                        voice.pan = track.pan;
                        voice.reverb = track.reverb;
                        voice.sync_flags |= kSyncPanMix;
                    }
                }
            }

            note.pending_tick = false;
        }
    }

    if (finished)
    {
        player.state = kStateFetch;
    }
}

void SbnPlayer::RunCommand(int index, Track& track, uint8_t opcode)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (opcode < 0xE0)
    {
        return;
    }

    const uint8_t count = kArgumentCounts[static_cast<std::size_t>(opcode - 0xE0)];
    std::array<uint8_t, 4>& args = player.args;
    for (uint8_t i = 0; i < count; i++)
    {
        args[i] = ReadTrackByte(player, track);
    }

    const auto word = [&](std::size_t at)
    {
        return static_cast<uint16_t>((args[at] << 8) | args[at + 1]);
    };
    const auto to_tempo = [&](uint32_t bpm)
    {
        auto value = static_cast<uint32_t>(static_cast<float>(bpm) * player.playback_rate);
        if (player.max_tempo < value)
        {
            value = player.max_tempo;
        }
        else if (value == 0)
        {
            value = 1;
        }

        return static_cast<int32_t>(value * kTempoScale);
    };

    switch (opcode)
    {
    case 0xE0:
        player.master_tempo_bpm = word(0);
        player.master_tempo = to_tempo(word(0));
        player.update_step =
            static_cast<int32_t>(kUpdateScale * static_cast<uint32_t>(player.master_tempo) / kTempoScale);
        player.tempo_ticks = 0;
        player.tempo_target = 0;
        player.tempo_step = 0;
        break;
    case 0xE1:
        player.master_volume = Volume8To32(args[0]);
        player.volume_ticks = 0;
        player.volume_target = 0;
        player.volume_step = 0;
        player.volume_changed = true;
        track.changed_volume = true;
        break;
    case 0xE2:
        player.pitch_shift = static_cast<int16_t>(static_cast<int8_t>(args[0]) * 100);
        break;
    case 0xE3:
        effect_changes_[player.bus].type = args[0];
        effect_changes_[player.bus].changed = true;
        break;
    case 0xE4:
        {
            const int32_t time = word(0) == 0 ? 1 : word(0);
            const int32_t tempo = to_tempo(word(2));
            player.tempo_ticks = time;
            player.tempo_target = tempo;
            player.tempo_step = (tempo - player.master_tempo) / time;
            break;
        }
    case 0xE5:
        {
            const int32_t time = word(0) == 0 ? 1 : word(0);
            const int32_t volume = Volume8To32(args[2]);
            player.volume_ticks = time;
            player.volume_target = volume;
            player.volume_step = (volume - player.master_volume) / time;
            break;
        }
    case 0xE6:
        {
            const uint8_t which = args[0];
            if (which < 4 && player.effect_indices[which] >= 0)
            {
                const auto bus = static_cast<std::size_t>(player.effect_indices[which]);
                if (effect_changes_[bus].type != args[1])
                {
                    effect_changes_[bus].type = args[1];
                    effect_changes_[bus].changed = true;
                }

                player.effect_values[which] = args[1];
            }

            break;
        }
    case 0xE8:
        track.patch = args[1];
        track.wave = WaveOf(args[0], track.patch, track.envelope);
        break;
    case 0xE9:
        track.instrument_volume = Volume8To32(args[0]);
        track.changed_volume = true;
        break;
    case 0xEA:
        track.pan = args[0] & 0x7F;
        track.random_pan = 0;
        track.changed_pan = true;
        break;
    case 0xEB:
        track.reverb = args[0] & 0x7F;
        track.changed_reverb = true;
        break;
    case 0xEC:
        track.volume = args[0] & 0x7F;
        track.changed_volume = true;
        break;
    case 0xED:
        track.coarse_detune = static_cast<int16_t>(static_cast<int8_t>(args[0]) * 100);
        break;
    case 0xEE:
        track.fine_detune = static_cast<int8_t>(args[0]);
        break;
    case 0xEF:
        track.detune = static_cast<int16_t>(word(0));
        track.changed_tune = true;
        break;
    case 0xF0:
        track.tremolo_delay = args[0];
        track.tremolo_rate = args[1];
        track.tremolo_depth = args[2];
        break;
    case 0xF1:
        track.tremolo_rate = args[0];
        break;
    case 0xF2:
        track.tremolo_depth = args[0];
        break;
    case 0xF3:
        track.tremolo_depth = 0;
        break;
    case 0xF4:
        track.pan = args[0] & 0x7F;
        track.random_pan = args[1] & 0x7F;
        break;
    case 0xF5:
        {
            BgmInstrument instrument;
            uint32_t which = args[0];
            if (which < 0x80)
            {
                if (which < player.parsed->instruments.size())
                {
                    instrument = player.parsed->instruments[which];
                }
            }
            else
            {
                which -= 0x80;
                if (which < global_instruments_.size())
                {
                    instrument = global_instruments_[which];
                }
            }

            track.patch = static_cast<uint8_t>(instrument.bank_patch);
            track.wave = WaveOf(instrument.bank_patch >> 8, track.patch, track.envelope);
            track.instrument_volume = Volume8To32(instrument.volume);
            track.pan = static_cast<uint8_t>(instrument.pan) & 0x7F;
            track.reverb = instrument.reverb & 0x7F;
            track.coarse_detune = static_cast<int16_t>(instrument.coarse_tune * 100);
            track.fine_detune = instrument.fine_tune;
            track.changed_volume = true;
            track.changed_pan = true;
            track.changed_reverb = true;
            break;
        }
    case 0xF6:
        {
            int32_t time = word(0);
            const int32_t volume = Volume8To32(args[2]);
            if (volume != track.instrument_volume)
            {
                time = time <= 0 ? 1 : time;
                track.volume_ticks = time;
                track.volume_target = volume;
                track.volume_step = (volume - track.instrument_volume) / time;
            }

            break;
        }
    case 0xF7:
        {
            const uint8_t which = args[0];
            track.bus = which < 4 && player.effect_indices[which] >= 0
                            ? static_cast<uint8_t>(player.effect_indices[which])
                            : player.bus;
            break;
        }
    case 0xFC:
        {
            uint32_t table = word(0);
            if (static_cast<uint32_t>(player.proximity_id) < args[2])
            {
                table += static_cast<uint32_t>(player.proximity_id) * 3;
            }

            track.previous_position = track.position;
            track.position = static_cast<uint32_t>((player.file[table] << 8) + player.file[table + 1]);
            track.drums = player.file[table + 2] != 0;
            if (track.proximity_set_changed)
            {
                track.proximity_set_changed = false;
                track.proximity_volume = 0;
                for (uint8_t i = track.first_voice; i < track.last_voice; i++)
                {
                    if (voices_[i].priority == player.priority && voices_[i].has_commands)
                    {
                        ResetVoice(i);
                    }
                }
            }

            track.coarse_detune = 0;
            track.fine_detune = 0;
            track.press_override = 0;
            track.detune = 0;
            track.tremolo_depth = 0;
            track.volume_ticks = 0;
            track.random_pan = 0;
            track.bus = player.bus;
            break;
        }
    case 0xFE:
        track.detour_length = args[2];
        track.saved_position = track.position;
        track.position = word(0);
        break;
    case 0xFF:
        Special(index, track);
        break;
    default:
        break;
    }
}

void SbnPlayer::Special(int index, Track& track)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    const uint8_t type = player.args[0];
    const uint8_t first = player.args[1];
    const uint8_t second = player.args[2];
    switch (type)
    {
    case 1:
        if (first < 4 && player.effect_indices[first] >= 0)
        {
            request_delay_bus_ = static_cast<uint8_t>(player.effect_indices[first]);
            if (second != 0)
            {
                const uint8_t time = second & 0xF;
                const auto side = static_cast<uint8_t>(((second >> 4) & 1) + 1);
                if (request_delay_time_ != time || request_delay_side_ != side)
                {
                    request_delay_time_ = time;
                    request_delay_side_ = side;
                    request_delay_pending_ = true;
                }
            }
            else if (request_delay_side_ != kDelayNone)
            {
                request_delay_side_ = kDelayNone;
                request_delay_pending_ = true;
            }
        }

        break;
    case 2:
        if (first >= 1 && first <= player.custom_envelopes.size())
        {
            player.custom_writing = first;
            player.custom_envelopes[first - 1U].fill(0xFF00);
            player.custom_write[first - 1U] = 0;
        }
        else
        {
            player.custom_writing = 0;
        }

        break;
    case 3:
        {
            const uint8_t writing = player.custom_writing;
            if (writing >= 1 && writing <= player.custom_envelopes.size())
            {
                auto& held = player.custom_envelopes[writing - 1U];
                const uint8_t place = player.custom_write[writing - 1U];
                if (place < held.size() - 1)
                {
                    const uint8_t time = first >= config_.tables.custom_envelope_times.size()
                                             ? first
                                             : config_.tables.custom_envelope_times[first];
                    held[place] = static_cast<uint16_t>((time << 8) + second);
                    player.custom_write[writing - 1U] = static_cast<uint8_t>(place + 1);
                }
            }

            break;
        }
    case 4:
        track.press_override = first <= player.custom_envelopes.size() ? first : uint8_t{0};
        break;
    case 6:
        if (first != 0)
        {
            track.proximity_1 = first;
            track.proximity_2 = second;
        }

        break;
    default:
        break;
    }
}

void SbnPlayer::ResetVoice(int voice)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    held.has_commands = false;
    held.done_pending = true;
    held.sync_flags = 0;
    SynthSetVolumeDelta(voice, 0, kChunk);
}

void SbnPlayer::ResetAllVoices(const Player& player)
{
    for (int i = 0; i < kVoices; i++)
    {
        if (voices_[static_cast<std::size_t>(i)].priority == player.priority)
        {
            ResetVoice(i);
        }
    }
}

int32_t SbnPlayer::IntervalTime(uint8_t interval) const
{
    const std::vector<int32_t>& times = config_.tables.envelope_times;

    return interval < times.size() ? times[interval] : 0;
}

uint8_t SbnPlayer::StepEnvelope(Voice& voice)
{
    while (true)
    {
        const uint8_t op = ReadEnvelope(voice.commands);
        if (op < 0x80)
        {
            return op;
        }

        const uint8_t argument = ReadEnvelope(voice.commands);
        switch (op)
        {
        case 0xFE:
            voice.scale = std::min<int32_t>(argument, kEnvMax);
            break;
        case 0xFD:
            voice.scale = std::clamp<int32_t>(voice.scale + static_cast<int8_t>(argument), 0, kEnvMax);
            break;
        case 0xFC:
            voice.loop_counter = argument;
            voice.loop_start = voice.commands;
            break;
        case 0xFB:
            if (voice.loop_counter == 0 || --voice.loop_counter != 0)
            {
                voice.commands = voice.loop_start;
            }

            break;
        default:
            break;
        }
    }
}

void SbnPlayer::StartEnvelope(Voice& voice)
{
    voice.commands = voice.envelope.press;
    voice.has_commands = true;
    voice.scale = kEnvMax;
    const uint8_t interval = StepEnvelope(voice);
    voice.envelope_flags = 0;
    voice.initial = 0;
    voice.target = ReadEnvelope(voice.commands);
    voice.interval = interval;
    voice.duration = IntervalTime(interval);
    voice.time_left = voice.duration;
    voice.volume = static_cast<int16_t>((voice.target * voice.client_volume * voice.scale) >> 7 >> 7);
    voice.delta = GetDelta(voice.duration);
    voice.envelope_delta = voice.duration != 0
                               ? (static_cast<float>(voice.target) - static_cast<float>(voice.initial)) /
                                     static_cast<float>(voice.duration)
                               : 0.0F;
    voice.relative_release = false;
    voice.relative_start = kEnvMax;
}

void SbnPlayer::AfterVolumeChange(Voice& voice)
{
    voice.volume =
        static_cast<int16_t>(Multiply4(voice.target, voice.client_volume, voice.relative_start, voice.scale));
    voice.delta = GetDelta(voice.time_left);
    voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvHandledChange);
    voice.sync_flags |= kSyncParams;
}

void SbnPlayer::UpdateVoices()
{
    const auto interpolated = [](const Voice& voice)
    {
        return voice.initial +
               static_cast<int32_t>(voice.envelope_delta * static_cast<float>(voice.duration - voice.time_left));
    };
    const auto apply_change = [&](Voice& voice, int32_t& current)
    {
        voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvVolumeChanged);
        if (voice.time_left > kFrameMicroseconds)
        {
            voice.time_left -= kFrameMicroseconds;
            voice.envelope_flags |= kEnvHandledChange;
            current = interpolated(voice);
        }
        else
        {
            current = voice.target;
        }

        voice.delta = kChunk;
    };
    const auto set_volume = [](Voice& voice, int32_t current)
    {
        voice.volume = static_cast<int16_t>(Multiply4(current, voice.client_volume, voice.relative_start, voice.scale));
        voice.sync_flags |= kSyncParams;
    };

    for (Voice& voice : voices_)
    {
        if (!voice.has_commands)
        {
            continue;
        }

        if ((voice.envelope_flags & kEnvHandledChange) != 0)
        {
            AfterVolumeChange(voice);
            continue;
        }

        int32_t current = 0;
        if ((voice.envelope_flags & kEnvKeyReleased) != 0)
        {
            voice.envelope_flags = static_cast<uint8_t>((voice.envelope_flags & ~kEnvKeyReleased) | kEnvReleasing);
            voice.commands = voice.envelope.release;
            voice.initial = voice.time_left > kFrameMicroseconds ? interpolated(voice) : voice.target;
            voice.interval = ReadEnvelope(voice.commands);
            uint8_t value = ReadEnvelope(voice.commands);
            if ((value & 0x80) != 0)
            {
                value &= 0x7F;
                voice.relative_release = true;
                voice.relative_start = voice.initial;
            }

            voice.target = value;
            voice.time_left = IntervalTime(voice.interval);
            voice.duration = voice.time_left;
            if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
            {
                apply_change(voice, current);
            }
            else
            {
                voice.delta = GetDelta(voice.duration);
                current = voice.target;
            }

            set_volume(voice, current);
        }
        else if (voice.time_left == -1)
        {
            if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
            {
                voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvVolumeChanged);
                set_volume(voice, voice.initial);
            }
        }
        else
        {
            voice.time_left -= kFrameMicroseconds;
            if (voice.time_left <= 0)
            {
                if (EnvelopeByte(voice.commands) == kEndCommand)
                {
                    if ((voice.envelope_flags & kEnvReleasing) != 0)
                    {
                        voice.envelope_flags = 0;
                        voice.has_commands = false;
                        voice.done_pending = true;
                    }
                    else
                    {
                        voice.time_left = -1;
                        voice.duration = -1;
                        voice.delta = kChunk;
                        voice.envelope_delta = 0.0F;
                        voice.initial = voice.target;
                    }
                }
                else
                {
                    voice.interval = StepEnvelope(voice);
                    voice.initial = voice.target;
                    voice.target = ReadEnvelope(voice.commands) & 0x7F;
                    voice.time_left = IntervalTime(voice.interval);
                    voice.duration = voice.time_left;
                    voice.envelope_delta =
                        voice.duration != 0 ? (static_cast<float>(voice.target) - static_cast<float>(voice.initial)) /
                                                  static_cast<float>(voice.duration)
                                            : 0.0F;
                    if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
                    {
                        apply_change(voice, current);
                    }
                    else
                    {
                        voice.delta = GetDelta(voice.duration);
                        current = voice.target;
                    }

                    set_volume(voice, current);
                }
            }
            else if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
            {
                apply_change(voice, current);
                set_volume(voice, current);
            }
        }
    }
}

void SbnPlayer::SynthStopVoice(int voice)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    held.playing = false;
    held.mixer_dirty = true;
    held.mixer_volume = 1;
    held.resampler_delta = 0.0F;
    held.resampler_first = true;
    held.last_sample = 0;
    held.decoder_first = true;
    held.sample = 0;
    if (held.physical_wave >= 0)
    {
        const Wave& wave = waves_[static_cast<std::size_t>(held.physical_wave)];
        held.memory_in = wave.data;
        if (wave.loop_end != 0)
        {
            held.loop_count = static_cast<int32_t>(wave.loop_count);
        }
    }
}

void SbnPlayer::SynthStartVoice(int voice)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    const Wave& wave = waves_[static_cast<std::size_t>(held.wave)];
    held.physical_bus = held.bus;
    held.physical_wave = held.wave;
    held.memory_in = wave.data;
    held.sample = 0;
    held.book_size = wave.book_size;
    if (wave.loop_end != 0)
    {
        held.loop_start_sample = wave.loop_start;
        held.loop_end_sample = wave.loop_end;
        held.loop_count = static_cast<int32_t>(wave.loop_count);
        if (wave.type == 0 && wave.loop_state != 0)
        {
            std::array<uint8_t, 32> state = {};
            build_memory_->Read(wave.loop_state, state.data(), state.size());
            build_memory_->Write(held.decoder_loop_state, state.data(), state.size());
        }
    }
    else
    {
        held.loop_start_sample = 0;
        held.loop_end_sample = 0;
        held.loop_count = 0;
    }

    held.playing = true;
    held.mixer_dirty = true;
    held.mixer_delta = 0;
    held.segment_end = held.delta;
    held.mixer_pan = held.pan;
    held.mixer_volume = static_cast<int16_t>((static_cast<int32_t>(held.volume) * held.volume) >> 15);
    held.dry = kEqualPower[held.reverb & 0x7F];
    held.wet = kEqualPower[127 - (held.reverb & 0x7F)];
    if (held.segment_end != 0)
    {
        held.current_left = 1;
        held.current_right = 1;
    }
    else
    {
        const std::size_t left = config_.stereo ? held.mixer_pan : 64;
        const std::size_t right = config_.stereo ? 127U - held.mixer_pan : 64;
        held.current_left = static_cast<int16_t>((held.mixer_volume * kEqualPower[left]) >> 15);
        held.current_right = static_cast<int16_t>((held.mixer_volume * kEqualPower[right]) >> 15);
    }

    held.ratio = held.pitch_ratio;
}

void SbnPlayer::RefreshMixer(Voice& voice)
{
    if (voice.mixer_delta >= voice.segment_end)
    {
        voice.mixer_delta = voice.segment_end;
        const std::size_t left = config_.stereo ? voice.mixer_pan : 64;
        const std::size_t right = config_.stereo ? 127U - voice.mixer_pan : 64;
        voice.current_left = static_cast<int16_t>((voice.mixer_volume * kEqualPower[left]) >> 15);
        voice.current_right = static_cast<int16_t>((voice.mixer_volume * kEqualPower[right]) >> 15);
    }
    else
    {
        voice.current_left = Ramp(voice.current_left, voice.mixer_delta, voice.rate_high_left, voice.rate_low_left);
        voice.current_right = Ramp(voice.current_right, voice.mixer_delta, voice.rate_high_right, voice.rate_low_right);
    }

    if (voice.current_left == 0)
    {
        voice.current_left = 1;
    }

    if (voice.current_right == 0)
    {
        voice.current_right = 1;
    }
}

void SbnPlayer::SynthSetMixer(int voice, int16_t volume, int32_t delta, uint8_t pan, uint8_t reverb)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    RefreshMixer(held);
    held.mixer_delta = 0;
    held.segment_end = delta;
    held.mixer_pan = pan;
    held.mixer_volume = static_cast<int16_t>((static_cast<int32_t>(volume) * volume) >> 15);
    held.dry = kEqualPower[reverb & 0x7F];
    held.wet = kEqualPower[127 - (reverb & 0x7F)];
    held.mixer_dirty = true;
}

void SbnPlayer::SynthSetPanAndMix(int voice, uint8_t pan, uint8_t reverb)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    RefreshMixer(held);
    held.mixer_pan = pan;
    held.dry = kEqualPower[reverb & 0x7F];
    held.wet = kEqualPower[127 - (reverb & 0x7F)];
    held.mixer_dirty = true;
}

void SbnPlayer::SynthSetVolumeDelta(int voice, int16_t volume, int32_t delta)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    RefreshMixer(held);
    held.mixer_volume = static_cast<int16_t>((static_cast<int32_t>(volume) * volume) >> 15);
    held.mixer_delta = 0;
    held.segment_end = delta;
    held.mixer_dirty = true;
}

void SbnPlayer::InitFilter(LowPass& filter, AudioMemory& memory)
{
    constexpr int kScale = 16384;
    filter.first = true;
    const int16_t time_constant = static_cast<int16_t>(filter.cutoff >> 1);
    filter.gain = static_cast<int16_t>(kScale - time_constant);
    filter.coefficients.fill(0);
    filter.coefficients[8] = time_constant;
    double attenuation = static_cast<double>(time_constant) / kScale;
    for (std::size_t i = 9; i < filter.coefficients.size(); i++)
    {
        attenuation *= static_cast<double>(time_constant) / kScale;
        filter.coefficients[i] = static_cast<int16_t>(attenuation * kScale);
    }

    std::array<uint8_t, 32> bytes = {};
    for (std::size_t i = 0; i < filter.coefficients.size(); i++)
    {
        xsflib::StoreBe16(bytes.data() + i * 2, static_cast<uint16_t>(filter.coefficients[i]));
    }

    memory.Write(filter.coefficients_address, bytes.data(), bytes.size());
}

void SbnPlayer::LoadEffect(Bus& bus, Effect& effect, uint8_t type)
{
    (void)bus;
    constexpr double kConvert = 173123.404906676;
    const EffectPreset& preset = PresetOf(type);
    effect.delay_count = static_cast<int>(preset.sections.size());
    effect.length = preset.length * kChunk;
    effect.input = 0;
    const std::vector<uint8_t> zeros(kFxLength * 2, 0);
    build_memory_->Write(effect.base, zeros.data(), zeros.size());
    for (std::size_t i = 0; i < preset.sections.size(); i++)
    {
        const EffectSection& section = preset.sections[i];
        Delay& delay = effect.delays[i];
        delay.input = static_cast<uint32_t>(section.input * kChunk);
        delay.output = static_cast<uint32_t>(section.output * kChunk);
        delay.feed_back = static_cast<int16_t>(section.feed_back);
        delay.feed_forward = static_cast<int16_t>(section.feed_forward);
        delay.gain = static_cast<int16_t>(section.gain);
        delay.has_resampler = section.rate != 0;
        if (delay.has_resampler)
        {
            delay.mod_increment = static_cast<float>(
                (2.0 * static_cast<double>(static_cast<float>(section.rate) / 1000.0F)) / config_.output_rate);
            delay.mod_gain = static_cast<float>(static_cast<double>(section.depth) / kConvert) *
                             static_cast<float>(delay.output - delay.input);
            delay.mod_value = 1.0F;
            delay.mod_delta = 0;
            delay.resampler_delta = 0.0F;
            delay.resampler_first = true;
        }

        delay.has_filter = section.cutoff != 0;
        if (delay.has_filter)
        {
            delay.filter.cutoff = static_cast<int16_t>(section.cutoff);
            InitFilter(delay.filter, *build_memory_);
        }
    }
}

void SbnPlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    build_memory_ = &memory;
    for (Player& player : players_)
    {
        player.frames++;
    }

    uint32_t place = output;
    for (int left = samples; left > 0; left -= kChunk)
    {
        RunChunkClients();
        BuildChunk(list, memory, place);
        place += kChunk * 4;
    }
}

void SbnPlayer::BuildChunk(AudioList& list, AudioMemory& memory, uint32_t place)
{
    constexpr uint32_t kSize = kChunkBytes;
    constexpr uint32_t kBoth = kChunkBytes * 2;
    const auto load = [&](uint32_t bytes, uint32_t dmem, uint32_t address)
    {
        list.commands.push_back({kOpLoad | (bytes << 12) | dmem, address});
    };
    const auto save = [&](uint32_t bytes, uint32_t dmem, uint32_t address)
    {
        list.commands.push_back({kOpSave | (bytes << 12) | dmem, address});
    };
    const auto mix = [&](uint16_t gain, uint32_t in, uint32_t out)
    {
        list.commands.push_back({kOpMix | gain, (in << 16) | out});
    };

    std::array<std::vector<int>, kBuses> by_bus;
    for (int i = 0; i < kVoices; i++)
    {
        const uint8_t bus = voices_[static_cast<std::size_t>(i)].physical_bus;
        if (bus < kBuses)
        {
            by_bus[bus].push_back(i);
        }
    }

    bool first_bus = true;
    for (int b = 0; b < kBuses; b++)
    {
        if (by_bus[static_cast<std::size_t>(b)].empty())
        {
            continue;
        }

        Bus& bus = buses_[static_cast<std::size_t>(b)];
        list.ClearBuffer(kMainLeft, 8 * kChunk);
        for (const int voice : by_bus[static_cast<std::size_t>(b)])
        {
            list.MarkVoice(voices_[static_cast<std::size_t>(voice)].host_track);
            PullVoice(list, voice, memory);
        }

        list.MarkVoicesEnd();

        if (bus.effect_type != 0)
        {
            PullEffect(list, bus.effects[0], kAuxLeft);
            PullEffect(list, bus.effects[1], kAuxRight);
        }

        if (b == delayed_bus_ && delayed_channel_ != kDelayNone)
        {
            const uint32_t main_out = delayed_channel_ == kDelayLeft ? kMainLeft : kMainRight;
            const uint32_t aux_out = delayed_channel_ == kDelayLeft ? kAuxLeft : kAuxRight;
            const auto now = static_cast<uint32_t>(delay_counter_ % delay_count_) * kSize;
            const auto next = static_cast<uint32_t>((delay_counter_ + 1) % delay_count_) * kSize;
            save(kSize, main_out, delay_main_ + now);
            load(kSize, main_out, delay_main_ + next);
            save(kSize, aux_out, delay_aux_ + now);
            load(kSize, aux_out, delay_aux_ + next);
        }

        if (first_bus)
        {
            list.ClearBuffer(kTemp, kBoth);
        }
        else
        {
            load(kBoth, kTemp, wet_accumulator_);
        }

        mix(bus.gain, kAuxLeft, kTemp);
        mix(bus.gain, kAuxRight, kTemp + kSize);
        save(kBoth, kTemp, wet_accumulator_);
        if (first_bus)
        {
            list.ClearBuffer(kTemp, kBoth);
            first_bus = false;
        }
        else
        {
            load(kBoth, kTemp, dry_accumulator_);
        }

        mix(bus.gain, kMainLeft, kTemp);
        mix(bus.gain, kMainRight, kTemp + kSize);
        save(kBoth, kTemp, dry_accumulator_);
    }

    list.MarkChunkEnd();
    list.commands.push_back({kOpMove, (kMainLeft << 16) | kBoth});
    load(kBoth, kAuxLeft, wet_accumulator_);
    mix(0x7FFF, kAuxLeft, kMainLeft);
    mix(0x7FFF, kAuxRight, kMainRight);
    list.commands.push_back({static_cast<uint32_t>(kAudioInterleave) << 24, 0});
    save(kBoth, 0, place);
    delay_counter_++;
}

void SbnPlayer::DecodeChunk(AudioList& list, Voice& voice, const Wave& wave, int tsam, int bytes, int16_t out,
                            uint32_t flags)
{
    (void)wave;
    uint32_t align = 0;
    if (bytes > 0)
    {
        const auto location = static_cast<uint32_t>(voice.memory_in);
        align = location & 7;
        const uint32_t total = static_cast<uint32_t>(bytes) + align;
        list.commands.push_back(
            {kOpLoad | (((total + 8 - (total & 7)) & kDmemMask) << 12) | kDecoderIn, location - align});
    }

    if ((flags & kAudioFlagLoop) != 0)
    {
        list.SetLoop(voice.decoder_loop_state);
    }

    list.commands.push_back({static_cast<uint32_t>(kAudioAdpcm) << 24 | (voice.decoder_state & kAddressMask),
                             (flags << 28) | ((static_cast<uint32_t>(tsam * 2) & kDmemMask) << 16) |
                                 ((align & 0xF) << 12) | (static_cast<uint32_t>(out) & kDmemMask)});
    voice.decoder_first = false;
}

void SbnPlayer::PullVoice(AudioList& list, int index, AudioMemory& memory)
{
    (void)memory;
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    if (!voice.playing)
    {
        return;
    }

    const Wave& wave = waves_[static_cast<std::size_t>(voice.physical_wave)];
    int out = kDecoderOut;
    voice.ratio = std::min(voice.ratio, kMaxRatio);
    voice.ratio = static_cast<float>(static_cast<int32_t>(voice.ratio * kUnityPitch));
    voice.ratio = voice.ratio / kUnityPitch;
    const float exact = voice.resampler_delta + voice.ratio * static_cast<float>(kChunk);
    int32_t count = static_cast<int32_t>(exact);
    voice.resampler_delta = exact - static_cast<float>(count);
    if (count != 0)
    {
        const auto loop_end = static_cast<int64_t>(voice.loop_end_sample);
        const int32_t wave_length = static_cast<int32_t>(wave.length / kFrameBytes * kFrameBytes);
        if (wave.type == 0)
        {
            list.LoadAdpcm(voice.book_size, wave.book);
            const bool looped = loop_end < static_cast<int64_t>(count) + voice.sample && voice.loop_count != 0;
            int32_t samples = looped ? static_cast<int32_t>(loop_end - voice.sample) : count;
            const int32_t left = voice.last_sample != 0 ? kFrameSamples - voice.last_sample : 0;
            int32_t tsam = std::max(samples - left, 0);
            int32_t frames = (tsam + kFrameSamples - 1) >> 4;
            int32_t bytes = frames * kFrameBytes;
            if (looped)
            {
                DecodeChunk(list, voice, wave, tsam, bytes, static_cast<int16_t>(out),
                            voice.decoder_first ? kAudioFlagInit : 0U);
                out += voice.last_sample != 0 ? voice.last_sample << 1 : kFrameSamples << 1;
                voice.last_sample = static_cast<int32_t>(voice.loop_start_sample & 0xF);
                voice.memory_in = static_cast<int64_t>(wave.data) + kFrameBytes * (voice.loop_start_sample >> 4);
                voice.sample = static_cast<int32_t>(voice.loop_start_sample);
                int buffer_end = out;
                while (count > samples)
                {
                    count -= samples;
                    const int place = (buffer_end + ((frames + 1) << 5) + 16) & ~0x1F;
                    buffer_end += samples << 1;
                    if (voice.loop_count != -1 && voice.loop_count != 0)
                    {
                        voice.loop_count--;
                    }

                    samples =
                        std::min<int32_t>(count, static_cast<int32_t>(voice.loop_end_sample - voice.loop_start_sample));
                    tsam = std::max(samples - kFrameSamples + voice.last_sample, 0);
                    frames = (tsam + kFrameSamples - 1) >> 4;
                    bytes = frames * kFrameBytes;
                    DecodeChunk(list, voice, wave, tsam, bytes, static_cast<int16_t>(place),
                                (voice.decoder_first ? kAudioFlagInit : 0U) | kAudioFlagLoop);
                    list.commands.push_back(
                        {kOpMove | (static_cast<uint32_t>(place + (voice.last_sample << 1)) & kAddressMask),
                         (static_cast<uint32_t>(buffer_end) << 16) | static_cast<uint32_t>(samples << 1)});
                }

                voice.last_sample = (count + voice.last_sample) & 0xF;
                voice.sample += count;
                voice.memory_in += static_cast<int64_t>(kFrameBytes) * frames;
            }
            else
            {
                samples = frames << 4;
                const int64_t over = voice.memory_in + bytes - (static_cast<int64_t>(wave.data) + wave_length);
                int32_t overflow = 0;
                if (over > 0)
                {
                    overflow = static_cast<int32_t>(over);
                    voice.playing = false;
                }

                int32_t over_samples = (overflow / kFrameBytes) << 4;
                over_samples = std::min(over_samples, samples + left);
                bytes -= overflow;
                bool decoded = false;
                if (over_samples - (over_samples & 0xF) < count)
                {
                    decoded = true;
                    DecodeChunk(list, voice, wave, samples - over_samples, bytes, static_cast<int16_t>(out),
                                voice.decoder_first ? kAudioFlagInit : 0U);
                    out += voice.last_sample != 0 ? voice.last_sample << 1 : kFrameSamples << 1;
                    voice.last_sample = (count + voice.last_sample) & 0xF;
                    voice.sample += count;
                    voice.memory_in += static_cast<int64_t>(kFrameBytes) * frames;
                }
                else
                {
                    voice.last_sample = 0;
                    voice.memory_in += static_cast<int64_t>(kFrameBytes) * frames;
                }

                if (over_samples != 0)
                {
                    voice.last_sample = 0;
                    const int32_t start = decoded ? (left + samples - over_samples) << 1 : 0;
                    list.ClearBuffer(static_cast<uint32_t>(start + out), static_cast<uint32_t>(over_samples << 1));
                }
            }
        }
        else
        {
            const auto byte_load = [&](int32_t bytes, int dmem)
            {
                const auto location = static_cast<uint32_t>(voice.memory_in);
                const uint32_t align = location & 7;
                const uint32_t total = static_cast<uint32_t>(bytes) + align;
                list.commands.push_back(
                    {kOpLoad | (((total + 8 - (total & 7)) & kDmemMask) << 12) | static_cast<uint32_t>(dmem),
                     location - align});

                return align;
            };

            if (loop_end < static_cast<int64_t>(count) + voice.sample && voice.loop_count != 0)
            {
                int32_t samples = static_cast<int32_t>(loop_end - voice.sample);
                uint32_t align = 0;
                if (samples > 0)
                {
                    align = byte_load(samples << 1, out);
                }

                out += static_cast<int>(align);
                voice.memory_in = static_cast<int64_t>(wave.data) + (voice.loop_start_sample << 1);
                voice.sample = static_cast<int32_t>(voice.loop_start_sample);
                int place = out;
                while (count > samples)
                {
                    place += samples << 1;
                    count -= samples;
                    if (voice.loop_count != -1 && voice.loop_count != 0)
                    {
                        voice.loop_count--;
                    }

                    samples =
                        std::min<int32_t>(count, static_cast<int32_t>(voice.loop_end_sample - voice.loop_start_sample));
                    const int dmem_align = (place & 7) != 0 ? 8 - (place & 7) : 0;
                    const uint32_t ram_align = byte_load(samples << 1, place + dmem_align);
                    if (ram_align != 0 || dmem_align != 0)
                    {
                        list.commands.push_back(
                            {kOpMove | (static_cast<uint32_t>(place + static_cast<int>(ram_align) + dmem_align) &
                                        kAddressMask),
                             (static_cast<uint32_t>(place) << 16) | static_cast<uint32_t>(samples * 2)});
                    }
                }

                voice.sample += count;
                voice.memory_in += static_cast<int64_t>(count) << 1;
            }
            else
            {
                int32_t bytes = count << 1;
                const int64_t over = voice.memory_in + bytes - (static_cast<int64_t>(wave.data) + wave.length);
                int32_t overflow = 0;
                if (over > 0)
                {
                    overflow = static_cast<int32_t>(over);
                    voice.playing = false;
                }

                overflow = std::min(overflow, bytes);
                if (overflow < bytes)
                {
                    if (count > 0)
                    {
                        bytes -= overflow;
                        out += static_cast<int>(byte_load(bytes, out));
                    }

                    voice.sample += count;
                    voice.memory_in += static_cast<int64_t>(count) << 1;
                }
                else
                {
                    voice.memory_in += static_cast<int64_t>(count) << 1;
                }

                if (overflow != 0)
                {
                    const int32_t start = std::max((count << 1) - overflow, 0);
                    list.ClearBuffer(static_cast<uint32_t>(start + out), static_cast<uint32_t>(overflow));
                }
            }
        }
    }

    const auto increment = static_cast<int32_t>(voice.ratio * kUnityPitch);
    list.commands.push_back({static_cast<uint32_t>(kAudioResample) << 24 | (voice.resampler_state & kAddressMask),
                             (voice.resampler_first ? 1U << 30 : 0U) |
                                 ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                                 ((static_cast<uint32_t>(out) & kDmemMask) << 2)});
    voice.resampler_first = false;
    if (voice.mixer_dirty)
    {
        constexpr uint32_t kSetTargetLeft = 0x09000000;
        constexpr uint32_t kSetCurrentLeft = 0x09060000;
        constexpr uint32_t kSetTargetRight = 0x09040000;
        constexpr uint32_t kEnvMixerInit = 0x03010000;
        const auto pair = [](int16_t high, uint16_t low)
        {
            return (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16) | low;
        };
        voice.mixer_dirty = false;
        const std::size_t left = config_.stereo ? voice.mixer_pan : 64;
        const std::size_t right = config_.stereo ? 127U - voice.mixer_pan : 64;
        voice.target_left = static_cast<int16_t>((voice.mixer_volume * kEqualPower[left]) >> 15);
        voice.target_right = static_cast<int16_t>((voice.mixer_volume * kEqualPower[right]) >> 15);
        voice.rate_high_left = Rate(voice.current_left, voice.target_left, voice.segment_end, voice.rate_low_left);
        voice.rate_high_right = Rate(voice.current_right, voice.target_right, voice.segment_end, voice.rate_low_right);
        list.commands.push_back({kSetTargetLeft | static_cast<uint16_t>(voice.target_left),
                                 pair(voice.rate_high_left, voice.rate_low_left)});
        list.commands.push_back({kSetCurrentLeft | static_cast<uint16_t>(voice.current_left),
                                 pair(voice.dry, static_cast<uint16_t>(voice.wet))});
        list.commands.push_back({kSetTargetRight | static_cast<uint16_t>(voice.target_right),
                                 pair(voice.rate_high_right, voice.rate_low_right)});
        list.commands.push_back({kEnvMixerInit | static_cast<uint16_t>(voice.current_right), voice.mixer_state});
    }
    else
    {
        list.EnvMixer(0, voice.mixer_state);
    }

    voice.mixer_delta += kChunk;
    if (voice.segment_end < voice.mixer_delta)
    {
        voice.mixer_delta = voice.segment_end;
    }

    if (!voice.playing)
    {
        voice.mixer_dirty = true;
        voice.mixer_volume = 1;
        voice.resampler_delta = 0.0F;
        voice.resampler_first = true;
        voice.last_sample = 0;
        voice.decoder_first = true;
        voice.sample = 0;
        voice.memory_in = wave.data;
        voice.loop_count = static_cast<int32_t>(wave.loop_count);
        voice.priority = 0;
        voice.has_commands = false;
    }
}

void SbnPlayer::LoadDelayLine(AudioList& list, const Effect& effect, int64_t position, int buffer, int count)
{
    const int64_t end = position + count;
    const auto address = static_cast<uint32_t>(effect.base + position * 2);
    if (end > effect.length)
    {
        const auto before = static_cast<uint32_t>(effect.length - position) << 1;
        const auto after = static_cast<uint32_t>(end - effect.length) << 1;
        list.commands.push_back({kOpLoad | ((before & kDmemMask) << 12) | static_cast<uint32_t>(buffer), address});
        list.commands.push_back(
            {kOpLoad | ((after & kDmemMask) << 12) | (static_cast<uint32_t>(buffer) + before), effect.base});
    }
    else
    {
        list.commands.push_back(
            {kOpLoad | ((static_cast<uint32_t>(count << 1) & kDmemMask) << 12) | static_cast<uint32_t>(buffer),
             address});
    }
}

void SbnPlayer::PullEffect(AudioList& list, Effect& effect, int16_t wet)
{
    constexpr uint32_t kSize = kChunkBytes;
    const auto load = [&](int buffer, uint32_t address)
    {
        list.commands.push_back({kOpLoad | (kSize << 12) | static_cast<uint32_t>(buffer), address});
    };
    const auto save = [&](int buffer, uint32_t address)
    {
        list.commands.push_back({kOpSave | (kSize << 12) | static_cast<uint32_t>(buffer), address});
    };
    const auto mix = [&](int16_t gain, int in, int out)
    {
        list.commands.push_back(
            {kOpMix | static_cast<uint16_t>(gain), (static_cast<uint32_t>(in) << 16) | static_cast<uint32_t>(out)});
    };

    int input_buffer = kTemp;
    int output_buffer = kTemp + kChunkBytes;
    const int resample_buffer = kTemp + kChunkBytes * 2;
    const auto place_of = [&](int64_t position)
    {
        return static_cast<uint32_t>(effect.base + position * 2);
    };
    save(wet, place_of(effect.input));
    list.ClearBuffer(static_cast<uint32_t>(wet), kSize);
    int64_t previous_output = -0x10000000;
    for (int i = 0; i < effect.delay_count; i++)
    {
        Delay& delay = effect.delays[static_cast<std::size_t>(i)];
        int64_t in_place = effect.input - static_cast<int64_t>(delay.input);
        if (in_place < 0)
        {
            in_place += effect.length;
        }

        int64_t out_place = effect.input - static_cast<int64_t>(delay.output);
        if (out_place < 0)
        {
            out_place += effect.length;
        }

        if (in_place == previous_output)
        {
            std::swap(input_buffer, output_buffer);
        }
        else
        {
            load(input_buffer, place_of(in_place));
        }

        if (delay.has_resampler)
        {
            const auto length = static_cast<int32_t>(delay.output - delay.input);
            delay.mod_value += delay.mod_increment * static_cast<float>(kChunk);
            if (static_cast<double>(delay.mod_value) > 2.0)
            {
                delay.mod_value = static_cast<float>(static_cast<double>(delay.mod_value) - 4.0);
            }

            float shape = delay.mod_value;
            if (shape < 0.0F)
            {
                shape = -shape;
            }

            shape = static_cast<float>(static_cast<double>(shape) - 1.0);
            float delta = delay.mod_gain * shape;
            delta /= static_cast<float>(length);
            delta = static_cast<float>(static_cast<int32_t>(delta * kUnityPitch));
            delta = delta / kUnityPitch;
            const auto ratio = static_cast<float>(1.0 - static_cast<double>(delta));
            const float exact = delay.resampler_delta + ratio * static_cast<float>(kChunk);
            const auto samples = static_cast<int32_t>(exact);
            delay.resampler_delta = exact - static_cast<float>(samples);
            int64_t resample_place = effect.input - (static_cast<int64_t>(delay.output) - delay.mod_delta);
            const int align = static_cast<int>((effect.base + resample_place * 2) & 7) >> 1;
            resample_place -= align;
            if (resample_place < 0)
            {
                resample_place += effect.length;
            }

            LoadDelayLine(list, effect, resample_place, resample_buffer, samples + align);
            const auto increment = static_cast<int32_t>(ratio * kUnityPitch);
            list.commands.push_back(
                {static_cast<uint32_t>(kAudioResample) << 24 | (delay.resampler_state & kAddressMask),
                 (delay.resampler_first ? 1U << 30 : 0U) | ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                     ((static_cast<uint32_t>((align << 1) + resample_buffer) & kDmemMask) << 2) |
                     ((static_cast<uint32_t>(output_buffer) >> 8) & 3)});
            delay.resampler_first = false;
            delay.mod_delta += samples - kChunk;
        }
        else
        {
            load(output_buffer, place_of(out_place));
        }

        if (delay.feed_forward != 0)
        {
            mix(delay.feed_forward, input_buffer, output_buffer);
            if (!delay.has_resampler && !delay.has_filter)
            {
                save(output_buffer, place_of(out_place));
            }
        }

        if (delay.feed_back != 0)
        {
            mix(delay.feed_back, output_buffer, input_buffer);
            save(input_buffer, place_of(in_place));
        }

        if (delay.has_filter)
        {
            list.LoadAdpcm(32, delay.filter.coefficients_address);
            list.commands.push_back(
                {static_cast<uint32_t>(kAudioPoleFilter) << 24 | (delay.filter.first ? 1U << 16 : 0U) |
                     static_cast<uint16_t>(delay.filter.gain),
                 ((static_cast<uint32_t>(output_buffer) >> 8) << 24) | (delay.filter.state & kAddressMask)});
            delay.filter.first = false;
        }

        if (!delay.has_resampler)
        {
            save(output_buffer, place_of(out_place));
        }

        if (delay.gain != 0)
        {
            mix(delay.gain, output_buffer, wet);
        }

        previous_output = effect.input + static_cast<int64_t>(delay.output);
    }

    effect.input += kChunk;
    if (effect.input >= effect.length)
    {
        effect.input = 0;
    }
}

} // namespace ntdlib::n64

// SPDX-License-Identifier: MIT

#include "ntdlib/n64/sbn_player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "xsflib/bytes.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/sbn.h"
#include "sbn_player_constants.h"

namespace ntdlib::n64
{

namespace
{

using sbn_player_constants::kChunk;
using sbn_player_constants::kChunkBytes;
using sbn_player_constants::kDelayLeft;
using sbn_player_constants::kDelayNone;
using sbn_player_constants::kDelayRight;
using sbn_player_constants::kFxLength;

constexpr int kFrameMicroseconds = 5750;
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

// The arguments of commands 0xE0 to 0xFF, from the same table of the game's code as the reader's.
constexpr std::array<uint8_t, 32> kArgumentCounts = {2, 1, 1, 1, 4, 3, 2, 0, 2, 1, 1, 1, 1, 1, 1, 2,
                                                     3, 1, 1, 0, 2, 1, 3, 1, 0, 0, 0, 0, 3, 3, 3, 3};

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
    if (player.loop_ends[depth] == 0)
    {
        player.loop_ends[depth] = player.composition;
        player.loop_counters[depth] = count;
        player.composition = player.loop_labels[label];
    }
    else if (player.loop_ends[depth] == player.composition)
    {
        RepeatCompositionLoop(player, depth, label);
    }
    else if (depth < 3)
    {
        depth++;
        player.loop_ends[depth] = player.composition;
        player.loop_counters[depth] = count;
        player.composition = player.loop_labels[label];
    }

    player.loop_depth = depth;
}

// Ends a pass of the loop at `depth`. Jumps back to the label of the loop, or leaves the loop after the last pass. A
// count of 0 repeats the loop without end.
void SbnPlayer::RepeatCompositionLoop(Player& player, uint32_t& depth, uint32_t label)
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

            return;
        }
    }

    player.composition = player.loop_labels[label];
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
    const bool fading = StepPlayerRamps(player);
    bool finished = false;
    player.volume_changed = false;
    for (int t = 0; t < kTracks; t++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(t)];
        if (track.position == 0)
        {
            continue;
        }

        StepTrackRamps(track, fading || player.volume_changed);
        track.delay--;
        if (track.delay <= 0 && RunTrackCommands(index, t, track))
        {
            finished = true;
        }

        UpdateTrackNotes(player, track);
    }

    if (finished)
    {
        player.state = kStateFetch;
    }
}

// Steps the tempo ramp and the volume ramp of a player. Returns whether the volume ramps.
bool SbnPlayer::StepPlayerRamps(Player& player)
{
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

    if (player.volume_ticks == 0)
    {
        return false;
    }

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

    return true;
}

// Clears the change flags of a track and steps the volume ramps of the track.
void SbnPlayer::StepTrackRamps(Track& track, bool changed_volume)
{
    track.changed_pan = false;
    track.changed_reverb = false;
    track.changed_tune = false;
    track.changed_volume = changed_volume;
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
        track.proximity_volume =
            track.proximity_ticks == 0 ? track.proximity_target << 16 : track.proximity_volume + track.proximity_step;
        track.changed_volume = true;
    }
}

// Runs the commands of a track until one sets a delay. Returns true when the track data ends.
bool SbnPlayer::RunTrackCommands(int index, int t, Track& track)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    uint8_t search_from = track.first_voice;
    while (track.delay == 0)
    {
        const uint32_t command_place = track.position;
        const uint8_t opcode = ReadTrackByte(player, track);
        if (opcode == 0)
        {
            if (track.previous_position == 0)
            {
                return true;
            }

            track.position = track.previous_position;
            track.previous_position = 0;
        }
        else if (opcode < 0x78)
        {
            track.delay = opcode;
        }
        else if (opcode < 0x80)
        {
            track.delay = ((opcode & 7) << 8) + ReadTrackByte(player, track) + 0x78;
        }
        else if (opcode < 0xD4)
        {
            StartNote(index, t, track, opcode, command_place, search_from);
        }
        else
        {
            RunCommand(index, track, opcode);
        }
    }

    return false;
}

// Reads the velocity and the length of a note command, then starts the note on a voice of the track.
void SbnPlayer::StartNote(int index, int t, Track& track, uint8_t opcode, uint32_t command_place, uint8_t& search_from)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    const int32_t key = opcode & 0x7F;
    const uint8_t velocity = ReadTrackByte(player, track);
    int32_t length = ReadTrackByte(player, track);
    if (length >= 0xC0)
    {
        length = ((length & 0x3F) << 8) + ReadTrackByte(player, track) + 0xC0;
    }

    uint8_t voice_index = 0;
    if (track.muted || !AcquireVoice(player, track, search_from, voice_index))
    {
        return;
    }

    Voice& voice = voices_[voice_index];
    Note& note = player.notes[voice_index];
    note.tremolo_depth = 0;
    note.velocity = velocity > 0 ? velocity + 1 : 0;
    note.length = length;
    const int32_t level =
        (((player.master_volume >> 21) * (track.instrument_volume >> 21) * (track.proximity_volume >> 21)) >> 20);
    if (track.drums)
    {
        SetUpDrumNote(player, track, key, level, voice, note);
    }
    else
    {
        SetUpInstrumentNote(index, track, key, level, voice, note);
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
        on_note({index, t, static_cast<uint8_t>(key), velocity, length, track.drums, command_place, voice_index});
    }
}

// Finds a voice for a new note of a track. Uses the next free voice of the track, or frees one by the rules of the
// polyphony. Returns false when no voice is free.
bool SbnPlayer::AcquireVoice(Player& player, const Track& track, uint8_t& search_from, uint8_t& voice_index)
{
    for (voice_index = search_from; voice_index < track.last_voice; voice_index++)
    {
        search_from++;
        if (voices_[voice_index].priority == 0)
        {
            return true;
        }
    }

    if (track.polyphony >= 5)
    {
        return StealVoice(player, track, voice_index);
    }

    voice_index = track.first_voice;
    player.notes[voice_index].length = 0;
    if (voices_[voice_index].priority <= player.priority)
    {
        ResetVoice(voice_index);

        return true;
    }

    return false;
}

// Frees a voice of a track of polyphony 5 or more: one of a lower priority, then one whose note has ended, then the
// one whose note has the shortest length left.
bool SbnPlayer::StealVoice(Player& player, const Track& track, uint8_t& voice_index)
{
    for (voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
    {
        if (voices_[voice_index].priority < player.priority)
        {
            ResetVoice(voice_index);

            return true;
        }
    }

    for (voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
    {
        if (voices_[voice_index].priority == player.priority && player.notes[voice_index].length == 0)
        {
            ResetVoice(voice_index);

            return true;
        }
    }

    bool acquired = false;
    int32_t shortest = 0xFFFF;
    for (uint8_t other = track.first_voice; other < track.last_voice; other++)
    {
        const Note& held = player.notes[other];
        if (voices_[other].priority == player.priority && !held.pending_tick && held.length < shortest)
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

    return acquired;
}

// Sets the wave, the volume, the pitch, the pan and the reverb of a drum note from the drum of its key.
void SbnPlayer::SetUpDrumNote(const Player& player, const Track& track, int32_t key, int32_t level, Voice& voice,
                              Note& note)
{
    const BgmDrum* drum = nullptr;
    if (key < 72)
    {
        drum = &global_drums_[static_cast<std::size_t>(key)];
    }
    else if (!player.parsed->drums.empty())
    {
        const auto number = static_cast<std::size_t>(key - 72);
        drum = number < player.parsed->drums.size() ? &player.parsed->drums[number] : &player.parsed->drums[0];
    }
    else
    {
        drum = &global_drums_[0];
    }

    note.wave = WaveOf(drum->bank_patch >> 8, drum->bank_patch & 0xFF, voice.envelope);
    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
    note.volume = drum->random_volume != 0
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
    voice.pan = drum->random_pan != 0 ? RandomPan(player.random_1, static_cast<uint8_t>(drum->pan), drum->random_pan)
                                      : static_cast<uint8_t>(drum->pan);
    voice.reverb =
        drum->random_reverb != 0 ? RandomReverb(player.random_1, drum->reverb, drum->random_reverb) : drum->reverb;
}

// Sets the wave, the volume, the pitch, the pan, the reverb and the envelope of a note from the instrument of its
// track.
void SbnPlayer::SetUpInstrumentNote(int index, const Track& track, int32_t key, int32_t level, Voice& voice, Note& note)
{
    const Player& player = players_[static_cast<std::size_t>(index)];
    note.volume = (level * (track.volume * note.velocity)) >> 9;
    voice.client_volume = static_cast<int16_t>(note.volume);
    note.wave = track.wave >= 0 ? track.wave : default_wave_;
    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
    note.detune = key * 100 + track.coarse_detune + player.pitch_shift + track.fine_detune - wave.key_base;
    note.pitch_ratio = PitchRatio(note.detune + track.detune + player.detune) * wave.pitch_ratio;
    voice.pan = track.random_pan != 0 ? RandomPan(player.random_1, track.pan, track.random_pan) : track.pan;
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

// Steps the notes that a track plays on the voices of its range.
void SbnPlayer::UpdateTrackNotes(Player& player, const Track& track)
{
    if (track.muted)
    {
        return;
    }

    for (uint8_t voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
    {
        Voice& voice = voices_[voice_index];
        if (voice.priority != player.priority)
        {
            continue;
        }

        Note& note = player.notes[voice_index];
        if (!note.pending_tick)
        {
            UpdateNote(player, track, voice, note);
        }

        note.pending_tick = false;
    }
}

// Counts down the length of a note and applies the changes of the track to the voice of the note.
void SbnPlayer::UpdateNote(const Player& player, const Track& track, Voice& voice, Note& note)
{
    if (note.length > 0)
    {
        note.length--;
        if (note.length == 0)
        {
            voice.envelope_flags |= kEnvKeyReleased;
        }
    }

    const int32_t level =
        (((player.master_volume >> 21) * (track.instrument_volume >> 21) * (track.proximity_volume >> 21)) >> 20);
    if (track.drums)
    {
        UpdateDrumNote(player, track, level, voice, note);
    }
    else
    {
        UpdateInstrumentNote(player, track, level, voice, note);
    }
}

// Applies the changes of tune and volume of a track to a drum note.
void SbnPlayer::UpdateDrumNote(const Player& player, const Track& track, int32_t level, Voice& voice, Note& note)
{
    if (track.changed_tune || player.detune != 0)
    {
        SetNotePitch(voice, note, (note.detune + note.rand_detune) + track.detune + player.detune);
    }

    if (track.changed_volume)
    {
        voice.client_volume = static_cast<int16_t>((level * (track.volume * note.volume)) >> 16);
        voice.envelope_flags |= kEnvVolumeChanged;
    }
}

// Steps the tremolo of an instrument note and applies the changes of tune, volume, pan and reverb of the track.
void SbnPlayer::UpdateInstrumentNote(const Player& player, const Track& track, int32_t level, Voice& voice, Note& note)
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
            int32_t detune = (quadrant == 1 || quadrant == 3) ? static_cast<uint8_t>(~shape) + 1 : shape;
            detune = (detune * track.tremolo_depth) >> 8;
            if (quadrant >= 2)
            {
                detune = -detune;
            }

            SetNotePitch(voice, note, detune + note.detune + track.detune + player.detune);
        }
    }
    else if (track.changed_tune || player.detune != 0)
    {
        SetNotePitch(voice, note, note.detune + track.detune + player.detune);
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

// Sets the pitch of a note from a tuning in cents. Marks the voice for a pitch sync when the pitch changes.
void SbnPlayer::SetNotePitch(Voice& voice, Note& note, int32_t tuning)
{
    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
    note.pitch_ratio = PitchRatio(tuning) * wave.pitch_ratio;
    if (voice.pitch_ratio != note.pitch_ratio)
    {
        voice.pitch_ratio = note.pitch_ratio;
        voice.sync_flags |= kSyncPitch;
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
    for (Voice& voice : voices_)
    {
        if (!voice.has_commands)
        {
            continue;
        }

        if ((voice.envelope_flags & kEnvHandledChange) != 0)
        {
            AfterVolumeChange(voice);
        }
        else if ((voice.envelope_flags & kEnvKeyReleased) != 0)
        {
            ReleaseEnvelope(voice);
        }
        else if (voice.time_left == -1)
        {
            if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
            {
                voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvVolumeChanged);
                SetEnvelopeVolume(voice, voice.initial);
            }
        }
        else
        {
            StepVoiceEnvelope(voice);
        }
    }
}

// Returns the envelope level of a voice at the current time of the segment.
int32_t SbnPlayer::EnvelopeLevel(const Voice& voice)
{
    return voice.initial +
           static_cast<int32_t>(voice.envelope_delta * static_cast<float>(voice.duration - voice.time_left));
}

// Applies a change of the client volume of a voice during a segment. Returns the envelope level to use.
int32_t SbnPlayer::ApplyVolumeChange(Voice& voice)
{
    int32_t current = 0;
    voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvVolumeChanged);
    if (voice.time_left > kFrameMicroseconds)
    {
        voice.time_left -= kFrameMicroseconds;
        voice.envelope_flags |= kEnvHandledChange;
        current = EnvelopeLevel(voice);
    }
    else
    {
        current = voice.target;
    }

    voice.delta = kChunk;

    return current;
}

// Sets the volume of a voice from an envelope level and marks the voice for a mixer sync.
void SbnPlayer::SetEnvelopeVolume(Voice& voice, int32_t current)
{
    voice.volume = static_cast<int16_t>(Multiply4(current, voice.client_volume, voice.relative_start, voice.scale));
    voice.sync_flags |= kSyncParams;
}

// Sets the volume and the ramp length of a voice at the start of an envelope segment.
void SbnPlayer::SetSegmentVolume(Voice& voice)
{
    int32_t current = 0;
    if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
    {
        current = ApplyVolumeChange(voice);
    }
    else
    {
        voice.delta = GetDelta(voice.duration);
        current = voice.target;
    }

    SetEnvelopeVolume(voice, current);
}

// Starts the release envelope of a voice whose key has been released.
void SbnPlayer::ReleaseEnvelope(Voice& voice)
{
    voice.envelope_flags = static_cast<uint8_t>((voice.envelope_flags & ~kEnvKeyReleased) | kEnvReleasing);
    voice.commands = voice.envelope.release;
    voice.initial = voice.time_left > kFrameMicroseconds ? EnvelopeLevel(voice) : voice.target;
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
    SetSegmentVolume(voice);
}

// Steps the envelope of a voice by a chunk. Starts the next segment when the current one ends. Holds or ends the
// envelope at the end of the commands.
void SbnPlayer::StepVoiceEnvelope(Voice& voice)
{
    voice.time_left -= kFrameMicroseconds;
    if (voice.time_left > 0)
    {
        if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
        {
            const int32_t current = ApplyVolumeChange(voice);
            SetEnvelopeVolume(voice, current);
        }

        return;
    }

    if (EnvelopeByte(voice.commands) != kEndCommand)
    {
        voice.interval = StepEnvelope(voice);
        voice.initial = voice.target;
        voice.target = ReadEnvelope(voice.commands) & 0x7F;
        voice.time_left = IntervalTime(voice.interval);
        voice.duration = voice.time_left;
        voice.envelope_delta = voice.duration != 0
                                   ? (static_cast<float>(voice.target) - static_cast<float>(voice.initial)) /
                                         static_cast<float>(voice.duration)
                                   : 0.0F;
        SetSegmentVolume(voice);

        return;
    }

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

} // namespace ntdlib::n64

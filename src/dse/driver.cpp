// SPDX-License-Identifier: MIT

#include "ntdlib/dse/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "driver_state.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/tables.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::dse
{

using ntr::snd::CommandId;

void Driver::State::StepVoices()
{
    for (int i = 0; i < voice_count; i++)
    {
        Voice& voice = VoiceAt(i);
        const int8_t level = voice.envelope.Tick(tick);
        if (static_cast<int8_t>(voice.envelope.active) == 1)
        {
            voice.flags |= StepLfos(voice.lfos);
            if (voice.envelope.state == kReleaseEnd)
            {
                voice.envelope.state = kDone;
                voice.flags |= 2;
            }
            if ((voice.state & 0xFF) != static_cast<uint8_t>(level))
            {
                voice.flags |= 0x20;
            }
        }
        voice.state = (voice.state & ~uint32_t{0xFF}) | static_cast<uint8_t>(level);
    }
}

void Driver::State::UpdateChannels(Synth& synth)
{
    for (Channel& channel : synth.channels)
    {
        const uint16_t update = channel.update | synth.update;
        if (update == 0)
        {
            continue;
        }
        if (update & 0x10)
        {
            channel.bend = static_cast<uint16_t>(synth.bend + channel.tuning +
                                                 static_cast<int16_t>(channel.bend_fade.current >> 16));
        }
        if (update & 0x20)
        {
            const int32_t product =
                Wrap(int64_t{synth.combined_volume} * ((channel.volume_fade.current >> 16) * channel.expression));
            channel.volume = static_cast<uint16_t>(product / (127 * 127));
        }
        if (update & 0x40)
        {
            channel.pan = static_cast<int16_t>(synth.pan - 0x40 + (channel.pan_fade.current >> 16));
        }
        ForEachVoice(channel, [update](Voice& voice) { voice.flags |= update; });
        channel.update = 0;
    }
    synth.update = 0;
}

void Driver::State::UpdateVoices()
{
    if (build == Build::kEarlier)
    {
        if (song)
        {
            UpdateChannels(song->synth);
        }
        for (const OwnedSequence& effect : effects)
        {
            UpdateChannels(effect->synth);
        }
    }
    for (int i = 0; i < voice_count; i++)
    {
        Voice& voice = VoiceAt(i);
        if (voice.channel == nullptr || voice.flags == 0)
        {
            continue;
        }
        const Channel& channel = *voice.channel;
        if (voice.flags & 0x10)
        {
            const auto pitch =
                static_cast<uint16_t>(channel.bend + voice.note_key + voice.key_bend + voice.lfos.outputs[1]);
            voice.timer = VoiceTimer(pitch, voice.Format());
        }
        if (voice.flags & 0x20)
        {
            const int32_t level = std::clamp(voice.note_volume + (voice.lfos.outputs[2] >> 6), 0, 0x7F);
            const int32_t product = Wrap(int64_t{channel.volume} * level * (voice.state & 0xFF));
            const int32_t scaled = product / 8032;
            voice.volume = Wrap(int64_t{scaled} * scaled) / 512;
        }
        if (voice.flags & 0x40)
        {
            int32_t pan = voice.wave_pan - 0x40 + channel.pan + (voice.lfos.outputs[3] >> 6);
            pan = std::clamp(pan, 0, 0x7F);
            voice.pan = tables::kPanCurve[pan];
        }
    }
}

void Driver::State::WriteCommands(ntr::snd::CommandList& list)
{
    for (int i = 0; i < voice_count; i++)
    {
        Voice& voice = VoiceAt(i);
        const uint16_t flags = voice.flags;
        if (flags == 0)
        {
            continue;
        }
        if (flags & 1)
        {
            const uint32_t loop = static_cast<int8_t>(voice.sample[5]) != 0 ? 1 : 2;
            const auto timer = static_cast<uint32_t>(voice.timer);
            const auto volume = static_cast<uint32_t>(voice.volume);
            const auto pan = static_cast<uint32_t>(voice.pan);
            switch (voice.Format())
            {
            case 0:
            case 1:
            case 2:
                list.push_back(
                    {CommandId::kSetUpPcmChannel,
                     {voice.index | timer << 16, voice.Address(), xsflib::LoadLe32(&voice.sample[0x1C]) | volume << 24,
                      xsflib::LoadLe32(&voice.sample[0x18]) | pan << 16 | uint32_t{voice.Format()} << 24 |
                          loop << 26}});
                break;
            case 3:
                list.push_back({CommandId::kSetUpPsgChannel, {voice.index, volume, pan | timer << 8, voice.sample[9]}});
                break;
            case 4:
                list.push_back({CommandId::kSetUpNoiseChannel, {voice.index, volume, pan | timer << 8, 0}});
                break;
            default:
                break;
            }
            MarkStarted(voice);
        }
        else
        {
            uint16_t update = flags;
            if (update & 2)
            {
                MarkStopped(voice);
                update = 0;
            }
            if (update & 0x10)
            {
                list.push_back({CommandId::kChannelTimer, {voice.bit, static_cast<uint32_t>(voice.timer), 0, 0}});
            }
            if (update & 0x20)
            {
                list.push_back({CommandId::kChannelVolume, {voice.bit, static_cast<uint32_t>(voice.volume), 0, 0}});
            }
            if (update & 0x40)
            {
                list.push_back({CommandId::kChannelPan, {voice.bit, static_cast<uint32_t>(voice.pan), 0, 0}});
            }
        }
        voice.flags = 0;
    }

    last_start_mask = start_mask;
    if (start_mask != 0 || build == Build::kEarlier)
    {
        list.push_back({CommandId::kStartTimer, {start_mask, 0, 0, 0}});
        start_mask = 0;
    }
    // The earlier build keeps a freed voice in the stops of the lists that follow until the voice starts again.
    if ((stop_mask | deallocated_mask) != 0 || build == Build::kEarlier)
    {
        list.push_back({CommandId::kStopTimer, {uint32_t{stop_mask} | deallocated_mask, 0, 0, 0}});
        if (build == Build::kLater)
        {
            deallocated_mask = 0;
        }
        stop_mask = 0;
    }
}

void Driver::State::CheckEndedChannels(uint16_t channel_status)
{
    auto status = static_cast<uint16_t>(channel_status | last_start_mask);
    for (int i = 0; i < voice_count; i++)
    {
        Voice& voice = VoiceAt(i);
        if (static_cast<int8_t>(voice.envelope.active) == 1)
        {
            if (!(status & 1))
            {
                voice.envelope.Stop();
                voice.state &= ~uint32_t{0xFFFF};
            }
        }
        else if (status & 1)
        {
            MarkStopped(voice);
        }
        status = static_cast<uint16_t>(status >> 1);
    }
}

void Driver::State::Repoint(const State& from)
{
    std::map<const Channel*, Channel*> channels;
    std::map<const Synth*, Synth*> synths;
    const auto pair_sequences = [&](const Sequence& old_one, Sequence& new_one)
    {
        synths[&old_one.synth] = &new_one.synth;
        for (std::size_t i = 0; i < old_one.synth.channels.size(); i++)
        {
            channels[&old_one.synth.channels[i]] = &new_one.synth.channels[i];
        }
    };
    if (from.song)
    {
        pair_sequences(*from.song, *song);
    }
    for (std::size_t i = 0; i < from.effects.size(); i++)
    {
        pair_sequences(*from.effects[i], *effects[i]);
    }

    std::map<const Bank*, Bank*> kept_banks;
    std::map<const std::vector<uint8_t>*, const std::vector<uint8_t>*> programs;
    auto new_one = banks.begin();
    for (auto old_one = from.banks.begin(); old_one != from.banks.end(); ++old_one, ++new_one)
    {
        kept_banks[&*old_one] = &*new_one;
        for (std::size_t i = 0; i < old_one->programs.size(); i++)
        {
            if (old_one->programs[i])
            {
                programs[&*old_one->programs[i]] = &*new_one->programs[i];
            }
        }
    }

    const auto find = [](const auto& map, auto* pointer)
    {
        return pointer == nullptr ? nullptr : map.at(pointer);
    };
    for (Voice& voice : voices)
    {
        voice.channel = find(channels, voice.channel);
    }
    const auto repoint_sequence = [&](Sequence& sequence)
    {
        for (Channel& channel : sequence.synth.channels)
        {
            channel.bank = find(kept_banks, channel.bank);
            channel.instrument = find(programs, channel.instrument);
            channel.synth = find(synths, channel.synth);
        }
        for (TrackState& track : sequence.tracks)
        {
            track.channel = find(channels, track.channel);
        }
    };
    if (song)
    {
        repoint_sequence(*song);
    }
    for (const OwnedSequence& effect : effects)
    {
        repoint_sequence(*effect);
    }
}

Sequence* Driver::State::Followed() const
{
    return followed == kSong ? song.get() : effects.at(static_cast<std::size_t>(followed)).get();
}

Hang::Hang(int track, uint32_t offset)
    : std::runtime_error("track " + std::to_string(track) + " runs " + std::to_string(kCommandBudget) +
                         " commands in a tick without waiting, up to offset " + xsflib::Hex(offset, 0, true) +
                         ": the sequence hangs"),
      track_(track),
      offset_(offset)
{
}

int Hang::TrackIndex() const
{
    return track_;
}

uint32_t Hang::Offset() const
{
    return offset_;
}

Build BuildOf(std::span<const uint8_t> arm9)
{
    // ldr rT, [rN, #0x14C], then subs rT, rT, #1 within three words, then str rT, [rN, #0x14C].
    const std::size_t words = arm9.size() / 4;
    const auto word = [&](std::size_t i)
    {
        return xsflib::LoadLe32(&arm9[4 * i]);
    };
    for (std::size_t i = 0; i + 4 < words; i++)
    {
        const uint32_t load = word(i);
        if ((load & 0xFFF00FFF) != 0xE590014C)
        {
            continue;
        }
        const uint32_t base = load >> 16 & 0xF;
        const uint32_t value = load >> 12 & 0xF;
        for (std::size_t j = i + 1; j <= i + 3 && j + 1 < words; j++)
        {
            if (word(j) == (0xE2500001 | value << 16 | value << 12) &&
                word(j + 1) == (0xE580014C | base << 16 | value << 12))
            {
                return Build::kEarlier;
            }
        }
    }
    return Build::kLater;
}

Driver::Driver(Build build) : state_(std::make_unique<State>())
{
    State& s = *state_;
    s.build = build;
    s.defaults[7] = 100;
    s.defaults[10] = 0x40;
    s.defaults[11] = 0x7F;
    s.global_volumes.fill(0x7F);
    s.global_volumes[0] = 0;
    for (std::size_t i = 0; i < s.notes.size(); i++)
    {
        s.notes[i].next = i + 1 < s.notes.size() ? static_cast<int>(i + 1) : kNone;
        s.notes[i].duration = -1;
    }
    s.free_notes = 0;
    for (int i = 0; i < kVoices; i++)
    {
        Voice& voice = s.VoiceAt(i);
        voice.index = static_cast<uint16_t>(i);
        voice.bit = static_cast<uint16_t>(1 << i);
    }
    // The pool of the sound effects' sequences (0x0206E920).
    for (int i = 0; i < kEffectSequences; i++)
    {
        s.effects.emplace_back(s.MakeSequence(kEffectTracks, kEffectTracks));
    }
}

Driver::~Driver() = default;

Driver::Driver(const Driver& other) : state_(std::make_unique<State>(*other.state_))
{
    state_->Repoint(*other.state_);
}

Driver& Driver::operator=(const Driver& other)
{
    if (this != &other)
    {
        auto copy = std::make_unique<State>(*other.state_);
        copy->Repoint(*other.state_);
        state_ = std::move(copy);
    }
    return *this;
}

std::optional<uint16_t> Driver::AddBank(const WaveBank& bank, uint32_t samples)
{
    Bank kept;
    kept.id = xsflib::LoadLe16(&bank.header[0x0E]);
    if (state_->BankById(kept.id) != nullptr)
    {
        return std::nullopt;
    }
    kept.file = &bank;
    kept.wave_count = static_cast<int16_t>(xsflib::LoadLe16(&bank.header[0x46]));
    kept.program_count = static_cast<int16_t>(xsflib::LoadLe16(&bank.header[0x48]));
    kept.key_group_count = bank.header[0x4A];
    if (bank.header[0x4B] == 2)
    {
        kept.main_bank = xsflib::LoadLe16(&bank.header[0x40]);
    }
    kept.samples = bank.samples ? samples : 0;
    for (const std::optional<WaveInfo>& wave : bank.waves)
    {
        kept.waves.push_back(wave ? std::optional(wave->bytes) : std::nullopt);
    }
    if (bank.instruments)
    {
        for (const std::optional<Program>& program : bank.instruments->programs)
        {
            if (!program)
            {
                kept.programs.emplace_back();
                continue;
            }
            kept.programs.emplace_back(program->Bytes());
        }
    }
    state_->banks.push_back(std::move(kept));
    return state_->banks.back().id;
}

uint32_t Driver::PlaceWaves(uint16_t id, const WaveBank& main_bank, uint32_t address)
{
    Bank* bank = state_->BankById(id);
    if (bank == nullptr)
    {
        throw std::runtime_error("no bank of that id is loaded");
    }
    uint32_t total = 0;
    uint32_t place = address;
    for (int i = 0; i < bank->wave_count; i++)
    {
        const auto index = static_cast<std::size_t>(i);
        if (index >= bank->waves.size() || !bank->waves[index])
        {
            continue;
        }
        std::array<uint8_t, WaveInfo::kSize>& wave = *bank->waves[index];
        xsflib::StoreLe32(&wave[0x24], place);
        if (index >= main_bank.waves.size() || !main_bank.waves[index])
        {
            continue;
        }
        const uint32_t size = (xsflib::LoadLe32(&wave[0x2C]) + xsflib::LoadLe32(&wave[0x28])) << 2;
        total += size;
        place += size;
    }
    return total;
}

void Driver::PlaySong(const Song& song, const StartParameters& parameters)
{
    State& s = *state_;
    if (s.song)
    {
        s.FreeAllVoices(s.song->synth);
        s.FreeHeldNotes(s.song->synth);
    }
    auto file = std::make_shared<const std::vector<uint8_t>>(song.Write());
    constexpr std::size_t kInfo = Song::kHeaderSize + 16;

    // The driver's load of a song: the sequence, its synth and its tracks (0x0206D808).
    std::unique_ptr<Sequence> sequence = s.MakeSequence((*file)[kInfo + 6], (*file)[kInfo + 7]);
    Sequence& q = *sequence;
    q.id = 0x10000 + uint32_t{xsflib::LoadLe16(&(*file)[0x0E])};
    q.file = std::move(file);
    s.LoadSequence(q, kInfo);
    q.unknown_a = 0;
    s.song = OwnedSequence(std::move(sequence));

    // The start of the song from its beginning (0x0206D9BC). The earlier build then resets the channels again, without
    // a bank (0x02060C04 in Explorers of Time's ARM9 binary).
    s.RestartSequence(q);
    if (s.build == Build::kEarlier)
    {
        s.ResetChannels(q.synth, -1, q.id);
    }
    q.volume.current = 0;
    q.volume.ticks = 0;
    q.muted = 0;
    s.StartSequence(q, 0, 0, parameters);
}

std::optional<uint16_t> Driver::AddEffects(const SoundEffects& effects)
{
    State& s = *state_;
    EffectBank bank;
    bank.id = xsflib::LoadLe16(&effects.header[0x0E]);
    if (s.EffectBankById(bank.id) != nullptr)
    {
        return std::nullopt;
    }
    bank.file = std::make_shared<const std::vector<uint8_t>>(effects.Write());
    const std::vector<uint8_t>& bytes = *bank.file;
    bank.count = static_cast<int16_t>(xsflib::LoadLe16(&bytes[0x30]));
    if (bytes[0x35] != 0)
    {
        // The chunks up to the end chunk: the driver keeps where the seq chunk's data starts.
        std::size_t at = SoundEffects::kHeaderSize;
        while (at + 16 <= bytes.size() && std::memcmp(bytes.data() + at, "eod ", 4) != 0)
        {
            if (std::memcmp(bytes.data() + at, "seq ", 4) == 0)
            {
                bank.sequences = at + 16;
            }
            const uint32_t alignment = bytes[at + 8];
            if (alignment == 0)
            {
                throw std::runtime_error("a chunk's alignment is 0");
            }
            at += (xsflib::LoadLe32(&bytes[at + 12]) + 16 + alignment - 1) & ~(alignment - 1);
        }
    }
    s.effect_banks.push_back(std::move(bank));
    return s.effect_banks.back().id;
}

bool Driver::PlayEffect(uint16_t bank, uint16_t index, const StartParameters& parameters, uint16_t handle)
{
    State& s = *state_;
    const EffectBank* kept = s.EffectBankById(bank);
    if (kept == nullptr)
    {
        return false;
    }
    const std::optional<std::size_t> at = s.EffectAt(*kept, static_cast<int16_t>(index));
    if (!at)
    {
        return false;
    }
    if (*at + SequenceInfo::kSize > kept->file->size())
    {
        throw std::runtime_error("a sound effect's settings lie past the end of its bank");
    }
    const std::span<const uint8_t> settings = std::span(*kept->file).subspan(*at, SequenceInfo::kSize);
    const uint32_t id = bank | uint32_t{index} << 16;
    const uint8_t group = settings[0x1A];
    const int stop = (settings[0x18] & 1) == 0 ? 1 : 0;
    bool refused = false;
    if (parameters.flags & 0x40)
    {
        refused = s.HandlePlays(handle, stop);
    }
    else if (group == 0)
    {
        refused = s.TooManyInstances([id](const Sequence& q) { return q.id == id; }, settings[0x1C], stop);
    }
    else
    {
        refused = s.TooManyInstances([group](const Sequence& q) { return q.group == group; }, settings[0x1C], stop);
    }
    if (refused)
    {
        return false;
    }
    Sequence* found = s.SequenceForEffect(settings[0x1B]);
    if (found == nullptr)
    {
        return false;
    }
    Sequence& q = *found;
    if (q.status[0] == 1)
    {
        s.StopSequence(q, 0, 1);
    }
    q.id = id;
    q.file = kept->file;
    s.ClearTracks(q);
    if (!s.LoadSequence(q, *at))
    {
        return false;
    }
    s.RestartSequence(q);
    q.volume.current = 0;
    q.volume.ticks = 0;
    q.unknown9 = kept->unknown18;
    // The driver's mute (0x0206C688), which the model keeps off, and the bank's byte.
    q.muted = 0;
    q.unknown_a = 1;
    s.StartSequence(q, handle, group, parameters);
    return true;
}

void Driver::StopSong(uint32_t ms)
{
    if (state_->song)
    {
        state_->StopSequence(*state_->song, ms, 0);
    }
}

void Driver::StopEffect(uint16_t bank, uint16_t index, uint16_t handle, uint32_t ms)
{
    const uint32_t id = bank | uint32_t{index} << 16;
    for (const OwnedSequence& q : state_->effects)
    {
        if (q->status[0] == 1 && q->id == id && (handle == 0 || q->handle == handle))
        {
            state_->StopSequence(*q, ms, 1);
        }
    }
}

void Driver::StopHandle(uint16_t handle, uint32_t ms)
{
    for (const OwnedSequence& q : state_->effects)
    {
        if (q->status[0] == 1 && q->handle == handle)
        {
            state_->StopSequence(*q, ms, 1);
        }
    }
}

void Driver::StopEffects(uint32_t ms)
{
    for (const OwnedSequence& q : state_->effects)
    {
        if (q->status[0] == 1)
        {
            state_->StopSequence(*q, ms, 1);
        }
    }
}

ntr::snd::CommandList Driver::Tick(uint16_t channel_status)
{
    State& s = *state_;
    s.total_ticks++;
    if (s.song)
    {
        s.StepSequence(*s.song);
    }
    for (const OwnedSequence& effect : s.effects)
    {
        s.StepSequence(*effect);
    }
    s.StepVoices();
    s.UpdateVoices();
    s.WriteCommands(s.commands);
    s.CheckEndedChannels(channel_status);
    ntr::snd::CommandList list = std::move(s.commands);
    s.commands.clear();
    return list;
}

bool Driver::Playing() const
{
    return state_->song && state_->song->status[0] != 0;
}

int Driver::EffectsPlaying() const
{
    int count = 0;
    for (const OwnedSequence& effect : state_->effects)
    {
        count += effect->status[0] != 0 ? 1 : 0;
    }
    return count;
}

void Driver::ReleaseFollowedNotes()
{
    Sequence* sequence = state_->Followed();
    if (sequence == nullptr)
    {
        return;
    }
    for (Channel& channel : sequence->synth.channels)
    {
        // The earlier build counts down each voice's duration, and the later one each note's. A voice or a note that
        // holds has none to count down.
        state_->ForEachVoice(channel,
                             [](Voice& voice)
                             {
                                 if (voice.duration > 0)
                                 {
                                     voice.duration = 1;
                                 }
                             });
        for (int n = channel.held; n != kNone; n = state_->NoteAt(n).next)
        {
            Note& note = state_->NoteAt(n);
            if (note.duration > 0)
            {
                note.duration = 1;
            }
        }
    }
}

void Driver::Follow(int sequence)
{
    if (sequence != kSong && (sequence < 0 || static_cast<std::size_t>(sequence) >= state_->effects.size()))
    {
        throw std::out_of_range("the pool of sound effects has no sequence " + std::to_string(sequence));
    }
    state_->followed = sequence;
}

void Driver::SetHooks(SequenceHooks hooks)
{
    state_->hooks = std::move(hooks);
}

const SequenceHooks& Driver::GetHooks() const
{
    return state_->hooks;
}

bool Driver::SequencePlays() const
{
    const Sequence* sequence = state_->Followed();
    return sequence && sequence->status[0] != 0;
}

uint32_t Driver::SequenceTick() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? static_cast<uint32_t>(sequence->sequencer_ticks) : 0;
}

uint16_t Driver::SequenceTimebase() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? static_cast<uint16_t>(sequence->ticks_per_eighth << 3) : 0;
}

int32_t Driver::SequenceTempo() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? sequence->microseconds_per_beat : 0;
}

int Driver::ChannelCount() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? static_cast<int>(sequence->synth.channels.size()) : 0;
}

ChannelView Driver::GetChannel(int channel) const
{
    const Sequence* followed = state_->Followed();
    if (followed == nullptr)
    {
        return {};
    }
    const Sequence& sequence = *followed;
    const Channel& c = sequence.synth.channels.at(static_cast<std::size_t>(channel));
    ChannelView view{.bank = c.bank_id,
                     .program = c.program,
                     .has_program = c.instrument != nullptr,
                     .volume = static_cast<uint8_t>(c.volume_fade.current >> 16),
                     .expression = c.expression,
                     .synth_volume = sequence.synth.combined_volume,
                     .pan = c.pan,
                     .bend = c.bend,
                     .key_bend = c.key_bend,
                     .bend_range = c.bend_range,
                     .damper = (c.flags & 1) != 0,
                     .jitter = c.jitter,
                     .random_keys = c.random_high != 0,
                     .lfo_level = c.lfo_level};
    for (std::size_t i = 0; i < c.lfos.size(); i++)
    {
        const LfoSettings& lfo = c.lfos[i];
        view.lfos[i] = {.type = lfo.type,
                        .output = lfo.output,
                        .waveform = lfo.waveform,
                        .amplitude = lfo.amplitude,
                        .phase_ms = lfo.phase_ms,
                        .delay_ms = lfo.delay_ms,
                        .envelope_ms = lfo.envelope_ms};
    }
    if (c.envelope.use != 0)
    {
        const EnvelopeParameters& e = c.envelope;
        view.envelope = {.multiplier = e.multiplier,
                         .attack_begin = e.attack_begin,
                         .attack_time = e.attack_time,
                         .decay_time = e.decay_time,
                         .sustain_level = e.sustain_level,
                         .hold_time = e.hold_time,
                         .sustain_time = e.sustain_time,
                         .release_time = e.release_time};
    }
    return view;
}

int Driver::TrackCount() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? sequence->track_slots : 0;
}

bool Driver::TrackPlays(int track) const
{
    const Sequence* sequence = state_->Followed();
    return sequence && sequence->tracks.at(static_cast<std::size_t>(track)).playing == 1;
}

std::optional<int32_t> EnvelopeTicks(uint8_t multiplier, uint8_t index)
{
    if (index == 0x7F)
    {
        return std::nullopt;
    }
    return SlideTicks(multiplier, index, Driver::kTickMicroseconds);
}

std::vector<uint8_t> EnvelopeLevels(const EnvelopeValues& values, int ticks, std::optional<int> release)
{
    Envelope envelope;
    envelope.parameters.Reset();
    envelope.parameters.use = 1;
    envelope.parameters.multiplier = values.multiplier;
    envelope.parameters.attack_begin = values.attack_begin;
    envelope.parameters.attack_time = values.attack_time;
    envelope.parameters.decay_time = values.decay_time;
    envelope.parameters.sustain_level = values.sustain_level;
    envelope.parameters.hold_time = values.hold_time;
    envelope.parameters.sustain_time = values.sustain_time;
    envelope.parameters.release_time = values.release_time;
    envelope.Start(Driver::kTickMicroseconds);

    std::vector<uint8_t> levels;
    levels.reserve(static_cast<std::size_t>(std::max(ticks, 0)));
    for (int k = 0; k < ticks; k++)
    {
        if (release && k == *release)
        {
            envelope.Release(Driver::kTickMicroseconds);
        }
        levels.push_back(static_cast<uint8_t>(std::max<int8_t>(envelope.Tick(Driver::kTickMicroseconds), 0)));
    }
    return levels;
}

int32_t VoiceTimer(uint16_t pitch, uint8_t format)
{
    const uint8_t place = tables::kKeyPlaces[(pitch >> 8) & 0x7F];
    uint32_t frequency = tables::kSemitoneFrequencies[(pitch & 0xFF) + ((place & 0xF) << 8)];
    const int shift = 4 - (place >> 4);
    frequency = shift >= 0 ? frequency >> shift : frequency << -shift;
    auto divisor = static_cast<int32_t>(frequency);
    if (format >= 3)
    {
        divisor /= 16;
    }
    return Divide(0x00FFB0FF, divisor);
}

VoiceView Driver::GetVoice(int voice) const
{
    const Voice& v = state_->voices.at(static_cast<std::size_t>(voice));
    VoiceView view;
    if (v.channel == nullptr)
    {
        return view;
    }
    view.allocated = true;
    const Sequence* followed = state_->Followed();
    view.followed = followed && v.channel->synth == &followed->synth;
    view.channel = v.channel->index;
    view.track = v.track;
    view.note = v.note;
    view.key = v.key;
    view.started = static_cast<int8_t>(v.envelope.active) == 1;
    view.releasing = v.envelope.state == kRelease || v.envelope.state == kReleaseEnd;
    view.level = static_cast<uint8_t>(v.state & 0xFF);
    const Envelope& envelope = v.envelope;
    if (view.releasing)
    {
        if (envelope.ticks != 0x7FFFFFFF)
        {
            view.release_ticks = envelope.ticks;
        }
    }
    else if (envelope.state != kOff && envelope.parameters.release_time != 0x7F)
    {
        view.release_ticks = SlideTicks(envelope.parameters.multiplier, envelope.parameters.release_time, state_->tick);
    }
    view.key_bend = v.key_bend;
    view.jitter = v.jitter;
    view.wave = v.wave;
    view.key_group = static_cast<uint8_t>(v.channel_key_group & 0xFF);
    view.key_group_voices = v.group_voices;
    return view;
}

std::array<LfoValues, 4> ProgramLfos(const WaveBank& bank, uint16_t program)
{
    std::array<LfoValues, 4> lfos;
    const auto count = static_cast<int16_t>(xsflib::LoadLe16(&bank.header[0x48]));
    if (!bank.instruments || static_cast<int16_t>(program) < 0 || static_cast<int16_t>(program) >= count ||
        program >= bank.instruments->programs.size() || !bank.instruments->programs[program])
    {
        return lfos;
    }

    const std::vector<uint8_t> bytes = bank.instruments->programs[program]->Bytes();
    for (std::size_t k = 0; k < lfos.size(); k++)
    {
        const std::size_t at = 0x10 + 0x10 * k;
        if (at + 16 <= bytes.size() && bytes[at + 1] != 0)
        {
            const LfoSettings lfo = LfoSettings::Read(std::span(bytes).subspan(at, 16));
            lfos[k] = {.type = lfo.type,
                       .output = lfo.output,
                       .waveform = lfo.waveform,
                       .amplitude = lfo.amplitude,
                       .phase_ms = lfo.phase_ms,
                       .delay_ms = lfo.delay_ms,
                       .envelope_ms = lfo.envelope_ms};
        }
    }

    return lfos;
}

} // namespace ntdlib::dse

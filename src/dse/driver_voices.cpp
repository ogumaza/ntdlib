// SPDX-License-Identifier: MIT

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
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/tables.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::dse
{

using ntr::snd::CommandId;

int32_t Driver::State::Random()
{
    auto x = static_cast<int32_t>(seed ^ seed << 17);
    x ^= x >> 15;
    seed = static_cast<uint32_t>(x);
    return x & 0x7FFF;
}

Bank* Driver::State::BankById(uint16_t id)
{
    for (Bank& bank : banks)
    {
        if (bank.id == id)
        {
            return &bank;
        }
    }
    return nullptr;
}

Voice& Driver::State::VoiceAt(int index)
{
    return voices[static_cast<std::size_t>(index)];
}

Note& Driver::State::NoteAt(int index)
{
    return notes[static_cast<std::size_t>(index)];
}

void Driver::State::FreeNote(int node)
{
    Note& note = NoteAt(node);
    note.next = free_notes;
    free_notes = node;
    note.duration = -1;
    note.held = 0;
}

void Driver::State::FlagVoices(Channel& channel, uint16_t flags)
{
    if (build != Build::kLater)
    {
        channel.update |= flags;
        return;
    }
    ForEachVoice(channel, [flags](Voice& voice) { voice.flags |= flags; });
}

void Driver::State::FlagSynth(Synth& synth, uint16_t flags)
{
    if (build != Build::kLater)
    {
        synth.update |= flags;
        return;
    }
    for (Channel& channel : synth.channels)
    {
        FlagVoices(channel, flags);
    }
}

void Driver::State::FreeVoice(Voice& voice)
{
    if (voice.channel != nullptr)
    {
        Channel& channel = *voice.channel;
        const int self = static_cast<int>(&voice - voices.data());
        if (channel.voice_list == self)
        {
            channel.voice_list = voice.next;
        }
        else
        {
            for (int v = channel.voice_list; v != kNone; v = VoiceAt(v).next)
            {
                if (VoiceAt(v).next == self)
                {
                    VoiceAt(v).next = voice.next;
                    break;
                }
            }
        }
    }
    voice.note_off = 0;
    voice.channel = nullptr;
    voice.next = kNone;
    voice.envelope.Stop();
    voice.state &= 0xFF;
    voice.sequence = 0;
    voice.channel_key_group = 0;
    voice.group_voices = 0;
    start_mask &= static_cast<uint16_t>(~voice.bit);
    deallocated_mask |= voice.bit;
}

void Driver::State::MarkStarted(Voice& voice)
{
    voice.flags = 0xFF0;
    voice.state |= 0x1FF;
    stop_mask &= static_cast<uint16_t>(~voice.bit);
    deallocated_mask &= static_cast<uint16_t>(~voice.bit);
    start_mask |= voice.bit;
}

void Driver::State::MarkStopped(Voice& voice)
{
    voice.state &= ~uint32_t{0xFF00};
    start_mask &= static_cast<uint16_t>(~voice.bit);
    stop_mask |= voice.bit;
}

int Driver::State::CountActive(Channel& channel)
{
    int count = 0;
    ForEachVoice(channel,
                 [&count](Voice& voice)
                 {
                     if ((voice.state & 0xFF) != 0)
                     {
                         count++;
                     }
                 });
    return count;
}

Voice* Driver::State::TakeVoice(Channel& channel, const std::array<uint8_t, 8>& group, uint16_t owner, uint8_t priority)
{
    uint32_t threshold = (uint32_t{priority} << 16) + 0x1FF;
    const int high = std::min<int>(group[5], voice_count - 1);
    const int low = group[4];
    switch (channel.allocation)
    {
    case 0:
        if (channel.voice_list != kNone)
        {
            return &VoiceAt(channel.voice_list);
        }
        break;
    case 1:
        break;
    default:
        return nullptr;
    }

    if (group[0] != 0)
    {
        int count = 0;
        int lowest = high;
        uint32_t level = threshold;
        for (int i = low; i <= high; i++)
        {
            Voice& voice = VoiceAt(i);
            if (voice.sequence == channel.sequence && voice.channel_key_group == owner)
            {
                count++;
                if (level > voice.state)
                {
                    level = voice.state;
                    lowest = i;
                }
                if (count >= group[2])
                {
                    break;
                }
            }
        }
        if (count >= group[2])
        {
            Voice& voice = VoiceAt(lowest);
            FreeVoice(voice);
            return &voice;
        }
    }

    int pick = high + 1;
    for (int i = low; i <= high; i++)
    {
        const uint32_t state = VoiceAt(i).state;
        if (threshold > state)
        {
            pick = i;
            if (state == 0)
            {
                break;
            }
            threshold = state;
        }
    }
    if (pick > high)
    {
        return nullptr;
    }
    Voice& voice = VoiceAt(pick);
    FreeVoice(voice);
    return &voice;
}

void Driver::State::StartVoice(Channel& channel, Voice& voice, uint8_t priority)
{
    voice.flags = static_cast<uint16_t>((voice.flags | 1) & ~2);
    voice.state |= (uint32_t{priority} << 16) + 0x1FF;
    voice.envelope.Start(tick);
    if (voice.channel != &channel)
    {
        voice.next = channel.voice_list;
        channel.voice_list = static_cast<int>(&voice - voices.data());
        voice.channel = &channel;
    }
}

void Driver::State::ReleaseVoice(Channel& channel, Voice& voice)
{
    voice.note_off |= 2;
    if (channel.flags & 1)
    {
        return;
    }
    voice.state &= 0xFF;
    voice.envelope.Release(tick);
    voice.flags &= static_cast<uint16_t>(~1);
    if (hooks.on_release && Followed() != nullptr && channel.synth == &Followed()->synth)
    {
        hooks.on_release(static_cast<int>(&voice - voices.data()));
    }
}

void Driver::State::ReleaseVoices(Channel& channel, uint8_t key)
{
    ForEachVoice(channel,
                 [&](Voice& voice)
                 {
                     if (voice.key == key)
                     {
                         ReleaseVoice(channel, voice);
                     }
                 });
}

void Driver::State::RandomKey(const Channel& channel, Note& note)
{
    if (channel.random_high != 0)
    {
        const int32_t span = channel.random_high - channel.random_low + 1;
        const int32_t product = Wrap(int64_t{span} * Random());
        note.key = static_cast<uint8_t>(channel.random_low + product / 32768);
    }
}

int Driver::State::HoldNote(Channel& channel, Note& note)
{
    RandomKey(channel, note);
    const int node = free_notes;
    if (node == kNone)
    {
        return kNone;
    }
    free_notes = NoteAt(node).next;
    Note& held = NoteAt(node);
    held.on = note.on;
    held.held = 1;
    held.key = note.key;
    held.velocity = note.velocity;
    held.duration = note.duration;
    held.next = channel.held;
    channel.held = node;
    return node;
}

void Driver::State::ReleaseHeldNote(Channel& channel, int node)
{
    const uint8_t key = NoteAt(node).key;
    int* link = &channel.held;
    int found = kNone;
    for (int n = channel.held; n != kNone; n = NoteAt(n).next)
    {
        if (NoteAt(n).key == key)
        {
            ReleaseVoices(channel, key);
            *link = NoteAt(n).next;
            found = n;
            break;
        }
        link = &NoteAt(n).next;
    }
    if (found != kNone)
    {
        FreeNote(found);
    }
}

void Driver::State::Legato(Channel& channel, const Note& note, uint8_t previous)
{
    if (!ListsNotes())
    {
        for (int v = channel.voice_list; v != kNone; v = VoiceAt(v).next)
        {
            Voice& voice = VoiceAt(v);
            if (voice.key == previous)
            {
                voice.key = note.key;
                voice.duration = note.duration;
                voice.note_key = static_cast<int16_t>(voice.key_base + (note.key << 8));
                voice.flags |= 0x10;
                return;
            }
        }
        return;
    }
    for (int n = channel.held; n != kNone; n = NoteAt(n).next)
    {
        Note& held = NoteAt(n);
        if (held.key == previous)
        {
            const int next = held.next;
            held = note;
            held.held = 1;
            held.next = next;
            break;
        }
    }
    ForEachVoice(channel,
                 [&](Voice& voice)
                 {
                     if (voice.key == previous)
                     {
                         voice.key = note.key;
                         voice.note_key = static_cast<int16_t>(voice.key_base + (note.key << 8));
                         voice.flags |= 0x10;
                     }
                 });
}

void Driver::State::ReleaseKey(Channel& channel, uint8_t key)
{
    for (int n = channel.held; n != kNone; n = NoteAt(n).next)
    {
        if (NoteAt(n).key == key)
        {
            ReleaseHeldNote(channel, n);
            return;
        }
    }
}

void Driver::State::ReleaseEndedNotes(Channel& channel)
{
    if (channel.flags & 3)
    {
        return;
    }
    int* link = &channel.held;
    for (int n = channel.held; n != kNone;)
    {
        Note& note = NoteAt(n);
        const int next = note.next;
        if (note.duration > 0)
        {
            *link = n;
            link = &note.next;
        }
        else
        {
            throw std::runtime_error("the damper frees an ended note twice, and the driver's lists break");
        }
        n = next;
    }
    *link = kNone;
}

void Driver::State::ReleaseDampedVoices(Channel& channel)
{
    if (channel.flags & 1)
    {
        return;
    }
    ForEachVoice(channel,
                 [&](Voice& voice)
                 {
                     if (voice.note_off & 2)
                     {
                         ReleaseVoice(channel, voice);
                     }
                 });
}

std::optional<SplitView> Driver::State::SplitAt(const std::vector<uint8_t>& program, std::size_t at)
{
    if (at + SplitView::kSize > program.size())
    {
        return std::nullopt;
    }
    return SplitView(std::span(program).subspan(at).first<SplitView::kSize>());
}

std::optional<std::size_t> Driver::State::NextSplit(const std::vector<uint8_t>& program,
                                                    std::optional<std::size_t> previous, uint8_t key, int8_t velocity)
{
    const uint8_t count = program[2];
    if (count == 0)
    {
        return std::nullopt;
    }
    std::size_t at = 0x60;
    if (previous)
    {
        const std::optional<SplitView> split = SplitAt(program, *previous);
        if (!split || split->Index() >= count - 1)
        {
            return std::nullopt;
        }
        at = *previous + SplitView::kSize;
    }
    while (true)
    {
        const std::optional<SplitView> split = SplitAt(program, at);
        if (!split)
        {
            return std::nullopt;
        }
        if (key >= split->LowKey() && key <= split->HighKey() && velocity >= split->LowVelocity() &&
            velocity <= split->HighVelocity())
        {
            return at;
        }
        if (split->Index() >= count - 1)
        {
            return std::nullopt;
        }
        at += SplitView::kSize;
    }
}

uint16_t Driver::State::StartNote(Channel& channel, const Note& given)
{
    const uint32_t number = ++notes_started;
    uint16_t started = 0;
    if (!ListsNotes() && channel.instrument == nullptr)
    {
        return 0;
    }
    int16_t jitter = 0;
    if (channel.jitter != 0)
    {
        const int32_t product = Wrap(int64_t{channel.jitter} * 2 * Random());
        jitter = static_cast<int16_t>(product / 32768 - channel.jitter);
    }
    Note note = given;
    if (!ListsNotes())
    {
        RandomKey(channel, note);
    }
    if (channel.instrument == nullptr || channel.bank == nullptr)
    {
        return 0;
    }
    const std::vector<uint8_t>& program = *channel.instrument;
    Bank& bank = *channel.bank;
    std::optional<std::size_t> at;
    while ((at = NextSplit(program, at, note.key, static_cast<int8_t>(note.velocity))))
    {
        const SplitView split = *SplitAt(program, *at);
        std::size_t group_index = split.KeyGroup();
        if (group_index >= bank.key_group_count)
        {
            group_index = 0;
        }
        std::array<uint8_t, 8> group = {};
        if (bank.file->instruments && group_index < bank.file->instruments->key_groups.size())
        {
            group = bank.file->instruments->key_groups[group_index];
        }
        const auto wave_index = static_cast<int16_t>(split.Wave());
        if (wave_index < 0 || wave_index >= bank.wave_count ||
            static_cast<std::size_t>(wave_index) >= bank.waves.size() ||
            !bank.waves[static_cast<std::size_t>(wave_index)])
        {
            continue;
        }
        const std::array<uint8_t, WaveInfo::kSize>& wave = *bank.waves[static_cast<std::size_t>(wave_index)];
        const auto owner = static_cast<uint16_t>(split.KeyGroup() + (channel.index << 8));
        Voice* found = TakeVoice(channel, group, owner, group[3]);
        if (found == nullptr)
        {
            continue;
        }
        Voice& voice = *found;
        voice.note_off = 0;
        voice.sequence = channel.sequence;
        voice.channel_key_group = owner;
        voice.group_voices = group[0] != 0 ? group[2] : 0;
        voice.wave = static_cast<int8_t>(wave_index);
        std::copy_n(wave.begin() + 0x10, 0x20, voice.sample.begin());
        xsflib::StoreLe32(&voice.sample[0x14], voice.Address() + bank.samples);
        voice.key = note.key;
        voice.key_base = static_cast<int16_t>(split.Tuning() + (split.CoarseTuning() << 8) + jitter);
        voice.note_key = static_cast<int16_t>(voice.key_base + (note.key << 8));
        voice.jitter = jitter;
        voice.wave_pan = static_cast<int8_t>(split.Pan());
        if (velocity_mode == 7)
        {
            const int32_t product = channel.optional_volume * note.velocity * program[4] * split.Volume();
            voice.note_volume = static_cast<uint8_t>(product / (127 * 127 * 127));
        }
        else
        {
            const int32_t product = note.velocity * program[4] * split.Volume();
            voice.note_volume = static_cast<uint8_t>(product / (127 * 127));
        }
        const uint8_t range = channel.bend_range != 0 ? channel.bend_range : split.BendRange();
        voice.key_bend = static_cast<int16_t>(Wrap(int64_t{range} * (channel.key_bend << 8)) / 8192);
        voice.bend_range = range;
        voice.duration = note.duration;
        voice.flags |= 0xFF0;
        StartLfos(voice.lfos, channel.lfos, channel.lfo_level);
        voice.envelope.parameters = EnvelopeParameters::Read(split.Envelope());
        voice.envelope.parameters.use = 1;
        // The oldest build plays a multiplier of 16 or more as 1 (0x020C44B4 in Luminous Arc's ARM9 binary).
        if (build == Build::kOldest && voice.envelope.parameters.multiplier >= 0x10)
        {
            voice.envelope.parameters.multiplier = 1;
        }
        if (channel.envelope.use != 0)
        {
            SetEnvelopeOverride(voice.envelope.parameters, channel.envelope);
        }
        StartVoice(channel, voice, group[3]);
        voice.note = number;
        voice.track = -1;
        started |= voice.bit;
    }
    return started;
}

void Driver::State::SetEnvelopeOverride(EnvelopeParameters& parameters, const EnvelopeParameters& override)
{
    const auto set = [](uint8_t& value, uint8_t given)
    {
        if (given <= 0x7F)
        {
            value = given;
        }
    };
    set(parameters.multiplier, override.multiplier);
    set(parameters.attack_begin, override.attack_begin);
    set(parameters.attack_time, override.attack_time);
    set(parameters.decay_time, override.decay_time);
    set(parameters.sustain_level, override.sustain_level);
    set(parameters.hold_time, override.hold_time);
    set(parameters.sustain_time, override.sustain_time);
    set(parameters.release_time, override.release_time);
}

void Driver::State::FreeVoices(Channel& channel)
{
    uint32_t freed = 0;
    while (channel.voice_list != kNone)
    {
        Voice& voice = VoiceAt(channel.voice_list);
        freed |= voice.bit;
        FreeVoice(voice);
    }
    if (build == Build::kEarlier)
    {
        commands.push_back({CommandId::kStopTimer, {freed, 0, 0, 0}});
    }
}

void Driver::State::SelectProgram(Channel& channel, uint16_t index)
{
    if (channel.bank == nullptr)
    {
        channel.instrument = nullptr;
        channel.program = 0;
    }
    else
    {
        const auto signed_index = static_cast<int16_t>(index);
        channel.program = static_cast<uint16_t>(signed_index);
        const Bank& bank = *channel.bank;
        const std::vector<uint8_t>* program = nullptr;
        if (signed_index < bank.program_count && signed_index >= 0 &&
            static_cast<std::size_t>(signed_index) < bank.programs.size() &&
            bank.programs[static_cast<std::size_t>(signed_index)])
        {
            program = &*bank.programs[static_cast<std::size_t>(signed_index)];
        }
        if (program != nullptr)
        {
            // The earlier and middle builds keep the bend range (0x020647F8 in Explorers of Time's ARM9 binary). The
            // late middle build clears it (0x0212BD0C in Shiren DS 2's ARM9 binary).
            if (build == Build::kLater || build == Build::kLateMiddle)
            {
                channel.bend_range = 0;
            }
            for (std::size_t k = 0; k < 4; k++)
            {
                const std::size_t at = 0x10 + 0x10 * k;
                if (at + 16 <= program->size() && TakesLfo(build, std::span(*program).subspan(at, 16)))
                {
                    channel.lfos[k] = LfoSettings::Read(std::span(*program).subspan(at, 16));
                }
                else
                {
                    channel.lfos[k].type = 0;
                }
            }
        }
        channel.instrument = program;
    }
    channel.envelope.Reset();
}

void Driver::State::SelectBank(Channel& channel, uint16_t id)
{
    channel.bank_id = id;
    channel.bank = BankById(id);
    if (channel.bank == nullptr)
    {
        channel.instrument = nullptr;
        return;
    }
    SelectProgram(channel, channel.program);
}

void Driver::State::ApplyKeyBend(Channel& channel, int16_t value)
{
    channel.key_bend = value;
    // The middle builds also mark the channel's pitch (0x0207611C in Luminous Arc 2's ARM9 binary).
    if (build == Build::kMiddle || build == Build::kLateMiddle)
    {
        channel.update |= 0x10;
    }
    ForEachVoice(channel,
                 [value](Voice& voice)
                 {
                     voice.key_bend = static_cast<int16_t>(Wrap(int64_t{voice.bend_range} * (value << 8)) / 8192);
                     voice.flags |= 0x10;
                 });
}

void Driver::State::ResetChannel(Channel& channel, uint32_t id)
{
    channel.sequence = id;
    channel.flags = 0;
    channel.unknown6 = 0;
    channel.allocation = 1;
    channel.unknown8 = 0;
    channel.voices = static_cast<int8_t>(voice_count);
    channel.jitter = 0;
    channel.random_low = 0;
    channel.random_high = 0;
    channel.bank_id =
        static_cast<uint16_t>(static_cast<int8_t>(defaults[32]) + (static_cast<int8_t>(defaults[0]) << 8));
    channel.program = 0;
    channel.bend_fade.current = 0;
    channel.bend_fade.ticks = 0;
    channel.volume_fade.current = defaults[7] << 16;
    channel.volume_fade.ticks = 0;
    channel.pan_fade.current = defaults[10] << 16;
    channel.pan_fade.ticks = 0;
    channel.tuning = 0;
    channel.expression = defaults[11];
    channel.lfo_level = static_cast<int8_t>(defaults[1]);
    channel.key_bend = 0;
    channel.unknown56 = 0;
    channel.optional_volume = 0x7F;
    channel.bend_range = 0;
    std::fill_n(channel.unknown5a.begin(), 7, uint8_t{0});
    for (LfoSettings& lfo : channel.lfos)
    {
        lfo.type = 0;
        lfo.output = 0;
    }
    channel.envelope.Reset();
    channel.voice_list = kNone;
    for (int n = channel.held; n != kNone;)
    {
        const int next = NoteAt(n).next;
        FreeNote(n);
        n = next;
    }
    channel.held = kNone;
    channel.bank = nullptr;
    channel.instrument = nullptr;
    channel.bend = 0;
    channel.volume = static_cast<uint16_t>(defaults[7] * defaults[11] * 127 / (127 * 127));
    channel.pan = 0x40;
}

void Driver::State::ChannelVolumes(Synth& synth)
{
    for (Channel& channel : synth.channels)
    {
        channel.volume = static_cast<uint16_t>(
            synth.combined_volume * ((channel.volume_fade.current >> 16) * channel.expression) / (127 * 127));
    }
}

void Driver::State::ResetSynth(Synth& synth)
{
    synth.global_volume = global_volumes[1];
    synth.global_volume_index = 1;
    synth.bend = 0;
    synth.volume = 0x7F;
    synth.pan = 0x40;
    synth.combined_volume = static_cast<int8_t>(synth.global_volume * synth.volume / 127);
    synth.paused = 0;
    synth.unknown_a = 0;
    synth.voices = static_cast<uint8_t>(voice_count);
}

void Driver::State::SelectGlobalVolume(Synth& synth, uint8_t index, int flag)
{
    synth.global_volume = global_volumes[index & 0xF];
    synth.global_volume_index = static_cast<int8_t>(index);
    synth.combined_volume = static_cast<int8_t>(synth.global_volume * synth.volume / 127);
    if (build == Build::kLater)
    {
        ChannelVolumes(synth);
    }
    if (flag == 1)
    {
        FlagSynth(synth, 0x20);
    }
}

void Driver::State::SetSynthBend(Synth& synth, int16_t bend)
{
    synth.bend = bend;
    if (build == Build::kLater)
    {
        for (Channel& channel : synth.channels)
        {
            channel.bend = static_cast<uint16_t>(synth.bend + channel.tuning +
                                                 static_cast<int16_t>(channel.bend_fade.current >> 16));
        }
    }
    FlagSynth(synth, 0x10);
}

void Driver::State::SetSynthVolume(Synth& synth, int8_t volume)
{
    synth.volume = volume;
    synth.combined_volume = static_cast<int8_t>(synth.global_volume * synth.volume / 127);
    if (build == Build::kLater)
    {
        ChannelVolumes(synth);
    }
    FlagSynth(synth, 0x20);
}

void Driver::State::SetSynthPan(Synth& synth, uint8_t pan)
{
    synth.pan = static_cast<int8_t>(pan);
    if (build == Build::kLater)
    {
        for (Channel& channel : synth.channels)
        {
            channel.pan = static_cast<int16_t>(synth.pan - 0x40 + ((channel.pan_fade.current >> 16) & 0xFF));
        }
    }
    FlagSynth(synth, 0x40);
}

void Driver::State::ResetChannels(Synth& synth, int bank, uint32_t id)
{
    for (Channel& channel : synth.channels)
    {
        FreeVoices(channel);
        ResetChannel(channel, id);
        if (bank >= 0)
        {
            SelectBank(channel, static_cast<uint16_t>(bank));
            SelectProgram(channel, static_cast<uint16_t>(bank));
        }
    }
}

void Driver::State::FreeHeldNotes(Synth& synth)
{
    for (Channel& channel : synth.channels)
    {
        for (int n = channel.held; n != kNone;)
        {
            const int next = NoteAt(n).next;
            FreeNote(n);
            n = next;
        }
        channel.held = kNone;
    }
}

void Driver::State::FreeAllVoices(Synth& synth)
{
    for (Channel& channel : synth.channels)
    {
        FreeVoices(channel);
    }
}

void Driver::State::SilenceVoices(Synth& synth, uint8_t flag)
{
    synth.paused = flag;
    for (Channel& channel : synth.channels)
    {
        if (flag == 1)
        {
            channel.flags |= 0x40;
            ForEachVoice(channel,
                         [](Voice& voice)
                         {
                             voice.timer = 0;
                             voice.volume = 0;
                             voice.flags |= 0x30;
                         });
        }
        else
        {
            FlagVoices(channel, 0x30);
            channel.flags &= static_cast<uint8_t>(~0x40);
        }
    }
}

void Driver::State::ReplayHeldNotes(Synth& synth)
{
    for (Channel& channel : synth.channels)
    {
        for (int n = channel.held; n != kNone; n = NoteAt(n).next)
        {
            StartNote(channel, NoteAt(n));
        }
    }
}

} // namespace ntdlib::dse

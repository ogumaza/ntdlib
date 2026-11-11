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

uint8_t Driver::State::Byte(Sequence& sequence, TrackState& track, uint32_t at)
{
    const std::vector<uint8_t>& bytes = *sequence.file;
    if (at >= bytes.size())
    {
        // The driver reads on into the memory after the file. The model stops the track there.
        track.playing = 0;
        return kEndTrack;
    }
    return bytes[at];
}

void Driver::State::RestartSequence(Sequence& sequence)
{
    ResetSynth(sequence.synth);
    ResetChannels(sequence.synth, 0, sequence.id);
    SelectGlobalVolume(sequence.synth, sequence.global_volume_index, 0);
    sequence.status = {};
    sequence.looped = 0;
    sequence.unknown9 = 1;
    sequence.tracks_playing = 0;
    sequence.reset_ticks = total_ticks;
    sequence.driver_ticks = 0;
    sequence.sequencer_ticks = 0;
    sequence.bpm = 120;
    sequence.accumulator = 0;
    sequence.step = Wrap(int64_t{sequence.ticks_per_eighth << 3} * tick);
    sequence.microseconds_per_beat = 500'000;
    sequence.fades = 0;
    sequence.tempo.current = 0x1000000;
    sequence.tempo.ticks = 0;
    sequence.bend.current = 0;
    sequence.bend.ticks = 0;
    sequence.volume.current = 0x7F0000;
    sequence.volume.ticks = 0;
    sequence.pan.current = 0x400000;
    sequence.pan.ticks = 0;
    sequence.volume_target = 0x7F;
    for (int i = 0; i < sequence.track_slots; i++)
    {
        TrackState& track = sequence.tracks[static_cast<std::size_t>(i)];
        if (track.data != 0)
        {
            track.playing = 1;
            track.loops = 0;
            track.position = track.data;
            track.wait = 0;
            track.previous_length = 0;
            track.multiplier = 0x7F;
            track.octave = 4;
            track.loop_index = 0;
            track.loop_start = 0;
            if (track.channel_number >= sequence.synth.channels.size())
            {
                throw std::runtime_error("a track plays on a channel that the song doesn't have");
            }
            track.channel = &sequence.synth.channels[track.channel_number];
        }
        sequence.tracks_playing++;
    }
    sequence.initialized = 1;
}

uint16_t Driver::State::FadeTicks(uint32_t ms) const
{
    if (ms == 0)
    {
        return 0;
    }
    const auto ticks =
        static_cast<uint16_t>(DivideUnsigned(ms * 1000u, static_cast<uint32_t>(static_cast<int32_t>(tick))));
    return ticks == 0 ? uint16_t{1} : ticks;
}

uint16_t Driver::State::SetFade(Fade& fade, int32_t value, uint16_t ticks)
{
    fade.target = value;
    uint16_t stored = ticks;
    if (ticks == 0)
    {
        fade.current = value;
    }
    else
    {
        const int32_t difference = Wrap(int64_t{value} - fade.current);
        if (difference == 0)
        {
            stored = 0;
        }
        else
        {
            fade.delta = Divide(difference, ticks);
        }
    }
    fade.ticks = stored;
    return ticks;
}

void Driver::State::StartFades(Sequence& sequence, const StartParameters& parameters)
{
    const uint8_t flags = parameters.flags;
    if (flags & 1)
    {
        const int16_t tempo = parameters.tempo != 0 ? parameters.tempo : int16_t{0x100};
        if (SetFade(sequence.tempo, tempo * 65536, FadeTicks(parameters.tempo_ms)) != 0)
        {
            sequence.fades |= 1;
        }
        else
        {
            const auto beat = static_cast<int32_t>(static_cast<int16_t>(sequence.tempo.current >> 16) *
                                                   static_cast<int16_t>(sequence.bpm));
            uint32_t eighths = static_cast<uint32_t>(beat) >> 8;
            if (eighths == 0)
            {
                eighths = 1;
            }
            sequence.microseconds_per_beat = static_cast<int32_t>(60'000'000u / eighths);
        }
    }
    if (flags & 2)
    {
        if (SetFade(sequence.bend, parameters.bend * 65536, FadeTicks(parameters.bend_ms)) != 0)
        {
            sequence.fades |= 2;
        }
        else
        {
            SetSynthBend(sequence.synth, static_cast<int16_t>(sequence.bend.current >> 16));
        }
    }
    if (flags & 4)
    {
        int16_t volume = parameters.volume;
        int16_t ms = static_cast<int16_t>(parameters.volume_ms);
        sequence.volume_target = static_cast<int8_t>(volume);
        if (sequence.muted == 1)
        {
            volume = 0;
            ms = 0;
        }
        const int8_t value = static_cast<int8_t>(volume);
        if (SetFade(sequence.volume, value * 65536, FadeTicks(static_cast<uint16_t>(ms))) != 0)
        {
            sequence.fades |= 4;
        }
        else
        {
            SetSynthVolume(sequence.synth, value);
        }
    }
    if (flags & 8)
    {
        const auto pan = static_cast<int8_t>(parameters.pan);
        if (SetFade(sequence.pan, pan * 65536, FadeTicks(parameters.pan_ms)) != 0)
        {
            sequence.fades |= 0x10;
        }
        else
        {
            SetSynthPan(sequence.synth, static_cast<uint8_t>(sequence.pan.current >> 16));
        }
    }
    if (sequence.delay < 0)
    {
        sequence.delay = 0;
    }
}

void Driver::State::StartSequence(Sequence& sequence, uint16_t handle, uint8_t group, const StartParameters& parameters)
{
    sequence.handle = handle;
    sequence.group = group;
    sequence.delay =
        parameters.delay_ms < 0 ? int16_t{-1} : static_cast<int16_t>(Divide(parameters.delay_ms * 1000, tick));
    sequence.status[3] = 0;
    StartFades(sequence, parameters);
    if (sequence.muted == 0)
    {
        ReplayHeldNotes(sequence.synth);
        sequence.status[1] = 1;
    }
    sequence.status[0] = 1;
}

void Driver::State::StopSequence(Sequence& sequence, uint32_t ms, int clear)
{
    if (sequence.stopped == 1 || sequence.status[0] == 0)
    {
        return;
    }
    if (ms != 0 && (sequence.volume.current >> 16) != 0)
    {
        sequence.status[3] = 1;
        sequence.volume_target = 0;
        if (SetFade(sequence.volume, 0, FadeTicks(ms)) != 0)
        {
            sequence.fades |= 4;
        }
        else
        {
            SetSynthVolume(sequence.synth, 0);
        }
        return;
    }
    sequence.status = {};
    sequence.muted = 0;
    FreeAllVoices(sequence.synth);
    if (clear == 1)
    {
        FreeHeldNotes(sequence.synth);
    }
}

void Driver::State::ClearTracks(Sequence& sequence)
{
    for (int i = 0; i < sequence.track_slots; i++)
    {
        TrackState& track = sequence.tracks[static_cast<std::size_t>(i)];
        track.playing = 0;
        track.preamble = 0;
        track.channel_number = 0;
        track.loops = 0;
        track.data = 0;
        track.position = 0;
        track.loop_start = 0;
        track.channel = nullptr;
    }
}

std::unique_ptr<Sequence> Driver::State::MakeSequence(uint8_t tracks, uint8_t channels)
{
    auto sequence = std::make_unique<Sequence>();
    Sequence& q = *sequence;
    q.ticks_per_eighth = 6;
    q.track_count = static_cast<int8_t>(tracks);
    q.track_slots = static_cast<int8_t>(tracks);
    q.channel_count = static_cast<int8_t>(channels);
    q.tracks.resize(tracks);
    q.synth.channel_count = channels;
    q.synth.channels.resize(channels);
    for (std::size_t i = 0; i < q.synth.channels.size(); i++)
    {
        q.synth.channels[i].index = static_cast<uint8_t>(i);
        q.synth.channels[i].synth = &q.synth;
    }
    ResetSynth(q.synth);
    q.bpm = 120;
    q.step = Wrap(int64_t{q.ticks_per_eighth << 3} * tick);
    q.microseconds_per_beat = 500'000;
    ClearTracks(q);
    return sequence;
}

bool Driver::State::LoadSequence(Sequence& sequence, std::size_t info)
{
    const std::vector<uint8_t>& bytes = *sequence.file;
    if (info + SequenceInfo::kSize > bytes.size())
    {
        throw std::runtime_error("a sequence's settings lie past the end of its file");
    }
    const std::span<const uint8_t> settings = std::span(bytes).subspan(info, SequenceInfo::kSize);
    if (settings[6] > static_cast<uint8_t>(sequence.track_count))
    {
        return false;
    }
    sequence.ticks_per_eighth = static_cast<uint8_t>(xsflib::LoadLe16(&settings[2]) >> 3);
    sequence.unknown11 = settings[4];
    sequence.track_slots = static_cast<int8_t>(settings[6]);
    sequence.channel_count = static_cast<int8_t>(settings[7]);
    sequence.priority = settings[0x1B];
    sequence.global_volume_index = settings[0x19];
    sequence.group = settings[0x1A];
    sequence.unknown_b = (settings[0x18] & 0x80) ? 1 : 0;
    std::size_t at = info + SequenceInfo::kSize;
    std::size_t slot = 0;
    while (true)
    {
        if (at + 16 > bytes.size())
        {
            throw std::runtime_error("a sequence's chunks run past the end of its file");
        }
        if (std::memcmp(bytes.data() + at, "eoc ", 4) == 0)
        {
            return true;
        }
        if (std::memcmp(bytes.data() + at, "trk ", 4) == 0)
        {
            if (slot >= sequence.tracks.size())
            {
                // The driver writes the track past those of the sequence.
                throw std::runtime_error("a sequence has more track chunks than its settings give");
            }
            TrackState& track = sequence.tracks[slot++];
            track.data = static_cast<uint32_t>(at + 0x14);
            track.preamble = bytes[at + 0x10];
            track.channel_number = bytes[at + 0x11];
        }
        const uint32_t alignment = bytes[at + 8];
        if (alignment == 0)
        {
            // The driver's walk of the chunks then stays on the chunk.
            throw std::runtime_error("a chunk's alignment is 0");
        }
        at += (xsflib::LoadLe32(&bytes[at + 12]) + 16 + alignment - 1) & ~(alignment - 1);
    }
}

EffectBank* Driver::State::EffectBankById(uint16_t id)
{
    for (EffectBank& bank : effect_banks)
    {
        if (bank.id == id)
        {
            return &bank;
        }
    }
    return nullptr;
}

std::optional<std::size_t> Driver::State::EffectAt(const EffectBank& bank, int16_t index)
{
    if (index < 0 || bank.count <= index)
    {
        return std::nullopt;
    }
    const std::vector<uint8_t>& bytes = *bank.file;
    const bool chunks = bytes[0x35] != 0;
    const std::size_t table = chunks ? bank.sequences : 0x40;
    const std::size_t base = chunks ? bank.sequences : 0;
    const std::size_t entry = table + 2 * static_cast<std::size_t>(index);
    if (entry + 2 > bytes.size())
    {
        throw std::runtime_error("a bank's table of sound effects runs past the end of its file");
    }
    const uint16_t offset = xsflib::LoadLe16(&bytes[entry]);
    if (offset == 0)
    {
        return std::nullopt;
    }
    return base + offset;
}

bool Driver::State::HandlePlays(uint16_t handle, int stop)
{
    for (const OwnedSequence& q : effects)
    {
        if (q->status[0] == 1 && q->handle == handle)
        {
            if (stop != 1)
            {
                return true;
            }
            StopSequence(*q, 0, 1);
        }
    }
    return false;
}

Sequence* Driver::State::SequenceForEffect(uint8_t priority)
{
    Sequence* found = nullptr;
    uint32_t lowest = 0xFFFFFFFF;
    for (const OwnedSequence& q : effects)
    {
        if (q->status[0] == 0)
        {
            return q.get();
        }
        if (q->priority >= priority && lowest > static_cast<uint32_t>(q->reset_ticks))
        {
            lowest = static_cast<uint32_t>(q->reset_ticks);
            found = q.get();
        }
    }
    return found;
}

void Driver::State::StepFades(Sequence& sequence)
{
    const auto step = [](Fade& fade)
    {
        fade.ticks--;
        fade.current = fade.ticks == 0 ? fade.target : Wrap(int64_t{fade.current} + fade.delta);
    };
    if (sequence.tempo.ticks != 0)
    {
        step(sequence.tempo);
        uint32_t beat = static_cast<uint32_t>((sequence.tempo.current >> 16) * sequence.bpm) >> 8;
        if (beat == 0)
        {
            beat = 1;
        }
        sequence.microseconds_per_beat = static_cast<int32_t>(60'000'000u / beat);
        if (sequence.tempo.ticks == 0)
        {
            sequence.fades &= static_cast<uint8_t>(~1);
        }
    }
    if (sequence.bend.ticks != 0)
    {
        step(sequence.bend);
        SetSynthBend(sequence.synth, static_cast<int16_t>(sequence.bend.current >> 16));
        if (sequence.bend.ticks == 0)
        {
            sequence.fades &= static_cast<uint8_t>(~2);
        }
    }
    if (sequence.volume.ticks != 0)
    {
        step(sequence.volume);
        SetSynthVolume(sequence.synth, static_cast<int8_t>(sequence.volume.current >> 16));
        if (sequence.volume.ticks == 0)
        {
            sequence.fades &= static_cast<uint8_t>(~4);
        }
    }
    if (sequence.pan.ticks != 0)
    {
        step(sequence.pan);
        SetSynthPan(sequence.synth, static_cast<uint8_t>(sequence.pan.current >> 16));
        if (sequence.pan.ticks == 0)
        {
            sequence.fades &= static_cast<uint8_t>(~0x10);
        }
    }
}

void Driver::State::StepNoteLengths(Sequence& sequence)
{
    if (build == Build::kEarlier)
    {
        for (Channel& channel : sequence.synth.channels)
        {
            ForEachVoice(channel,
                         [&](Voice& voice)
                         {
                             if (!(channel.flags & 2) && voice.duration > 0 && --voice.duration == 0)
                             {
                                 ReleaseVoice(channel, voice);
                             }
                         });
        }
        return;
    }
    for (Channel& channel : sequence.synth.channels)
    {
        int* link = &channel.held;
        for (int n = channel.held; n != kNone;)
        {
            Note& note = NoteAt(n);
            const int next = note.next;
            int32_t left = note.duration - 1;
            if (left <= 0)
            {
                if (channel.flags & 3)
                {
                    left = 0;
                }
                else
                {
                    ReleaseVoices(channel, note.key);
                    FreeNote(n);
                    left = -1;
                }
            }
            note.duration = left;
            if (left >= 0)
            {
                *link = n;
                link = &note.next;
            }
            n = next;
        }
        *link = kNone;
    }
}

void Driver::State::RunTrack(Sequence& sequence, TrackState& track)
{
    Channel& channel = *track.channel;
    uint32_t at = track.position;
    if (track.wait == 0)
    {
        for (int run = 0;; run++)
        {
            if (run == kCommandBudget)
            {
                throw Hang(static_cast<int>(&track - sequence.tracks.data()), at);
            }
            const uint32_t start = at;
            const uint8_t code = Byte(sequence, track, at++);
            if (track.playing == 0)
            {
                break;
            }
            if (hooks.on_event && &sequence == Followed())
            {
                const std::vector<uint8_t>& bytes = *sequence.file;
                const int arguments = ArgumentBytes(code, at < bytes.size() ? bytes[at] : 0);
                hooks.on_event({.track = static_cast<int>(&track - sequence.tracks.data()),
                                .offset = start,
                                .next = at + static_cast<uint32_t>(std::max(arguments, 0)),
                                .code = code,
                                .repeats = track.loop_index});
            }
            if (code >= 0x90)
            {
                RunEvent(sequence, track, code, at);
            }
            else if (code >= 0x80)
            {
                track.previous_wait = tables::kPauseLengths[code - 0x80];
                track.wait = track.previous_wait;
            }
            else
            {
                Note note;
                note.velocity = code;
                const uint8_t b = Byte(sequence, track, at);
                track.octave = static_cast<uint8_t>(static_cast<int8_t>(track.octave + (((b >> 4) & 3) - 2)));
                note.key = static_cast<uint8_t>(static_cast<int8_t>(track.octave) * 12 + (b & 0xF));
                int count = b >> 6;
                at++;
                uint32_t length = track.previous_length;
                if (count != 0)
                {
                    length = 0;
                    for (; count != 0; count--)
                    {
                        length = Byte(sequence, track, at++) + (length << 8);
                    }
                    track.previous_length = length;
                }
                note.duration = static_cast<int32_t>(length * track.multiplier / 127);
                note.on = 0;
                if (track.ignore_notes == 0)
                {
                    NoteEvent event{.track = static_cast<int>(&track - sequence.tracks.data()),
                                    .channel = channel.index,
                                    .key = note.key,
                                    .velocity = note.velocity,
                                    .offset = start,
                                    .length = static_cast<uint32_t>(note.duration)};
                    if (channel.flags & 2)
                    {
                        Legato(channel, note, track.previous_key);
                        event.legato = true;
                    }
                    else if (build == Build::kEarlier)
                    {
                        // 0x02062B98 in Explorers of Time's ARM9 binary: the earlier build releases no voice
                        // of the key first.
                        channel.bend_fade.current = 0;
                        event.voices = StartNote(channel, note);
                        event.number = notes_started;
                    }
                    else
                    {
                        ReleaseKey(channel, note.key);
                        channel.bend_fade.current = 0;
                        const int node = HoldNote(channel, note);
                        if (node != kNone)
                        {
                            event.voices = StartNote(channel, NoteAt(node));
                            event.number = notes_started;
                        }
                    }
                    for (Voice& voice : voices)
                    {
                        if (event.voices & voice.bit)
                        {
                            voice.track = event.track;
                        }
                    }
                    if (hooks.on_note && &sequence == Followed())
                    {
                        hooks.on_note(event);
                    }
                }
                track.previous_key = note.key;
                channel.flags &= static_cast<uint8_t>(~2);
            }
            if (track.playing == 0 || track.wait != 0)
            {
                break;
            }
        }
    }
    track.wait--;
    track.position = at;
}

void Driver::State::StepSequence(Sequence& sequence)
{
    if (sequence.status[1] != 1)
    {
        return;
    }
    if (build == Build::kEarlier)
    {
        if (sequence.status[3] == 1 && sequence.volume.current == 0)
        {
            sequence.status = {};
            FreeAllVoices(sequence.synth);
        }
    }
    else if (sequence.delay != 0)
    {
        if (sequence.delay > 0)
        {
            sequence.delay--;
        }
        return;
    }
    sequence.driver_ticks++;
    if (sequence.fades != 0 || build == Build::kEarlier)
    {
        StepFades(sequence);
    }
    sequence.accumulator = Wrap(int64_t{sequence.accumulator} + sequence.step);
    while (sequence.accumulator > sequence.microseconds_per_beat)
    {
        if (hooks.on_tick && &sequence == Followed())
        {
            hooks.on_tick();
        }
        StepNoteLengths(sequence);
        Synth& synth = sequence.synth;
        for (Channel& channel : synth.channels)
        {
            if (channel.bend_fade.ticks != 0)
            {
                channel.bend_fade.ticks--;
                channel.bend_fade.current = channel.bend_fade.ticks == 0
                                                ? channel.bend_fade.target
                                                : Wrap(int64_t{channel.bend_fade.current} + channel.bend_fade.delta);
                channel.bend = static_cast<uint16_t>(channel.tuning + (channel.bend_fade.current >> 16) + synth.bend);
                FlagVoices(channel, 0x10);
            }
            if (channel.volume_fade.ticks != 0)
            {
                channel.volume_fade.ticks--;
                channel.volume_fade.current =
                    channel.volume_fade.ticks == 0
                        ? channel.volume_fade.target
                        : Wrap(int64_t{channel.volume_fade.current} + channel.volume_fade.delta);
                const int32_t product =
                    Wrap(int64_t{synth.combined_volume} * ((channel.volume_fade.current >> 16) * channel.expression));
                channel.volume = static_cast<uint16_t>(product / (127 * 127));
                FlagVoices(channel, 0x20);
            }
            if (channel.pan_fade.ticks != 0)
            {
                channel.pan_fade.ticks--;
                channel.pan_fade.current = channel.pan_fade.ticks == 0
                                               ? channel.pan_fade.target
                                               : Wrap(int64_t{channel.pan_fade.current} + channel.pan_fade.delta);
                channel.pan = static_cast<int16_t>(synth.pan - 0x40 + (channel.pan_fade.current >> 16));
                FlagVoices(channel, 0x40);
            }
        }
        sequence.tracks_playing = 0;
        for (int i = 0; i < sequence.track_slots; i++)
        {
            TrackState& track = sequence.tracks[static_cast<std::size_t>(i)];
            if (track.playing == 1)
            {
                RunTrack(sequence, track);
                if (track.playing == 1)
                {
                    sequence.tracks_playing++;
                }
            }
        }
        sequence.sequencer_ticks++;
        sequence.accumulator = Wrap(int64_t{sequence.accumulator} - sequence.microseconds_per_beat);
    }

    if (sequence.tracks_playing == 0)
    {
        sequence.status = {};
        FreeHeldNotes(sequence.synth);
    }
    else if (build == Build::kLater && sequence.status[3] != 0 && sequence.volume.current == 0)
    {
        if (sequence.status[3] == 1)
        {
            sequence.status = {};
            FreeAllVoices(sequence.synth);
            if (sequence.unknown_a == 1)
            {
                sequence.tracks_playing = 0;
            }
        }
        else
        {
            sequence.status[2] = 1;
            SilenceVoices(sequence.synth, 1);
        }
    }
}

} // namespace ntdlib::dse

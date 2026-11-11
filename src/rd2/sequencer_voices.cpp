// SPDX-License-Identifier: MIT

// The voices of the R&D2 sequencer: their allocation, the lists that link them to the tracks and to each other, and
// their release and stop.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "sequencer_internal.h"

namespace ntdlib::rd2
{

// Returns a voice for a note of the class, or -1 if none can be had: a free sample voice, or the first of the voices
// that play, if it's released or its priority isn't above the note's; or the PSG voice of the class, on the same terms.
int Sequencer::AllocateVoice(uint8_t voice_class, uint8_t priority)
{
    int v = -1;
    if (voice_class == 0)
    {
        // The first voice of the mixer's list is the one that a new note takes. It's in its release if any voice is.
        if (free_ < 0 && growing_ && (active_ < 0 || voices_[size_t(active_)].state == kVoicePlaying))
        {
            NewSampleVoice();
        }

        if (free_ >= 0)
        {
            v = free_;
        }
        else
        {
            v = active_;
            if (v < 0 || (voices_[size_t(v)].state == kVoicePlaying && priority < voices_[size_t(v)].priority))
            {
                return -1;
            }

            StopVoice(v);
        }

        Unlink(v, free_);
        voices_[size_t(v)].state = kVoicePlaying;
        voices_[size_t(v)].priority = priority;
        InsertActive(v);

        return v;
    }

    if (voice_class > kPsgVoices)
    {
        Warn("a region has a type the driver has no voice for");
        return -1;
    }

    v = kSampleVoices + voice_class - 1;
    if (growing_ && voices_[size_t(v)].state == kVoicePlaying)
    {
        if (const int grown = GrownPsgVoice(voice_class); grown >= 0)
        {
            v = grown;
        }
    }

    Voice& x = voices_[size_t(v)];
    if (x.state == kVoicePlaying && priority < x.priority)
    {
        return -1;
    }

    if (x.state != kVoiceFree)
    {
        StopVoice(v);
    }
    x.state = kVoicePlaying;
    x.priority = priority;

    return v;
}

// Takes sample voices out of use until `count` are left, as the driver's request 0x0304 does. The driver allocates each
// one as a note of the highest priority would. That takes the first free voice, or else the first voice that plays. The
// driver then takes the voice off the list of voices that play and keeps it on a separate list. Neither the notes nor
// the mixer use that list.
int Sequencer::NewSampleVoice()
{
    if (voices_.size() >= kMostVoices)
    {
        return -1;
    }

    const int v = int(voices_.size());
    voices_.emplace_back();
    PushFront(v, free_);

    return v;
}

int Sequencer::GrownPsgVoice(uint8_t voice_class)
{
    // A kind's voices each lie in a unit of their own. A new one takes the next unit that holds none of the kind's.
    int units = 0;
    for (size_t v = kVoices; v < voices_.size(); v++)
    {
        if (voices_[v].type == voice_class)
        {
            if (voices_[v].state != kVoicePlaying)
            {
                return int(v);
            }
            units++;
        }
    }
    if (voices_.size() >= kMostVoices || units + 1 >= agb::kMostPsgUnits)
    {
        return -1;
    }

    const int v = int(voices_.size());
    Voice& x = voices_.emplace_back();
    x.type = voice_class;
    x.unit = uint16_t(units + 1);
    if (size_t(x.unit) > grown_registers_.size())
    {
        // The new unit starts as the driver's init leaves the console's PSG, each channel with its pan on both sides.
        std::array<uint8_t, 0x40>& registers = grown_registers_.emplace_back();
        registers[kNr10 - kRegisters] = 0x08;
        registers[kNr12 - kRegisters] = 0xF0;
        registers[kNr51 - kRegisters] = 0xFF;
        grown_waves_.push_back(0);
    }

    return v;
}

void Sequencer::SetSampleVoices(int count)
{
    for (int left = kSampleVoices; left > count; left--)
    {
        const int v = AllocateVoice(0, 0xFF);
        if (v < 0)
        {
            break;
        }

        Unlink(v, active_);
        voices_[size_t(v)].state = kVoiceFree;
    }
}

void Sequencer::Unlink(int v, int& head)
{
    const Voice& x = voices_[size_t(v)];
    if (x.prev >= 0)
    {
        voices_[size_t(x.prev)].next = x.next;
    }
    else
    {
        head = x.next;
    }
    if (x.next >= 0)
    {
        voices_[size_t(x.next)].prev = x.prev;
    }
}

void Sequencer::PushFront(int v, int& head)
{
    Voice& x = voices_[size_t(v)];
    x.next = head;
    x.prev = -1;
    if (head >= 0)
    {
        voices_[size_t(head)].prev = v;
    }
    head = v;
}

// Puts a sample voice in the list the mixer takes them in. Released voices come first, then those that play, each
// ordered by priority and then by age. A new note takes the first.
void Sequencer::InsertActive(int v)
{
    Voice& x = voices_[size_t(v)];
    int before = -1;
    int at = active_;
    if (x.state == kVoicePlaying)
    {
        // A voice that plays goes after the released voices, and after the voices that play at its priority or lower.
        while (at >= 0 && (voices_[size_t(at)].state != kVoicePlaying || x.priority >= voices_[size_t(at)].priority))
        {
            before = at;
            at = voices_[size_t(at)].next;
        }
    }
    else if (x.state == kVoiceReleased)
    {
        // A released voice goes after the released voices at its priority or lower.
        while (at >= 0 && voices_[size_t(at)].state != kVoicePlaying && x.priority >= voices_[size_t(at)].priority)
        {
            before = at;
            at = voices_[size_t(at)].next;
        }
    }
    else
    {
        return;
    }

    x.next = at;
    if (at >= 0)
    {
        voices_[size_t(at)].prev = v;
    }
    x.prev = before;
    if (before >= 0)
    {
        voices_[size_t(before)].next = v;
    }
    else
    {
        active_ = v;
    }
}

void Sequencer::LinkToTrack(int slot, int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.track >= 0)
    {
        return;
    }

    Track& t = tracks_[size_t(slot)];
    x.track = slot;
    x.track_prev = -1;
    x.track_next = t.voices;
    t.voices = v;
    if (x.track_next >= 0)
    {
        voices_[size_t(x.track_next)].track_prev = v;
    }
}

void Sequencer::UnlinkFromTrack(int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.track < 0)
    {
        return;
    }

    const int slot = x.track;
    x.track = -1;
    if (x.track_next >= 0)
    {
        voices_[size_t(x.track_next)].track_prev = x.track_prev;
    }
    if (x.track_prev >= 0)
    {
        voices_[size_t(x.track_prev)].track_next = x.track_next;
    }
    else
    {
        tracks_[size_t(slot)].voices = x.track_next;
    }
}

// Returns the track of a voice that plays. TakeVoice() links every voice that plays to its track. For a voice without a
// track, slot 0 only keeps the index in range.
const Track& Sequencer::TrackOf(const Voice& v) const
{
    return tracks_[size_t(std::max(v.track, 0))];
}

// Releases a voice's note: a sample or wave voice fades out by itself, and a square or noise channel fades with its
// envelope. That frees the voice at once. A track in legato holds its notes.
void Sequencer::Release(int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.state != kVoicePlaying || x.track < 0 || tracks_[size_t(x.track)].legato)
    {
        return;
    }

    unit_ = x.unit;
    if (x.type == kSampleType)
    {
        Unlink(v, active_);
        x.state = kVoiceReleased;
        InsertActive(v);
    }
    else if (x.type == kWaveType)
    {
        x.state = kVoiceReleased;
    }
    else
    {
        const uint32_t frequency = x.pitch & 0xFFFF;
        const uint32_t step = x.release >> 5u;
        const uint8_t envelope = step ? uint8_t((x.volume << 4) | step) : 0;
        if (x.type == kSquare1Type)
        {
            Write(kNr12, 1, envelope);
            Write(kNr13, 2, frequency | 0x8000);
            Write(kNr11, 1, ReadBack(kNr11, 1) & 0xC0);
        }
        else if (x.type == kSquare2Type)
        {
            Write(kNr22, 1, envelope);
            Write(kNr23, 2, frequency | 0x8000);
            Write(kNr21, 1, ReadBack(kNr21, 1) & 0xC0);
        }
        else if (x.type == kNoiseType)
        {
            Write(kNr42, 1, envelope);
            Write(kNr44, 1, 0x80);
        }
        x.state = kVoiceFree;
    }

    events_.push_back(MakeEvent(Event::kRelease, tracks_[size_t(x.track)].number, v, x.id,
                                phase_ == Event::kCommands ? command_units_ : 0, 0, phase_));
    if (!x.own_pan)
    {
        x.pan = tracks_[size_t(x.track)].pan;
    }
    UnlinkFromTrack(v);
}

// Stops a voice at once and frees it.
void Sequencer::StopVoice(int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.state == kVoiceFree)
    {
        return;
    }

    unit_ = x.unit;
    switch (x.type)
    {
    case kSampleType:
        Unlink(v, active_);
        PushFront(v, free_);
        break;

    case kSquare1Type:
        Write(kNr12, 1, kNoEnvelope);
        Write(kNr14, 1, 0xC0);
        break;

    case kSquare2Type:
        Write(kNr22, 1, kNoEnvelope);
        Write(kNr24, 1, 0xC0);
        break;

    case kWaveType:
        Write(kNr30, 1, 0);
        break;

    case kNoiseType:
        Write(kNr42, 1, kNoEnvelope);
        Write(kNr44, 1, 0xC0);
        break;

    default:
        break;
    }

    events_.push_back(MakeEvent(Event::kStop, x.track >= 0 ? tracks_[size_t(x.track)].number : -1, v, x.id,
                                phase_ == Event::kCommands ? command_units_ : 0, 0, phase_));
    UnlinkFromTrack(v);
    x.state = kVoiceFree;
}

} // namespace ntdlib::rd2

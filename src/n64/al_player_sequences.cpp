// SPDX-License-Identifier: MIT

// The readers of the sequences of libultra's two sequence players: compressed MIDI for ALCSPlayer, and standard MIDI
// files of format 0 for ALSeqPlayer.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "al_player_constants.h"
#include "ntdlib/n64/al_player.h"

namespace ntdlib::n64
{

namespace
{

using al_player_constants::kStatusChannelPressure;
using al_player_constants::kStatusNoteOn;
using al_player_constants::kStatusProgram;

constexpr uint8_t kStatusMeta = 0xFF;
constexpr uint8_t kMetaTempo = 0x51;
constexpr uint8_t kMetaEndOfTrack = 0x2F;
constexpr uint8_t kMetaLoopStart = 0x2E;
constexpr uint8_t kMetaLoopEnd = 0x2D;
constexpr uint8_t kBlockCode = 0xFE;

constexpr std::size_t kCompressedHeader = 68;

} // namespace

uint8_t AlPlayer::SequenceByte(uint32_t place) const
{
    return place < bytes_.size() ? bytes_[place] : uint8_t{0};
}

void AlPlayer::NewCompressed()
{
    valid_tracks_ = 0;
    last_delta_ticks_ = 0;
    last_ticks_ = 0;
    delta_flag_ = true;
    for (int i = 0; i < 16; i++)
    {
        Track& track = tracks_[static_cast<std::size_t>(i)];
        track = {};
        const auto at = static_cast<uint32_t>(i) * 4;
        const uint32_t offset = (static_cast<uint32_t>(SequenceByte(at)) << 24) |
                                (static_cast<uint32_t>(SequenceByte(at + 1)) << 16) |
                                (static_cast<uint32_t>(SequenceByte(at + 2)) << 8) | SequenceByte(at + 3);
        if (offset != 0)
        {
            valid_tracks_ |= 1U << i;
            track.location = offset;
            track.delta_ticks = TrackVarLen(i);
        }
    }

    const uint32_t division = (static_cast<uint32_t>(SequenceByte(kCompressedHeader - 4)) << 24) |
                              (static_cast<uint32_t>(SequenceByte(kCompressedHeader - 3)) << 16) |
                              (static_cast<uint32_t>(SequenceByte(kCompressedHeader - 2)) << 8) |
                              SequenceByte(kCompressedHeader - 1);
    ticks_per_quarter_inverse_ = static_cast<float>(1.0 / static_cast<double>(static_cast<float>(division)));
}

bool AlPlayer::CompressedNextDelta(int32_t& ticks)
{
    if (valid_tracks_ == 0)
    {
        return false;
    }

    uint32_t first = 0xFFFFFFFF;
    for (int i = 0; i < 16; i++)
    {
        if (((valid_tracks_ >> i) & 1U) != 0)
        {
            Track& track = tracks_[static_cast<std::size_t>(i)];
            if (delta_flag_)
            {
                track.delta_ticks -= last_delta_ticks_;
            }

            first = std::min(first, track.delta_ticks);
        }
    }

    delta_flag_ = false;
    ticks = static_cast<int32_t>(first);

    return true;
}

AlPlayer::SequenceEvent AlPlayer::CompressedNextEvent()
{
    uint32_t first = 0xFFFFFFFF;
    int first_track = 0;
    for (int i = 0; i < 16; i++)
    {
        if (((valid_tracks_ >> i) & 1U) != 0)
        {
            Track& track = tracks_[static_cast<std::size_t>(i)];
            if (delta_flag_)
            {
                track.delta_ticks -= last_delta_ticks_;
            }

            if (track.delta_ticks < first)
            {
                first = track.delta_ticks;
                first_track = i;
            }
        }
    }

    const SequenceEvent event = CompressedTrackEvent(first_track);
    last_ticks_ += first;
    last_delta_ticks_ = first;
    if (event.type != SequenceEventType::kTrackEnd)
    {
        tracks_[static_cast<std::size_t>(first_track)].delta_ticks += TrackVarLen(first_track);
    }

    delta_flag_ = true;

    return event;
}

AlPlayer::SequenceEvent AlPlayer::CompressedTrackEvent(int track_number)
{
    Track& track = tracks_[static_cast<std::size_t>(track_number)];
    SequenceEvent event;
    event.offset = track.block_length != 0 ? track.block_place : track.location;
    const uint8_t status = TrackByte(track_number);
    if (status == kStatusMeta)
    {
        const uint8_t type = TrackByte(track_number);
        if (type == kMetaTempo)
        {
            event.type = SequenceEventType::kTempo;
            event.byte1 = TrackByte(track_number);
            event.byte2 = TrackByte(track_number);
            event.byte3 = TrackByte(track_number);
            track.last_status = 0;
        }
        else if (type == kMetaEndOfTrack)
        {
            valid_tracks_ ^= 1U << track_number;
            event.type = valid_tracks_ != 0 ? SequenceEventType::kTrackEnd : SequenceEventType::kEnd;
        }
        else if (type == kMetaLoopStart)
        {
            TrackByte(track_number);
            TrackByte(track_number);
            track.last_status = 0;
            event.type = SequenceEventType::kLoopStart;
        }
        else if (type == kMetaLoopEnd)
        {
            CompressedLoopEnd(track);
            track.last_status = 0;
            event.type = SequenceEventType::kLoopEnd;
        }

        return event;
    }

    event.type = SequenceEventType::kMidi;
    if ((status & 0x80) != 0)
    {
        event.status = status;
        event.byte1 = TrackByte(track_number);
        track.last_status = status;
    }
    else
    {
        event.status = track.last_status;
        event.byte1 = status;
    }

    const auto kind = static_cast<uint8_t>(event.status & 0xF0);
    if (kind != kStatusProgram && kind != kStatusChannelPressure)
    {
        event.byte2 = TrackByte(track_number);
        if (kind == kStatusNoteOn)
        {
            event.duration = TrackVarLen(track_number);
        }
    }

    return event;
}

// A loop end of a compressed track. Its running count is in the sequence's bytes. While the count is above 0, the
// track goes back to the loop's start and the count falls by 1. At 0 the track goes on, and the count starts again.
// A count of 0xFF loops forever.
void AlPlayer::CompressedLoopEnd(Track& track)
{
    const uint32_t count_place = track.location;
    const uint8_t loop_count = SequenceByte(count_place);
    const uint8_t running = SequenceByte(count_place + 1);
    if (running == 0)
    {
        bytes_[count_place + 1] = loop_count;
        track.location = count_place + 6;
    }
    else
    {
        if (running != 0xFF)
        {
            bytes_[count_place + 1] = static_cast<uint8_t>(running - 1);
        }

        const uint32_t offset = (static_cast<uint32_t>(SequenceByte(count_place + 2)) << 24) |
                                (static_cast<uint32_t>(SequenceByte(count_place + 3)) << 16) |
                                (static_cast<uint32_t>(SequenceByte(count_place + 4)) << 8) |
                                SequenceByte(count_place + 5);
        track.location = count_place + 6 - offset;
    }
}

uint8_t AlPlayer::TrackByte(int track_number)
{
    Track& track = tracks_[static_cast<std::size_t>(track_number)];
    if (track.block_length != 0)
    {
        const uint8_t byte = SequenceByte(track.block_place);
        track.block_place++;
        track.block_length--;

        return byte;
    }

    uint8_t byte = SequenceByte(track.location);
    track.location++;
    if (byte != kBlockCode)
    {
        return byte;
    }

    const uint8_t next = SequenceByte(track.location);
    track.location++;
    if (next == kBlockCode)
    {
        return byte;
    }

    const uint8_t low = SequenceByte(track.location);
    track.location++;
    const uint8_t length = SequenceByte(track.location);
    track.location++;
    const uint32_t back = (static_cast<uint32_t>(next) << 8) + low;
    track.block_place = track.location - (back + 4);
    track.block_length = length;
    byte = SequenceByte(track.block_place);
    track.block_place++;
    track.block_length--;

    return byte;
}

uint32_t AlPlayer::TrackVarLen(int track_number)
{
    uint32_t value = TrackByte(track_number);
    if ((value & 0x80) != 0)
    {
        value &= 0x7F;
        uint32_t next = 0;
        do
        {
            next = TrackByte(track_number);
            value = (value << 7) + (next & 0x7F);
        } while ((next & 0x80) != 0);
    }

    return value;
}

void AlPlayer::NewStandard()
{
    place_ = 0;
    last_status_ = 0;
    standard_ticks_ = 0;
    loop_count_ = 0;

    // The file's header: the chunk's id and length, the format, the track count and the division, then the track's id
    // and length.
    constexpr uint32_t kDivision = 12;
    constexpr uint32_t kFirstEvent = 22;
    const uint32_t division = (static_cast<uint32_t>(SequenceByte(kDivision)) << 8) | SequenceByte(kDivision + 1);
    ticks_per_quarter_inverse_ = static_cast<float>(1.0 / static_cast<double>(static_cast<float>(division)));
    track_start_ = kFirstEvent;
    place_ = kFirstEvent;
}

uint8_t AlPlayer::StandardByte()
{
    const uint8_t byte = SequenceByte(place_);
    place_++;

    return byte;
}

int32_t AlPlayer::StandardVarLen()
{
    int32_t next = StandardByte();
    int32_t value = next;
    if ((next & 0x80) != 0)
    {
        value &= 0x7F;
        do
        {
            next = StandardByte();
            value = (value << 7) + (next & 0x7F);
        } while ((next & 0x80) != 0);
    }

    return value;
}

bool AlPlayer::StandardNextDelta(int32_t& ticks)
{
    if (place_ >= bytes_.size())
    {
        return false;
    }

    const uint32_t saved = place_;
    ticks = StandardVarLen();
    place_ = saved;

    return true;
}

AlPlayer::SequenceEvent AlPlayer::StandardNextEvent()
{
    SequenceEvent event;
    const int32_t delta = StandardVarLen();
    standard_ticks_ += delta;
    event.offset = place_;
    const uint8_t status = StandardByte();
    if (status == kStatusMeta)
    {
        const uint8_t type = StandardByte();
        if (type == kMetaTempo)
        {
            event.type = SequenceEventType::kTempo;
            StandardByte();
            event.byte1 = StandardByte();
            event.byte2 = StandardByte();
            event.byte3 = StandardByte();
        }
        else if (type == kMetaEndOfTrack)
        {
            event.type = SequenceEventType::kEnd;
            StandardByte();
        }
        else
        {
            // The library's release build reads nothing more of other meta events, and its event keeps its old type. No
            // file of the images holds one in a played sequence's place: a meta event here skips its length.
            event.type = SequenceEventType::kLoopStart;
            const int32_t length = StandardVarLen();
            place_ += static_cast<uint32_t>(length);
        }

        last_status_ = 0;

        return event;
    }

    event.type = SequenceEventType::kMidi;
    if ((status & 0x80) != 0)
    {
        event.status = status;
        event.byte1 = StandardByte();
        last_status_ = status;
    }
    else
    {
        event.status = last_status_;
        event.byte1 = status;
    }

    const auto kind = static_cast<uint8_t>(event.status & 0xF0);
    if (kind != kStatusProgram && kind != kStatusChannelPressure)
    {
        event.byte2 = StandardByte();
    }

    return event;
}

AlPlayer::StandardMarker AlPlayer::MakeMarker(int32_t ticks)
{
    StandardMarker marker;
    if (ticks == 0)
    {
        marker.place = track_start_;

        return marker;
    }

    const uint32_t saved_place = place_;
    const uint8_t saved_status = last_status_;
    const int32_t saved_ticks = standard_ticks_;
    place_ = track_start_;
    last_status_ = 0;
    standard_ticks_ = 0;
    uint32_t last_place = place_;
    uint8_t last_status = last_status_;
    int32_t last_ticks = standard_ticks_;
    do
    {
        last_place = place_;
        last_status = last_status_;
        last_ticks = standard_ticks_;
        const SequenceEvent event = StandardNextEvent();
        if (event.type == SequenceEventType::kEnd)
        {
            last_place = place_;
            last_status = last_status_;
            last_ticks = standard_ticks_;
            break;
        }
    } while (standard_ticks_ < ticks && place_ < bytes_.size());

    marker.place = last_place;
    marker.last_status = last_status;
    marker.last_ticks = last_ticks;
    marker.current_ticks = standard_ticks_;
    place_ = saved_place;
    last_status_ = saved_status;
    standard_ticks_ = saved_ticks;

    return marker;
}

} // namespace ntdlib::n64

// SPDX-License-Identifier: MIT

// The C JAudio's tracks (jammain): their tree, their sequences' start, their mutes and pauses, and the updates of
// their levels. track_registers.cpp, track_notes.cpp and track_parser.cpp contain the rest of Tracks. The four
// files build as one translation unit. The compiler can then inline the small helpers across them.

#include "ntdlib/jac/track.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "jac/track_internal.h"
#include "ntdlib/jaudio1/calc.h"

namespace ntdlib::jac
{

namespace
{

// __PanCalc: the first level, the second, or a mix of the two by `weight`.
float PanCalc(float from, float to, float weight, uint8_t type)
{
    switch (type)
    {
    case 0:
        return from;
    case 1:
        return to;
    case 2:
        return from * (1.0f - weight) + to * weight;
    default:
        // The game returns a register that holds no result.
        return from;
    }
}

// The levels that UpdateTrack gives a track's channels.
struct TrackLevels
{
    float volume = 0.0f;
    float pitch = 0.0f;
    float pan = 0.0f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
};

// The track's levels that `flags` names, with the outer levels that `outer` switches on. The other levels are 0.
TrackLevels OwnLevels(const Track& track, uint32_t flags, const OuterParam* outer)
{
    const float weight = static_cast<float>(track.registers[kRegArguments + 3]) / kShortMax;
    const auto switched = [outer](uint32_t level)
    {
        return outer != nullptr && (outer->switches & level) != 0;
    };

    TrackLevels levels;
    if ((flags & kUpdateVolume) != 0)
    {
        levels.volume = track.muted ? 0.0f : track.timed[kTimedVolume].value;
        if (switched(OuterParam::kVolume))
        {
            levels.volume *= outer->volume;
        }
        if (track.paused && (track.pause_status & Track::kPauseVolume) != 0)
        {
            levels.volume *= track.timed[kTimedPause].value;
        }
    }
    if ((flags & kUpdatePitch) != 0)
    {
        levels.pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
        if (switched(OuterParam::kPitch))
        {
            levels.pitch = levels.pitch * outer->pitch;
        }
    }
    if ((flags & kUpdatePan) != 0)
    {
        levels.pan = track.timed[kTimedPan].value;
        if (switched(OuterParam::kPan))
        {
            levels.pan = PanCalc(levels.pan, outer->pan, weight, track.pan_calcs[0]);
        }
    }
    if ((flags & kUpdateFxmix) != 0)
    {
        levels.fxmix = track.timed[kTimedFxmix].value;
        if (switched(OuterParam::kFxmix))
        {
            levels.fxmix = PanCalc(levels.fxmix, outer->fxmix, weight, track.pan_calcs[1]);
        }
    }
    if ((flags & kUpdateDolby) != 0)
    {
        levels.dolby = track.timed[kTimedDolby].value;
        if (switched(OuterParam::kDolby))
        {
            levels.dolby = PanCalc(levels.dolby, outer->dolby, weight, track.pan_calcs[2]);
        }
    }

    return levels;
}

// The filters that `flags` names: the IIR's coefficients, the FIR when `outer` switches it on, and the distortion.
void UpdateFilters(ChannelManager& manager, const Track& track, uint32_t flags, const OuterParam* outer)
{
    if ((flags & kUpdateIir) != 0)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            manager.iir[i] = static_cast<int16_t>(ToInt32(track.timed[kTimedIir + i].value * kShortMax));
        }
        manager.filter_mode = static_cast<uint8_t>(manager.filter_mode | 0x20);
    }
    if (outer != nullptr && (flags & kUpdateFir) != 0 && (outer->switches & OuterParam::kFir) != 0)
    {
        manager.fir = outer->fir;
        manager.filter_mode = static_cast<uint8_t>((manager.filter_mode & 0x20) + 8);
    }
    if ((flags & kUpdateDistortion) != 0)
    {
        manager.distortion = static_cast<int16_t>(ToInt32(track.timed[kTimedDistortion].value * kShortMax));
    }
}

// Each of the track's two oscillators that the track itself routes (0x0e) moves its target level by its next offset.
void MoveByOscillators(Track& track, TrackLevels& levels)
{
    for (std::size_t i = 0; i < 2; i++)
    {
        if (track.osc_routes[i] != 0x0e)
        {
            continue;
        }

        const float offset = OscToOfs(&track.oscillators[i], track.osc_states[i]);
        switch (track.oscillators[i].target)
        {
        case OscData::kVolume:
            levels.volume = levels.volume * offset;
            break;
        case OscData::kPitch:
            levels.pitch = levels.pitch * offset;
            break;
        case OscData::kPan:
            levels.pan = levels.pan * offset;
            break;
        case OscData::kFxmix:
            levels.fxmix = levels.fxmix * offset;
            break;
        case OscData::kDolby:
            levels.dolby = levels.dolby * offset;
            break;
        default:
            break;
        }
    }
}

// Stores the levels that `flags` names in the track's channels. A child track that doesn't stand alone mixes them with
// its parent's, `parent`.
void StoreLevels(ChannelManager& manager, const ChannelManager* parent, const Track& track, uint32_t flags,
                 const TrackLevels& levels)
{
    if (parent == nullptr)
    {
        if ((flags & kUpdateVolume) != 0)
        {
            manager.volume = levels.volume;
        }
        if ((flags & kUpdatePitch) != 0)
        {
            manager.pitch = levels.pitch;
        }
        if ((flags & kUpdatePan) != 0)
        {
            manager.pan = levels.pan;
        }
        if ((flags & kUpdateFxmix) != 0)
        {
            manager.fxmix = levels.fxmix;
        }
        if ((flags & kUpdateDolby) != 0)
        {
            manager.dolby = levels.dolby;
        }
        return;
    }

    const float parent_weight = static_cast<float>(track.registers[kRegArguments + 4]) / kShortMax;
    if ((flags & kUpdateVolume) != 0)
    {
        manager.volume = parent->volume * levels.volume;
    }
    if ((flags & kUpdatePitch) != 0)
    {
        manager.pitch = parent->pitch * levels.pitch;
    }
    if ((flags & kUpdatePan) != 0)
    {
        manager.pan = PanCalc(levels.pan, parent->pan, parent_weight, track.parent_pan_calcs[0]);
    }
    if ((flags & kUpdateFxmix) != 0)
    {
        manager.fxmix = PanCalc(levels.fxmix, parent->fxmix, parent_weight, track.parent_pan_calcs[1]);
    }
    if ((flags & kUpdateDolby) != 0)
    {
        manager.dolby = PanCalc(levels.dolby, parent->dolby, parent_weight, track.parent_pan_calcs[2]);
    }
}

} // namespace

Tracks::Tracks(Channels& channels, const Banks& banks, Random& random)
    : channels_(channels), banks_(banks), random_(random)
{
    // Jaq_Reset.
    for (uint32_t i = 0; i < kTracks; i++)
    {
        tracks_[i].state = Track::kClosed;
        tracks_[i].child_mutes = 0;
        tracks_[i].muted = false;
        free_queue_[i] = i;
    }
    get_ = 0;
    back_ = 0;
    remain_ = kTracks;
    roots_.fill(Track::kNone);
    player_calls_.fill(Track::kNone);
    outer_.resize(kRoots);
    InitRegistTrack();
}

// seqsetup.

uint32_t Tracks::GetNewTrack()
{
    if (remain_ == 0)
    {
        return Track::kNone;
    }

    const uint32_t index = free_queue_[get_];
    get_ = (get_ + 1) % kTracks;
    remain_--;
    Track& track = tracks_[index];
    track.state = Track::kReady;
    track.allocated = true;

    return index;
}

bool Tracks::BackTrack(uint32_t index)
{
    Track& track = tracks_[index];
    track.state = Track::kClosed;
    if (!track.allocated || remain_ == kTracks)
    {
        return false;
    }

    free_queue_[back_] = index;
    remain_++;
    back_ = (back_ + 1) % kTracks;

    return true;
}

int32_t Tracks::AllocNewRoot(uint32_t index)
{
    for (uint32_t i = 0; i < kRoots; i++)
    {
        if (roots_[i] == Track::kNone)
        {
            roots_[i] = index;
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

void Tracks::DeAllocRoot(uint32_t index)
{
    for (uint32_t& root : roots_)
    {
        if (root == index)
        {
            root = Track::kNone;
            return;
        }
    }
}

void Tracks::InitTrack(uint32_t index, uint32_t cursor, uint32_t parent)
{
    // Init_Track. The fields that it doesn't set keep what the track's last use left.
    Track& track = tracks_[index];
    if (parent == Track::kNone)
    {
        track.cursor = 0;
        track.tempo = 120;
        track.timebase = 48;
        track.time_relate = 1;
        track.paused = false;
        track.pause_status = Track::kPauseParser | Track::kPauseChannels;
        track.child_mutes = 0;
        track.muted = false;
    }
    else
    {
        const Track& from = tracks_[parent];
        track.sequence = from.sequence;
        track.cursor = cursor;
        track.file_handle = from.file_handle;
        track.tempo = from.tempo;
        track.tempo_sync = false;
        track.tempo_rate = from.tempo_rate;
        track.timebase = from.timebase;
        track.time_relate = from.time_relate;
        track.paused = from.paused;
        track.pause_status = from.pause_status;
        track.child_mutes = 0;
    }
    track.depth = 0;
    track.note_time = 0;
    track.wait = 0;
    track.state = Track::kPlaying;
    track.parent = parent;
    track.enabled = 0;
    track.interrupted = 0;
    track.timer = 0;

    for (MoveParam& param : track.timed)
    {
        param.time = 0.0f;
        param.value = 1.0f;
        param.target = 1.0f;
    }
    const auto set = [&track](std::size_t param, float value)
    {
        track.timed[param].value = value;
        track.timed[param].target = value;
    };
    set(kTimedPitch, 0.0f);
    set(kTimedPan, 0.5f);
    set(kTimedPause, 0.5f);
    set(kTimedDelay, 0.0f);
    set(kTimedFxmix, 0.0f);
    set(kTimedDolby, 0.0f);
    for (std::size_t i = 1; i < 4; i++)
    {
        set(kTimedIir + i, 0.0f);
    }
    set(kTimedDistortion, 0.0f);

    track.registers.fill(0);
    ChannelManager& manager = channels_.Manager(index);
    if ((track.flags & 2) != 0 || parent == Track::kNone)
    {
        track.registers[kRegArguments + 0] = 0;
        track.registers[kRegArguments + 1] = 1;
        track.registers[kRegArguments + 2] = 1;
        track.registers[kRegArguments + 3] = 0x7fff;
        track.registers[kRegArguments + 4] = 0x4000;
        track.pan_calcs.fill(2);
        track.parent_pan_calcs.fill(2);
        manager.pan_calcs.fill(26);
        track.registers[kRegBank] = 0xf0;
        track.registers[kRegBendRange] = 0x0c;
        track.registers[kRegPriority] = 0x40;
    }
    else
    {
        const Track& from = tracks_[parent];
        for (std::size_t i = 0; i < 5; i++)
        {
            track.registers[kRegArguments + i] = from.registers[kRegArguments + i];
        }
        track.registers[kRegBank] = from.registers[kRegBank];
        track.registers[kRegBendRange] = from.registers[kRegBendRange];
        track.registers[kRegPriority] = from.registers[kRegPriority];
        track.pan_calcs = from.pan_calcs;
        track.parent_pan_calcs = from.parent_pan_calcs;
        manager.pan_calcs = channels_.Manager(parent).pan_calcs;
    }

    track.child_outer.fill(Track::kNone);
    track.children.fill(Track::kNone);
    track.notes.fill(kNoNote);
    track.channels.fill(Track::kNone);
    track.channel_ids.fill(0);
    track.note_flags = 0;
    track.last_note = 0;
    track.note_played = 0;
    track.gate = 0;
    InitEnv(track);
    track.transpose = 0;
    track.final_transpose = 0;
    track.ticks = -1;
    for (Port& port : track.ports)
    {
        port.import_flag = 0;
        port.export_flag = 0;
    }
    track.registered = false;

    notes_[index] = 0;
    jump_notes_[index].fill(0);
    looped_[index] = false;
    quiet_[index] = false;
    opens_[index]++;
}

int32_t Tracks::SetSeqData(uint32_t index, const Sequence& sequence)
{
    // Jaq_SetSeqData_Limit with the data in memory (mode 0) and no channels for the root's manager.
    if (index == Track::kNone)
    {
        index = GetNewTrack();
        if (index == Track::kNone)
        {
            return -1;
        }
    }
    else
    {
        tracks_[index].allocated = false;
    }

    // A root that the list can't hold leaves the track out of the pool, as the game does.
    const int32_t root = AllocNewRoot(index);
    if (root == -1)
    {
        return -1;
    }

    Track& track = tracks_[index];
    track.source = 0;
    track.sequence = &sequence;
    track.track_id = static_cast<uint32_t>(root);
    track.flags = 3;
    InitTrack(index, 0, Track::kNone);
    InitExtBuffer(static_cast<uint32_t>(root));
    AssignExtBuffer(index, static_cast<uint32_t>(root));
    channels_.InitOneShot(index, 0);
    track.tempo_counter = 0.0f;
    track.tempo_rate = 1.0f;
    UpdateTrackAll(index);
    track.state = Track::kReady;

    return root;
}

void Tracks::SetBankNumber(uint32_t index, uint8_t bank)
{
    Track& track = tracks_[index];
    track.registers[kRegBank] = static_cast<uint16_t>(bank << 8 | (track.registers[kRegBank] & 0xff));
}

bool Tracks::StartSeq(int32_t handle)
{
    if (handle < 0 || static_cast<uint32_t>(handle) >= kRoots)
    {
        return false;
    }

    const uint32_t index = roots_[handle];
    if (index == Track::kNone || tracks_[index].state != Track::kReady)
    {
        return false;
    }

    // Jac_RegisterPlayerCallback: the first free slot. The game refuses a callback that the list already holds.
    tracks_[index].state = Track::kPlaying;
    if (std::find(player_calls_.begin(), player_calls_.end(), index) != player_calls_.end())
    {
        return true;
    }

    for (uint32_t& slot : player_calls_)
    {
        if (slot == Track::kNone)
        {
            slot = index;
            break;
        }
    }

    return true;
}

bool Tracks::RegisterPlayerCall(PlayerCall call)
{
    for (std::size_t i = 0; i < kPlayerCalls; i++)
    {
        if (player_calls_[i] == Track::kNone)
        {
            player_calls_[i] = kReserved;
            game_calls_[i] = std::move(call);
            return true;
        }
    }

    return false;
}

void Tracks::RunPlayerCallbacks()
{
    // PlayerCallback (0x26a3d0).
    for (std::size_t i = 0; i < kPlayerCalls; i++)
    {
        const uint32_t slot = player_calls_[i];
        if (slot == Track::kNone)
        {
            continue;
        }

        const int32_t result = slot == kReserved ? (game_calls_[i] ? game_calls_[i]() : 0) : RootCallback(slot);
        if (result == -1)
        {
            player_calls_[i] = Track::kNone;
            game_calls_[i] = nullptr;
        }
    }
}

int32_t Tracks::OpenTrack(uint32_t index, uint32_t flags, uint32_t cursor)
{
    // Jaq_OpenTrack: the child's slot in the low 4 bits (a register's number with bit 5), its kind in bits 6 and 7,
    // or 4 with bit 5.
    uint8_t slot = flags & 0x0f;
    uint8_t kind = static_cast<uint8_t>((flags & 0xc0) >> 6);
    if ((flags & 0x20) != 0)
    {
        kind = 4;
        slot = static_cast<uint8_t>(ReadRegDirect(index, slot));
    }
    if (slot >= 16)
    {
        return -1;
    }

    Track& track = tracks_[index];
    if (track.children[slot] != Track::kNone)
    {
        CloseTrack(track.children[slot]);
    }

    const uint32_t child_index = GetNewTrack();
    if (child_index == Track::kNone)
    {
        unmodelled_++;
        return -1;
    }

    track.children[slot] = child_index;
    Track& child = tracks_[child_index];
    child.track_id = ((track.track_id << 4 | slot) & 0x0fffffff) | ((track.track_id & 0xf0000000) + 0x10000000);
    child.connection_id = 0;
    child.source = track.source;
    child.flags = kind;
    InitTrack(child_index, cursor, index);
    child.muted = track.muted || ((track.child_mutes >> slot) & 1) != 0;
    child.unheard = track.unheard || track.unheard_opens;
    child.outer = track.child_outer[slot];
    if (child.outer != Track::kNone)
    {
        outer_[child.outer].references++;
    }
    channels_.InitOneShot(child_index, 0);
    UpdateTrackAll(child_index);

    return 0;
}

void Tracks::AllNoteOff(uint32_t index)
{
    // __AllNoteOff: a root's notes release over 10 frames, a child's stop.
    Track& track = tracks_[index];
    const uint16_t release = track.parent == Track::kNone ? 10 : 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        NoteOff(index, i, release);
        track.notes[i] = kNoNote;
        track.channels[i] = Track::kNone;
    }
}

void Tracks::CloseTrack(uint32_t index)
{
    // Jaq_CloseTrack.
    if (index == Track::kNone || tracks_[index].state == Track::kClosed)
    {
        return;
    }

    AllNoteOff(index);
    BackTrack(index);
    Track& track = tracks_[index];
    for (uint32_t& child : track.children)
    {
        if (child != Track::kNone)
        {
            CloseTrack(child);
            child = Track::kNone;
        }
    }
    if (track.outer != Track::kNone)
    {
        outer_[track.outer].references--;
        track.outer = Track::kNone;
    }
    for (uint32_t& outer : track.child_outer)
    {
        if (outer != Track::kNone)
        {
            outer_[outer].assigned = false;
            outer = Track::kNone;
        }
    }
    track.muted = false;
    track.child_mutes = 0;
    track.unheard = false;
    track.unheard_opens = false;
    if (track.parent != Track::kNone)
    {
        channels_.FixMoveAll(index, track.parent);
    }
    else
    {
        channels_.FixReleaseAll(index);
    }
    UnRegistTrack(index);
}

void Tracks::StopSequence(uint32_t index)
{
    // __StopSeq.
    SeqUpdate(index, 0);
    CloseTrack(index);
    DeAllocRoot(index);
}

int32_t Tracks::RootCallback(uint32_t index)
{
    // Jaq_RootCallback: the root's ticks at its tempo, each a tick of every track of the tree, then the update of
    // their levels.
    Track& track = tracks_[index];
    if (track.state == Track::kClosed)
    {
        return -1;
    }
    if (track.state == Track::kStopping)
    {
        StopSequence(index);
        return -1;
    }

    track.tempo_counter += track.tempo_rate;
    while (track.tempo_counter >= 1.0f)
    {
        track.tempo_counter -= 1.0f;
        if (SeqMain(index, false) == -1)
        {
            StopSequence(index);
            return -1;
        }
    }
    SeqUpdate(index, 0);

    return 0;
}

void Tracks::MuteTrack(uint32_t index, bool mute)
{
    Track& track = tracks_[index];
    if (track.parent != Track::kNone)
    {
        Track& parent = tracks_[track.parent];
        track.muted = parent.muted || mute;
        const auto bit = static_cast<uint16_t>(1u << (track.track_id & 0xf));
        if (!mute)
        {
            parent.child_mutes = static_cast<uint16_t>(parent.child_mutes & ~bit);
        }
        else
        {
            parent.child_mutes = static_cast<uint16_t>(parent.child_mutes | bit);
        }
    }
    else
    {
        track.muted = mute;
    }
    track.update_flags |= kUpdateVolume;
    if (track.muted && (track.pause_status & Track::kMuteNotesOff) != 0)
    {
        for (uint8_t i = 0; i < 8; i++)
        {
            NoteOff(index, i, 10);
        }
    }
}

void Tracks::PauseTrack(uint32_t index)
{
    Track& track = tracks_[index];
    track.paused = true;
    if ((track.pause_status & Track::kPauseVolume) != 0)
    {
        track.update_flags |= kUpdateVolume;
    }
    if ((track.pause_status & Track::kPauseNotesOff) != 0)
    {
        for (uint8_t i = 0; i < 8; i++)
        {
            NoteOff(index, i, 10);
        }
    }
    if ((track.pause_status & Track::kPauseChannels) != 0)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            const uint32_t channel = track.channels[i];
            if (channel != Track::kNone && track.channel_ids[i] == channels_.At(channel).id)
            {
                channels_.SetPause(channel, true);
            }
        }
    }
    SetInterrupt(index, 0);

    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            PauseTrack(child);
        }
    }
}

void Tracks::UnpauseTrack(uint32_t index)
{
    // Jam_UnPauseTrack (0x26d820) with its children's. It acts on a track that isn't paused too.
    Track& track = tracks_[index];
    track.update_flags |= kUpdateVolume;
    track.paused = false;

    for (std::size_t i = 0; i < 8; i++)
    {
        const uint32_t channel = track.channels[i];
        if (channel != Track::kNone && track.channel_ids[i] == channels_.At(channel).id)
        {
            channels_.SetPause(channel, false);
        }
    }

    SetInterrupt(index, 1);

    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            UnpauseTrack(child);
        }
    }
}

void Tracks::UpdateTrackAll(uint32_t index)
{
    // Jam_UpdateTrackAll (0x26c8f0): every level that the track's channel manager gives its channels. A track of kind
    // 4 takes its parent's channels and sets nothing.
    Track& track = tracks_[index];
    if (track.flags == 4)
    {
        return;
    }

    ChannelManager& manager = channels_.Manager(index);
    const float weight = static_cast<float>(track.registers[kRegArguments + 3]) / kShortMax;
    manager.priority = track.registers[kRegPriority] | 0x10000u;
    manager.release_time = 0;
    const std::array<uint8_t, 2> delays = jaudio1::DelaySamples(track.timed[kTimedDelay].value);
    manager.delay_max = 0x10;
    manager.delays[0] = delays[0];
    manager.delays[1] = delays[1];

    float volume = track.muted ? 0.0f : track.timed[kTimedVolume].value;
    float pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
    float pan = track.timed[kTimedPan].value;
    float fxmix = track.timed[kTimedFxmix].value;
    float dolby = track.timed[kTimedDolby].value;
    if (track.outer != Track::kNone)
    {
        const OuterParam& outer = outer_[track.outer];
        if ((outer.switches & OuterParam::kVolume) != 0)
        {
            volume = volume * outer.volume;
        }
        if ((outer.switches & OuterParam::kPitch) != 0)
        {
            pitch = pitch * outer.pitch;
        }
        if ((outer.switches & OuterParam::kFxmix) != 0)
        {
            fxmix = PanCalc(fxmix, outer.fxmix, weight, track.pan_calcs[1]);
        }
        if ((outer.switches & OuterParam::kDolby) != 0)
        {
            dolby = PanCalc(dolby, outer.dolby, weight, track.pan_calcs[2]);
        }
        if ((outer.switches & OuterParam::kPan) != 0)
        {
            pan = PanCalc(pan, outer.pan, weight, track.pan_calcs[0]);
        }
    }

    if (track.parent == Track::kNone || (track.flags & 1) != 0)
    {
        manager.volume = volume;
        manager.pitch = pitch;
        manager.pan = pan;
        manager.fxmix = fxmix;
        manager.dolby = dolby;
        return;
    }

    const ChannelManager& parent = channels_.Manager(track.parent);
    const float parent_weight = static_cast<float>(track.registers[kRegArguments + 4]) / kShortMax;
    manager.volume = volume * parent.volume;
    manager.pitch = pitch * parent.pitch;
    manager.pan = PanCalc(pan, parent.pan, parent_weight, track.parent_pan_calcs[0]);
    manager.fxmix = PanCalc(fxmix, parent.fxmix, parent_weight, track.parent_pan_calcs[1]);
    manager.dolby = PanCalc(dolby, parent.dolby, parent_weight, track.parent_pan_calcs[2]);
    if (track.outer != Track::kNone && (outer_[track.outer].switches & OuterParam::kFir) != 0)
    {
        manager.fir = outer_[track.outer].fir;
        manager.filter_mode = 8;
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        manager.iir[i] = static_cast<int16_t>(ToInt32(track.timed[kTimedIir + i].value * kShortMax));
    }
    manager.filter_mode = static_cast<uint8_t>(manager.filter_mode | 0x20);
    manager.distortion = static_cast<int16_t>(ToInt32(track.timed[kTimedDistortion].value * kShortMax));
}

void Tracks::UpdateTrack(uint32_t index, uint32_t flags)
{
    // Jam_UpdateTrack: the levels that `flags` names, and the levels that the track's oscillators move.
    Track& track = tracks_[index];
    if (track.flags == 4)
    {
        return;
    }

    ChannelManager& manager = channels_.Manager(index);
    if ((flags & kUpdateDelay) != 0)
    {
        const std::array<uint8_t, 2> delays = jaudio1::DelaySamples(track.timed[kTimedDelay].value);
        manager.delays[0] = delays[0];
        manager.delays[1] = delays[1];
    }
    if ((flags & kUpdateTempo) != 0 && track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }

    const OuterParam* outer = track.outer != Track::kNone ? &outer_[track.outer] : nullptr;
    TrackLevels levels = OwnLevels(track, flags, outer);
    UpdateFilters(manager, track, flags, outer);
    MoveByOscillators(track, levels);
    const bool alone = track.parent == Track::kNone || (track.flags & 1) != 0;
    StoreLevels(manager, alone ? nullptr : &channels_.Manager(track.parent), track, flags, levels);
}

void Tracks::UpdateTempo(uint32_t index)
{
    // Jam_UpdateTempo: a root's ticks per sub-frame, from its timebase and tempo, times the outer tempo. Each child
    // copies its parent's.
    Track& track = tracks_[index];
    if (track.parent == Track::kNone)
    {
        track.tempo_rate =
            static_cast<float>(track.timebase) * static_cast<float>(track.tempo) / (kDacRate * 60.0f / 80.0f);
        if (track.outer != Track::kNone && (outer_[track.outer].switches & OuterParam::kTempo) != 0)
        {
            track.tempo_rate = track.tempo_rate * outer_[track.outer].tempo;
        }
    }
    else
    {
        track.tempo_rate = tracks_[track.parent].tempo_rate;
        track.timebase = tracks_[track.parent].timebase;
    }
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].state != Track::kClosed)
        {
            UpdateTempo(child);
        }
    }
}

std::vector<uint32_t> Tracks::FreeTracks() const
{
    std::vector<uint32_t> free;
    free.reserve(remain_);
    for (uint32_t i = 0; i < remain_; i++)
    {
        free.push_back(free_queue_[(get_ + i) % kTracks]);
    }

    return free;
}

} // namespace ntdlib::jac

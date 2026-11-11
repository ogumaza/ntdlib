// SPDX-License-Identifier: MIT

// JAudio1's tracks (JASTrack): their tree and their ports. track_levels.cpp, track_registers.cpp, track_notes.cpp and
// track_parser.cpp contain the rest of Tracks. The five files build as one translation unit. The compiler can then
// inline the small helpers across them.

#include "ntdlib/jaudio1/track.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jaudio1/track_internal.h"

namespace ntdlib::jaudio1
{

void OuterParam::SetParam(uint8_t flag, float value)
{
    switch (flag)
    {
    case kVolume:
        volume = value;
        break;
    case kPitch:
        pitch = value;
        break;
    case kFxmix:
        fxmix = value;
        break;
    case kPan:
        pan = value;
        break;
    case kDolby:
        dolby = value;
        break;
    case kTempo:
        tempo = value;
        break;
    default:
        return;
    }
    outer_update |= flag;
}

Tracks::Tracks(Voices& voices) : voices_(voices)
{
    tracks_.resize(kPoolTracks + kRootTracks);
    notes_.resize(tracks_.size());
    loop_notes_.resize(tracks_.size());
    looped_.resize(tracks_.size());
    quiet_.resize(tracks_.size());

    // JASTrack::newMemPool: each track of the pool comes with outer parameters (their constructor zeroes them).
    for (uint32_t i = 0; i < kPoolTracks; i++)
    {
        tracks_[i].outer = &pool_outer_.emplace_back();
        free_tracks_.push_back(i);
    }

    // The root tracks are JAInter::SeqParameter's, constructed once.
    for (std::size_t i = kPoolTracks; i < tracks_.size(); i++)
    {
        Construct(tracks_[i]);
    }

    // JASBankMgr::init: no number names a bank yet. A note on such a number asks for physical bank 0xff.
    virtual_banks_.fill(0xffff);
}

void Tracks::SetBank(uint32_t physical, const InstrumentBank* bank)
{
    banks_[physical & 0xff] = bank;
}

void Tracks::SetVirtualBank(uint32_t number, uint32_t physical)
{
    // JASBankMgr::setVir2PhyTable: number 0xffff names no bank.
    if (number == 0xffff)
    {
        return;
    }

    virtual_banks_[number & 0xff] = static_cast<uint16_t>(physical);
}

void Tracks::Construct(Track& track)
{
    // JASTrack's constructor. The outer parameters stay: the pool's tracks keep theirs. So do registers 14, 15 and 24
    // to 31.
    OuterParam* const outer = track.outer;
    const std::array<uint16_t, Track::kRegisters> registers = track.registers;
    track = Track{};
    track.outer = outer;
    track.vibrate.Init();
    track.tempo = 120;
    track.timebase = 120;
    track.pause_status = 0;
    track.time_relate = false;
    track.tempo_rate = 0.0f;
    std::copy(registers.begin() + 14, registers.begin() + 16, track.registers.begin() + 14);
    std::copy(registers.begin() + kRegAfter, registers.end(), track.registers.begin() + kRegAfter);
    std::copy(kAdsTable.begin(), kAdsTable.end(), track.adsr.begin());
    track.timed.fill(MoveParam{});
}

void Tracks::Init(uint32_t index)
{
    // JASTrack::init (0x3dc23c).
    Track& track = tracks_[index];
    track.sequence = nullptr;
    track.cursor = 0;
    track.wait = 0;
    track.depth = 0;
    track.stack.fill(0);
    track.loop_counts.fill(0);
    track.saved_wait = 0;
    track.interrupted = false;
    track.return_cursor = 0;

    track.import_flags.fill(0);
    track.export_flags.fill(0);
    track.ports.fill(0);

    track.interrupts_on = true;
    track.requests = 0;
    track.enabled = 0;
    track.timer_repeats = 0;
    track.timer = 0;
    track.timer_reload = 0;
    track.interrupt_targets.fill(0);
    track.targets_set = 0;

    track.channels.fill(Track::kNone);
    track.note_time = 0;
    track.note_flags = 0;
    track.latest_key = 0;
    track.gate = 0;
    track.updater = ChannelUpdater{};

    // initTimed: the times go to 0, and each value and target to its default. The steps stay.
    for (MoveParam& param : track.timed)
    {
        param.time = 0.0f;
        param.value = 1.0f;
        param.target = 1.0f;
    }
    for (const std::size_t i : {kTimedPitch, kTimedFxmix, kTimedDolby, kTimedDistortion, kTimedIir + 1, kTimedIir + 2,
                                kTimedIir + 3, kTimedDelay})
    {
        track.timed[i].value = 0.0f;
        track.timed[i].target = 0.0f;
    }
    for (const std::size_t i : {kTimedPan, kTimedPause})
    {
        track.timed[i].value = 0.5f;
        track.timed[i].target = 0.5f;
    }

    // JASRegisterParam::init. Registers 14 and 15 keep their values.
    for (const uint32_t reg : {0u, 1u, 2u, 3u, 4u, 5u})
    {
        track.registers[reg] = 0;
    }
    track.registers[kRegBankProgram] = 0xf0;
    track.registers[kRegBendRange] = 0x0c;
    track.registers[kRegPanPower] = 0;
    track.registers[kRegPanPower + 1] = 1;
    track.registers[kRegPanPower + 2] = 1;
    track.registers[kRegOuterWeight] = 0x7fff;
    track.registers[kRegParentWeight] = 0x4000;
    track.registers[kRegPriority] = 0x40;
    std::fill(track.registers.begin() + kRegLong, track.registers.begin() + kRegAfter, uint16_t{0});

    track.osc_routes = {0xf, 0xf};
    track.oscillators = {kEnvelopeDef, kEnvelopeDef};
    track.parent = Track::kNone;
    track.children.fill(Track::kNone);
    if (track.outer != nullptr)
    {
        track.outer->Init();
    }
    track.counter = 0.0f;
    track.tempo_rate = 1.0f;
    track.route = 0;
    track.vibrate.Init();
    track.update_flags = 0;
    track.direct_release = 0;
    track.tempo = 120;
    track.timebase = 48;
    UpdateTempo(index);
    track.transpose = 0;
    track.inherit = 0;
    track.pause_status = 0x0a;
    track.volume_mode = 0;
    track.updater.pan_calc = 13;
    track.updater.fxmix_calc = 13;
    track.updater.dolby_calc = 13;
    track.note_mask = 0;
    track.status = Track::kClosed;
    track.pan_calc = 0;
    track.fxmix_calc = 0;
    track.dolby_calc = 0;
    track.parent_pan_calc = 0;
    track.parent_fxmix_calc = 0;
    track.parent_dolby_calc = 0;
    track.paused = false;
    track.muted = false;
    track.time_relate = true;
    track.tempo_child = false;
}

void Tracks::Inherit(uint32_t index)
{
    // The end of JASTrack::openChild: the child takes its parent's tempo, timing, pause and mute, and unless bit 1
    // of its inherit flags says not to, the parent's registers and calculation types.
    Track& track = tracks_[index];
    if (track.parent == Track::kNone)
    {
        return;
    }

    const Track& parent = tracks_[track.parent];
    track.tempo = parent.tempo;
    track.tempo_child = false;
    track.tempo_rate = parent.tempo_rate;
    track.timebase = parent.timebase;
    track.time_relate = parent.time_relate;
    track.paused = parent.paused;
    track.pause_status = parent.pause_status;
    track.volume_mode = parent.volume_mode;
    track.muted = parent.muted;
    if ((track.inherit & 2) != 0)
    {
        return;
    }

    // JASRegisterParam::inherit.
    for (uint32_t reg = 0; reg <= 5; reg++)
    {
        track.registers[reg] = 0;
    }
    for (const uint32_t reg : {kRegBankProgram, kRegBendRange, kRegPriority, kRegPanPower, kRegPanPower + 1,
                               kRegPanPower + 2, kRegOuterWeight, kRegParentWeight})
    {
        track.registers[reg] = parent.registers[reg];
    }
    std::fill(track.registers.begin() + kRegLong, track.registers.begin() + kRegAfter, uint16_t{0});

    track.pan_calc = parent.pan_calc;
    track.parent_pan_calc = parent.parent_pan_calc;
    track.updater.pan_calc = parent.updater.pan_calc;
    track.fxmix_calc = parent.fxmix_calc;
    track.parent_fxmix_calc = parent.parent_fxmix_calc;
    track.updater.fxmix_calc = parent.updater.fxmix_calc;
    track.dolby_calc = parent.dolby_calc;
    track.parent_dolby_calc = parent.parent_dolby_calc;
    track.updater.dolby_calc = parent.updater.dolby_calc;
}

uint32_t Tracks::AllocTrack()
{
    // JASTrack's operator new: the pool's first free track, or none.
    if (free_tracks_.empty())
    {
        return Track::kNone;
    }

    const uint32_t index = free_tracks_.front();
    free_tracks_.pop_front();

    return index;
}

void Tracks::FreeTrack(uint32_t index)
{
    // JASTrack's operator delete: the track goes to the end of the pool's list.
    tracks_[index].allocated = false;
    free_tracks_.push_back(index);
}

uint32_t Tracks::StartSequence(const Sequence& sequence, OuterParam* outer)
{
    for (uint32_t index = kPoolTracks; index < tracks_.size(); index++)
    {
        Track& track = tracks_[index];
        if (track.allocated)
        {
            continue;
        }

        // JAInter::SeqParameter's assignExtBuffer, then JASTrack::setSeqData and startSeq.
        track.allocated = true;
        track.outer = outer;
        Init(index);
        track.inherit = 3;
        track.sequence = &sequence;
        track.cursor = 0;
        notes_[index] = 0;
        loop_notes_[index] = 0;
        looped_[index] = false;
        quiet_[index] = false;
        UpdateTrackAll(index);
        track.status = Track::kPlaying;
        list_.push_back(index);

        return index;
    }

    return Track::kNone;
}

void Tracks::RunSubframe()
{
    // The driver's sub-frame callbacks run in the order that they were registered. A callback that returns -1 is
    // removed.
    for (std::size_t i = 0; i < list_.size();)
    {
        if (RootCallback(list_[i]) < 0)
        {
            tracks_[list_[i]].allocated = false;
            list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
}

int32_t Tracks::RootCallback(uint32_t index)
{
    // JASTrack::rootCallback (0x3de40c). The game's branch for a track that stops (JASTrack::stopSeq) is left out:
    // nothing in the model stops a sequence.
    Track& track = tracks_[index];
    if (track.status == Track::kClosed)
    {
        return -1;
    }

    track.counter = track.tempo_rate + track.counter;
    if (track.counter < 1.0f)
    {
        UpdateSeq(index, 0, true);
        return 0;
    }
    while (track.counter >= 1.0f)
    {
        track.counter = track.counter + -1.0f;
        if (MainProc(index) == -1)
        {
            UpdateSeq(index, 0, true);
            Close(index);
            return -1;
        }
    }

    return 0;
}

void Tracks::Request(Track& track, uint32_t interrupt)
{
    // JASIntrMgr::request: a request counts only for an enabled interrupt.
    const uint32_t bit = 1u << (interrupt & 31);
    if ((bit & track.enabled) != 0)
    {
        track.requests = static_cast<uint8_t>(track.requests | bit);
    }
}

void Tracks::TimerProcess(Track& track)
{
    // JASIntrMgr::timerProcess: the timer interrupt (6) at the count's end, then the reload unless the repeats run
    // out.
    if (track.timer == 0)
    {
        return;
    }

    track.timer--;
    if (track.timer != 0)
    {
        return;
    }
    if ((track.enabled & 0x40) != 0)
    {
        track.requests = static_cast<uint8_t>(track.requests | 0x40);
    }
    if (track.timer_repeats != 0)
    {
        track.timer_repeats--;
        if (track.timer_repeats == 0)
        {
            return;
        }
    }
    track.timer = track.timer_reload;
}

bool Tracks::TryInterrupt(Track& track)
{
    // JASTrack::tryInterrupt: JASIntrMgr::checkIntr takes the lowest pending interrupt, then JASSeqCtrl::callIntr
    // saves the cursor and the wait.
    if (track.interrupted || !track.interrupts_on)
    {
        return false;
    }

    const uint32_t pending = static_cast<uint32_t>(track.requests & track.enabled);
    if (pending == 0)
    {
        return false;
    }

    const uint32_t interrupt = static_cast<uint32_t>(std::countr_zero(pending));
    track.requests = static_cast<uint8_t>(track.requests & ~(1u << interrupt));

    track.interrupted = true;
    track.return_cursor = track.cursor;
    track.cursor = track.interrupt_targets[interrupt];
    track.saved_wait = track.wait;
    track.wait = 0;

    return true;
}

int32_t Tracks::MainProc(uint32_t index)
{
    // JASTrack::mainProc (0x3dc60c).
    Track& track = tracks_[index];
    if (track.tempo_child && track.parent != Track::kNone)
    {
        const float ratio =
            std::fmin(static_cast<float>(track.tempo) / static_cast<float>(tracks_[track.parent].tempo), 1.0f);
        track.counter = track.counter + ratio;
        if (track.counter < 1.0f)
        {
            return 0;
        }

        track.counter = track.counter + -1.0f;
    }

    Request(track, 7);
    TimerProcess(track);
    TryInterrupt(track);

    if (track.paused && (track.pause_status & 2) != 0)
    {
        UpdateSeq(index, 0, false);
    }
    else
    {
        const int32_t result = ReadyToParse(track) ? ParseSeq(index) : 0;

        UpdateTimedParam(track);
        UpdateSeq(index, 0, false);
        if (result < 0)
        {
            return -1;
        }
    }

    for (std::size_t slot = 0; slot < 16; slot++)
    {
        const uint32_t child = track.children[slot];
        if (child != Track::kNone && tracks_[child].status != Track::kClosed && MainProc(child) == -1)
        {
            Close(child);
            track.children[slot] = Track::kNone;
        }
    }

    return 0;
}

bool Tracks::ReadyToParse(Track& track)
{
    // mainProc's wait: a wait of -1 lasts while the track's first channel plays. A count of ticks counts down
    // (JASSeqCtrl::waitCountDown), and the parser reads on when it reaches 0. The saved wait counts down too.
    if (track.wait == -1)
    {
        const uint32_t channel = track.channels[0];
        if (channel != Track::kNone && voices_.ChannelAt(channel).status != Channel::kStop)
        {
            return false;
        }
        track.wait = 0;
    }

    if (track.saved_wait - 1 >= 0)
    {
        track.saved_wait = track.saved_wait - 1;
    }
    if (track.wait < 1)
    {
        return true;
    }

    track.wait--;
    if (track.wait != 0)
    {
        return false;
    }
    if (track.note_time != -1 && track.note_flags == 0)
    {
        track.channels[0] = Track::kNone;
    }

    return true;
}

void Tracks::ReleaseChannels(uint32_t index)
{
    // The release of every channel in the track's list, which then ends the channels' callbacks (~JASTrack's loop).
    Track& track = tracks_[index];
    for (const uint32_t channel : track.channel_list)
    {
        voices_.Release(channel, 0);
        voices_.Free(channel);
    }
    track.channel_list.clear();
}

void Tracks::Close(uint32_t index)
{
    // JASTrack::close (0x3dcfd8). A root track releases its notes with a direct release of 10, a child with its
    // envelopes' release.
    Track& track = tracks_[index];
    if (track.status == Track::kClosed)
    {
        return;
    }

    const uint16_t release = track.parent == Track::kNone ? 10 : 0;
    for (uint32_t& channel : track.channels)
    {
        if (channel != Track::kNone)
        {
            voices_.Release(channel, release);
            channel = Track::kNone;
        }
    }
    track.status = Track::kClosed;
    for (std::size_t slot = 0; slot < 16; slot++)
    {
        const uint32_t child = track.children[slot];
        if (child != Track::kNone)
        {
            Close(child);
            track.children[slot] = Track::kNone;
        }
    }

    track.muted = false;
    ReleaseChannels(index);
    if (track.from_pool)
    {
        FreeTrack(index);
    }
}

void Tracks::Start(uint32_t index, const Sequence* sequence, uint32_t offset)
{
    // JASTrack::start (0x3de86c).
    Track& track = tracks_[index];
    track.sequence = sequence;
    track.cursor = offset;
    track.status = Track::kPlaying;
    UpdateTrackAll(index);
}

uint32_t Tracks::OpenChild(uint32_t index, uint32_t slot, uint32_t inherit)
{
    // JASTrack::openChild (0x3de8a4).
    slot &= 0xff;
    if (slot >= 16)
    {
        unmodelled_++;
        return Track::kNone;
    }
    const uint32_t old = tracks_[index].children[slot];
    if (old != Track::kNone)
    {
        Close(old);
        tracks_[index].children[slot] = Track::kNone;
    }

    const uint32_t child = AllocTrack();
    if (child == Track::kNone)
    {
        return Track::kNone;
    }

    Track& track = tracks_[child];
    Construct(track);
    track.allocated = true;
    Init(child);
    track.from_pool = true;
    track.parent = index;
    track.inherit = static_cast<uint8_t>(inherit);
    const uint32_t route = tracks_[index].route;
    track.route = (((route << 4) & 0x0ffffff0u) | (slot & 0xff)) | ((route + 0x10000000u) & 0xf0000000u);
    tracks_[index].children[slot] = child;
    notes_[child] = 0;
    loop_notes_[child] = 0;
    looped_[child] = false;
    quiet_[child] = false;
    Inherit(child);

    return child;
}

uint32_t Tracks::RouteTrack(uint32_t root, uint32_t route) const
{
    // JASTrack::routeTrack: the depth in the top 4 bits, then a slot of 4 bits for each level from the low bits.
    uint32_t at = root;
    const uint32_t depth = route >> 28;
    for (uint32_t i = 0; i < depth; i++)
    {
        at = tracks_[at].children[route & 0xf];
        if (at == Track::kNone)
        {
            return Track::kNone;
        }

        route >>= 4;
    }

    return at;
}

void Tracks::WritePortAppDirect(uint32_t index, uint32_t port, uint16_t value)
{
    // JASTrack::writePortAppDirect: a write to ports 0 and 1 requests interrupts 3 and 4.
    Track& track = tracks_[index];
    if (port >= 16)
    {
        unmodelled_++;
        return;
    }
    track.import_flags[port] = 1;
    track.ports[port] = value;
    if (port <= 1)
    {
        Request(track, port == 0 ? 3 : 4);
    }
}

bool Tracks::WritePortApp(uint32_t root, uint32_t route, uint16_t value)
{
    const uint32_t index = RouteTrack(root, route);
    if (index == Track::kNone)
    {
        return false;
    }

    WritePortAppDirect(index, (route >> 16) & 0xff, value);

    return true;
}

uint16_t Tracks::ReadPortAppDirect(uint32_t index, uint32_t port)
{
    // JASTrack::readPortAppDirect (0x3df4c8).
    if (port >= 16)
    {
        unmodelled_++;
        return 0;
    }
    tracks_[index].export_flags[port] = 0;
    return tracks_[index].ports[port];
}

bool Tracks::ReadPortApp(uint32_t root, uint32_t route, uint16_t& value)
{
    const uint32_t index = RouteTrack(root, route);
    if (index == Track::kNone)
    {
        return false;
    }

    value = ReadPortAppDirect(index, (route >> 16) & 0xff);

    return true;
}

void Tracks::MuteTrack(uint32_t index, bool mute)
{
    // JASTrack::muteTrack (0x3de684).
    Track& track = tracks_[index];
    track.muted = mute;
    track.update_flags |= kUpdateVolume;
    if (mute && (track.pause_status & 0x20) != 0)
    {
        for (uint32_t& channel : track.channels)
        {
            if (channel != Track::kNone)
            {
                voices_.Release(channel, 10);
                channel = Track::kNone;
            }
        }
    }
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone)
        {
            MuteTrack(child, mute);
        }
    }
}

} // namespace ntdlib::jaudio1

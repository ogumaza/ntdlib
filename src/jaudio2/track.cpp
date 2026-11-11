// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio2/track.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "jaudio2/track_constants.h"
#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jaudio2
{

using detail::kEnvOsc;
using detail::kPitchEnvOsc;

namespace
{

// main's constant of the tempo, by its bits.
constexpr float kTempoScale = std::bit_cast<float>(0x3faaaaabu); // 1.3333334: ticks per sub-frame per tempo × timebase

// main's constant of the pitch, by its bits.
const float kThird = std::bit_cast<float>(0x3eaaaaabu); // 0.33333334: the pitch's octaves per bend semitone

// JASDriver's sub-frames per frame (sSubFrames).
constexpr uint32_t kSubFrames = 7;

// JASTrack's OSC_ENV, the volume's envelope of a note on the DSP's oscillator: no attack, and a release of 10 to
// silence along the square curve.
constexpr std::array<OscPoint, 2> kOscEnvRelease = {OscPoint{1, 10, 0}, OscPoint{15, 0, 0}};
const OscData kOscEnv{0, 1.0f, {}, kOscEnvRelease, 1.0f, 0.0f};

// The rate of the DSP's oscillators' waveforms: the step of a note's pitch at its base key.
constexpr float kOscillatorRate = 16736.015625f;

// A channel's owner: its track and the track's channel manager.
uint32_t Owner(uint32_t track, uint32_t mgr)
{
    return track * 4 + mgr;
}

} // namespace

std::span<const OscPoint> Sequence::Table(uint32_t offset) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    auto [table, added] = tables_.try_emplace(offset);
    if (added)
    {
        for (uint64_t at = offset; at + 6 <= bytes_.size(); at += 6)
        {
            const auto at32 = static_cast<uint32_t>(at);
            table->second.push_back({static_cast<int16_t>(Native16(at32)), static_cast<int16_t>(Native16(at32 + 2)),
                                     static_cast<int16_t>(Native16(at32 + 4))});
        }
    }

    return table->second;
}

Tracks::Tracks(Voices& voices) : voices_(voices)
{
    tracks_.resize(kPoolTracks + kRootTracks);
    notes_.resize(tracks_.size());
    loop_notes_.resize(tracks_.size());
    looped_.resize(tracks_.size());
    quiet_.resize(tracks_.size());
    for (std::size_t i = kPoolTracks; i-- > 0;)
    {
        tracks_[i].in_pool = true;
        free_tracks_.push_back(static_cast<uint32_t>(i));
    }
}

void Tracks::SetBank(uint32_t number, const InstrumentBank* bank)
{
    if (number < banks_.size())
    {
        banks_[number] = bank;
    }
}

uint32_t Tracks::StartSequence(const Sequence& sequence, uint32_t offset, const SoundParams* sound)
{
    for (uint32_t index = kPoolTracks; index < tracks_.size(); index++)
    {
        Track& track = tracks_[index];
        if (track.allocated && track.status == Track::kRun)
        {
            continue;
        }

        // JAISeq: the track's construction, its channel manager's levels from the sound, the reader at the
        // sequence's start, and JASTrack::startSeq.
        Construct(index);
        track.allocated = true;
        track.mgrs[0].sound_params = sound;
        track.sequence = &sequence;
        track.cursor = offset;
        track.depth = 0;
        track.stack.fill(0);
        track.loop_counts.fill(0);
        list_.push_back(index);
        track.status = Track::kRun;

        return index;
    }

    return Track::kNone;
}

void Tracks::ReserveChildTracks(uint32_t root, uint32_t count)
{
    // Each track comes from the pool with the flag of a child that its parent frees. connectChild puts the track in a
    // slot that's empty.
    const auto reserve = [this](uint32_t parent, uint32_t slot)
    {
        const uint32_t child = AllocTrack();
        if (child == Track::kNone)
        {
            return Track::kNone;
        }

        Construct(child);
        tracks_[child].flags |= Track::kOwned;
        if (tracks_[parent].children[slot] == Track::kNone)
        {
            tracks_[child].parent = parent;
            tracks_[parent].children[slot] = child;
        }

        return child;
    };

    for (uint32_t i = 0; i < 2; i++)
    {
        const uint32_t child = reserve(root, i);
        for (uint32_t slot = 0; child != Track::kNone && slot < 16; slot++)
        {
            if (i * 16 + slot < count)
            {
                reserve(child, slot);
            }
        }
    }
}

void Tracks::UseRhythmParser(uint32_t index)
{
    tracks_[index].rhythm_parser = true;
}

void Tracks::Construct(uint32_t index)
{
    Track& track = tracks_[index];
    track.flags = 0;
    track.mgr_count = 1;
    track.status = Track::kFree;
    track.mgrs[0] = ChannelMgr{};
    track.mgrs[0].exists = true;
    for (std::size_t i = 1; i < track.mgrs.size(); i++)
    {
        track.mgrs[i] = ChannelMgr{};
    }

    Init(index);
}

void Tracks::Init(uint32_t index)
{
    Track& track = tracks_[index];
    notes_[index] = 0;
    loop_notes_[index] = 0;
    looped_[index] = false;
    quiet_[index] = false;

    // JASSeqCtrl::init and the reader's, the ports and the registers.
    track.sequence = nullptr;
    track.cursor = 0;
    track.depth = 0;
    track.stack.fill(0);
    track.loop_counts.fill(0);
    track.timer = 0;
    track.interrupted = false;
    track.swap = 0;
    track.interrupt_table = 0;
    track.pending = 0;
    track.mask = 0;
    track.interrupt_timer = 0;
    track.wait_note_finish = false;
    track.timer_count = 0;
    track.timer_reload = 0;
    track.rhythm_parser = false;
    track.import_flags = 0;
    track.export_flags = 0;
    track.ports.fill(0);
    track.registers.fill(0);

    track.params = {TrackParam{1.0f, 1.0f, 0}, TrackParam{}, TrackParam{},
                    TrackParam{0.5f, 0.5f, 0}, TrackParam{}, TrackParam{}};
    track.adsr = {OscPoint{0, 0, 0x7fff}, OscPoint{0, 0, 0x7fff}, OscPoint{0, 0, 0}, OscPoint{14, 0, 0}};
    track.parent = Track::kNone;
    track.oscillators = {kEnvOsc, kPitchEnvOsc};
    track.oscillator_tables = {false, false};
    track.children.fill(Track::kNone);

    // The first channel manager starts again without channels. The others go.
    ChannelMgr& first = track.mgrs[0];
    first.params = ChannelParams{};
    first.channels.fill(Channel::kNone);
    first.sound_params = nullptr;
    track.mgr_count = 1;
    for (std::size_t i = 1; i < track.mgrs.size(); i++)
    {
        track.mgrs[i] = ChannelMgr{};
    }

    track.vibrato_delay = 0;
    track.tremolo_delay = 0;
    track.playtime = 1.0f;
    track.vibrato_depth = 0.0f;
    track.vibrato_pitch = 1.0f;
    track.tremolo_depth = 0.0f;
    track.tremolo_pitch = 1.0f;
    track.pan_power = 1.0f;
    track.direct_release = 0;
    track.tempo = 120;
    track.timebase = 48;
    track.tempo_rate = 1.0f;
    track.skip_samples = 0;
    track.interval = track.tempo_rate * (7680.0f / voices_.DacRate());
    track.transpose = 0;
    track.latest_key = 60;
    track.bank = 0;
    track.program = 0xf0;
    track.bend_sense = 12;
    track.note_on_priority = 0x40;
    track.release_priority = 0;
    track.gate_rate = 100;
    track.mix_configs = {0x150, 0x210, 0x352, 0x412, 0, 0};
    track.fir.fill(0);
    track.fir[0] = 0x7fff;
    track.iir.fill(0);
    track.iir[0] = 0x7fff;
    track.filter_mode = 0;
    track.flags = static_cast<uint8_t>((track.flags & 0x80) | Track::kInvalid | Track::kGateByTempo);
    track.status = Track::kFree;
}

uint32_t Tracks::AllocTrack()
{
    if (free_tracks_.empty())
    {
        return Track::kNone;
    }

    const uint32_t index = free_tracks_.back();
    free_tracks_.pop_back();
    tracks_[index].allocated = true;

    return index;
}

void Tracks::FreeTrack(uint32_t index)
{
    tracks_[index].allocated = false;
    if (tracks_[index].in_pool)
    {
        free_tracks_.push_back(index);
    }
}

uint32_t Tracks::Root(uint32_t index) const
{
    while (tracks_[index].parent != Track::kNone)
    {
        index = tracks_[index].parent;
    }

    return index;
}

bool Tracks::Muted(uint32_t index) const
{
    for (; index != Track::kNone; index = tracks_[index].parent)
    {
        if ((tracks_[index].flags & Track::kMute) != 0)
        {
            return true;
        }
    }

    return false;
}

int32_t Tracks::TransposeSum(uint32_t index) const
{
    int32_t sum = 0;
    for (; index != Track::kNone; index = tracks_[index].parent)
    {
        sum += tracks_[index].transpose;
    }

    return sum;
}

void Tracks::SeqMain()
{
    // The list drops a sequence that ends. The game reads each node's next before it runs the node.
    std::vector<uint32_t> list = list_;
    for (const uint32_t index : list)
    {
        Track& track = tracks_[index];
        bool end = false;
        while (!end && track.playtime >= 1.0f)
        {
            track.playtime = track.playtime + -1.0f;
            if (TickProc(index) < 0)
            {
                end = true;
            }
        }
        if (end)
        {
            UpdateSeq(index, false, 1.0f);
            Close(index);
            std::erase(list_, index);
            if ((track.flags & Track::kOwned) != 0)
            {
                FreeTrack(index);
            }
            continue;
        }

        track.playtime = track.playtime + track.interval;
        UpdateSeq(index, false, 1.0f);
    }
}

int32_t Tracks::TickProc(uint32_t index)
{
    Track& track = tracks_[index];
    const int32_t result = SeqTick(index);
    for (TrackParam& param : track.params)
    {
        if (param.count != 0)
        {
            const float step = (param.target - param.value) / static_cast<float>(param.count);
            param.count--;
            param.value = param.value + step;
        }
    }
    track.flags |= Track::kInvalid;
    if (result < 0)
    {
        return -1;
    }

    for (uint32_t& child : track.children)
    {
        if (child == Track::kNone || tracks_[child].status != Track::kRun)
        {
            continue;
        }

        if (TickProc(child) < 0)
        {
            UpdateSeq(Root(index), false, 1.0f);
            Close(child);
            if ((tracks_[child].flags & Track::kOwned) != 0)
            {
                FreeTrack(child);
                child = Track::kNone;
            }
        }
    }

    return 0;
}

int32_t Tracks::SeqTick(uint32_t index)
{
    Track& track = tracks_[index];
    if (track.sequence == nullptr)
    {
        return 0;
    }

    // Interrupt 6 comes every tick, and interrupt 5 when the timer's count runs out.
    if ((track.mask & 0x40) != 0)
    {
        track.pending |= 0x40;
    }
    if (track.timer_count != 0)
    {
        track.timer_count--;
        if (track.timer_count == 0)
        {
            if ((track.mask & 0x20) != 0)
            {
                track.pending |= 0x20;
            }
            // A count of 0 reloads the timer for ever. Any other count stops it after that many runs.
            if (track.interrupt_timer == 0 || --track.interrupt_timer != 0)
            {
                track.timer_count = track.timer_reload;
            }
        }
    }

    if (track.wait_note_finish)
    {
        if (!CheckNoteStop(track, 0))
        {
            return 0;
        }
        track.wait_note_finish = false;
    }

    if (track.timer > 0)
    {
        track.timer--;
    }
    CheckInterrupt(track);
    while ((track.interrupted || track.timer == 0) && !track.wait_note_finish)
    {
        if (Parse(index) < 0)
        {
            return -1;
        }
    }

    return 0;
}

void Tracks::CheckInterrupt(Track& track)
{
    if (track.interrupted)
    {
        return;
    }
    const uint32_t ready = track.pending & track.mask;
    if (ready == 0)
    {
        return;
    }

    const uint32_t i = static_cast<uint32_t>(std::countr_zero(ready));
    track.pending = static_cast<uint16_t>(track.pending & ~(1u << i));
    const uint32_t target = track.sequence->U24(track.interrupt_table + 3 * i);
    track.interrupted = true;
    track.swap = track.cursor;
    track.cursor = target;
}

bool Tracks::CheckNoteStop(const Track& track, uint32_t note) const
{
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        const ChannelMgr& mgr = track.mgrs[i];
        if (mgr.exists && mgr.channels[note] != Channel::kNone)
        {
            return false;
        }
    }

    return true;
}

void Tracks::UpdateSeq(uint32_t index, bool update, float num)
{
    Track& track = tracks_[index];
    update = update || (track.flags & Track::kInvalid) != 0;
    track.flags &= static_cast<uint8_t>(~Track::kInvalid);
    if (update)
    {
        UpdateTrack(index, num);
    }

    const float child_num = track.pan_power * num;
    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].status == Track::kRun)
        {
            UpdateSeq(child, update, child_num);
        }
    }
}

void Tracks::UpdateTempo(Track& track)
{
    if (track.parent != Track::kNone)
    {
        const Track& parent = tracks_[track.parent];
        track.tempo = parent.tempo;
        track.timebase = parent.timebase;
        track.interval = parent.interval;
        return;
    }

    const uint32_t ticks = uint32_t{track.tempo} * track.timebase;
    const float scaled = static_cast<float>(static_cast<int32_t>(ticks)) * kTempoScale;
    track.interval = track.tempo_rate * (scaled / voices_.DacRate());
}

void Tracks::UpdateTrack(uint32_t index, float num)
{
    Track& track = tracks_[index];
    UpdateTempo(track);

    // Each channel manager's levels: the track's, the sound's and the parent's.
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (!mgr.exists)
        {
            continue;
        }

        float volume = track.params[0].value * track.params[0].value;
        float pan = (track.params[3].value + -0.5f) * track.pan_power;
        float fxmix = track.params[2].value;
        float dolby = track.params[4].value;
        float pitch = 1.0f;
        if (const SoundParams* sound = mgr.sound_params)
        {
            fxmix = fxmix + sound->fxmix;
            volume = volume * sound->volume;
            dolby = dolby + sound->dolby;
            pitch = sound->pitch;
            pan = pan + (sound->pan + -0.5f);
        }
        pan = pan * num;
        float octaves = (track.params[1].value * static_cast<float>(track.bend_sense)) * kThird;
        if (track.parent != Track::kNone)
        {
            const Track& parent = tracks_[track.parent];
            const ChannelMgr& from = parent.mgrs[i].exists ? parent.mgrs[i] : parent.mgrs[0];
            mgr.params.volume = volume * from.params.volume;
            mgr.params.pitch = pitch * from.params.pitch;
            pan = pan + (from.params.pan + -0.5f);
            mgr.params.fxmix = fxmix + from.params.fxmix;
            mgr.params.dolby = dolby + from.params.dolby;
            octaves = octaves + from.params.octaves;
        }
        else
        {
            mgr.params.volume = volume;
            mgr.params.pitch = pitch;
            mgr.params.fxmix = fxmix;
            mgr.params.dolby = dolby;
        }
        mgr.params.octaves = octaves;
        mgr.params.pan = pan + 0.5f;
    }
}

void Tracks::Close(uint32_t index)
{
    Track& track = tracks_[index];
    for (uint32_t& child : track.children)
    {
        if (child == Track::kNone)
        {
            continue;
        }

        Close(child);
        if ((tracks_[child].flags & Track::kOwned) != 0)
        {
            FreeTrack(child);
            child = Track::kNone;
        }
    }

    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        if (track.mgrs[i].exists)
        {
            ReleaseAll(track.mgrs[i]);
        }
    }

    track.parent = Track::kNone;
    track.status = Track::kStop;
}

void Tracks::ReleaseAll(ChannelMgr& mgr)
{
    for (uint32_t& channel : mgr.channels)
    {
        if (channel != Channel::kNone)
        {
            voices_.Release(channel, 0);
            voices_.Free(channel);
            channel = Channel::kNone;
        }
    }
}

uint32_t Tracks::ChildTrack(uint32_t root, uint32_t id) const
{
    const uint32_t child = tracks_[root].children[(id >> 4) & 15];
    if (child == Track::kNone)
    {
        return Track::kNone;
    }

    return tracks_[child].children[id & 15];
}

void Tracks::AssignSoundParams(uint32_t index, const SoundParams* params)
{
    tracks_[index].mgrs[0].sound_params = params;
}

void Tracks::Mute(uint32_t index, bool mute)
{
    Track& track = tracks_[index];
    track.flags = static_cast<uint8_t>(mute ? track.flags | Track::kMute : track.flags & ~Track::kMute);
}

uint32_t Tracks::OpenChild(uint32_t index, uint32_t slot)
{
    if (slot >= tracks_[index].children.size())
    {
        return Track::kNone;
    }

    uint32_t child = tracks_[index].children[slot];
    if (child != Track::kNone)
    {
        const uint32_t status = tracks_[child].status;
        if (status == Track::kRun)
        {
            UpdateSeq(Root(index), false, 1.0f);
            Close(child);
        }
        if (status == Track::kRun || status == Track::kStop)
        {
            Track& t = tracks_[child];
            const uint8_t owned = t.flags & Track::kOwned;
            Init(child);
            t.flags = static_cast<uint8_t>((t.flags & ~Track::kOwned) | owned);
            t.parent = index;
        }
    }
    else
    {
        child = AllocTrack();
        if (child == Track::kNone)
        {
            return Track::kNone;
        }
        Construct(child);
        tracks_[child].flags |= Track::kOwned;
        tracks_[child].parent = index;
        tracks_[index].children[slot] = child;
    }

    // As many channel managers as the parent's.
    Track& parent = tracks_[index];
    Track& t = tracks_[child];
    t.mgr_count = 1;
    for (uint32_t i = 1; i < parent.mgr_count && i < t.mgrs.size(); i++)
    {
        if (!t.mgrs[i].exists)
        {
            t.mgrs[i] = ChannelMgr{};
            t.mgrs[i].exists = true;
        }
        t.mgr_count++;
    }
    for (uint32_t i = t.mgr_count; i < t.mgrs.size(); i++)
    {
        t.mgrs[i] = ChannelMgr{};
    }

    // The child takes the parent's settings of its notes.
    t.flags = static_cast<uint8_t>((t.flags & ~Track::kGateByTempo) | (parent.flags & Track::kGateByTempo));
    t.bank = parent.bank;
    t.program = parent.program;
    t.note_on_priority = parent.note_on_priority;
    t.release_priority = parent.release_priority;
    t.skip_samples = parent.skip_samples;
    t.fir = parent.fir;
    t.iir = parent.iir;
    t.filter_mode = parent.filter_mode;
    t.mix_configs = parent.mix_configs;

    return child;
}

void Tracks::CloseChild(uint32_t index, uint32_t slot)
{
    if (slot >= tracks_[index].children.size())
    {
        return;
    }
    const uint32_t child = tracks_[index].children[slot];
    if (child == Track::kNone)
    {
        return;
    }

    UpdateSeq(Root(index), false, 1.0f);
    Close(child);
    if ((tracks_[child].flags & Track::kOwned) != 0)
    {
        FreeTrack(child);
        tracks_[index].children[slot] = Track::kNone;
    }
}

void Tracks::NoteOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity)
{
    if (Muted(index) || note >= 8)
    {
        return;
    }

    Track& track = tracks_[index];
    const uint32_t played = key + static_cast<uint32_t>(TransposeSum(index));
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (!mgr.exists)
        {
            continue;
        }

        if (mgr.channels[note] != Channel::kNone)
        {
            voices_.Release(mgr.channels[note], 0);
            voices_.Free(mgr.channels[note]);
            mgr.channels[note] = Channel::kNone;
        }
        mgr.channels[note] = ChannelStart(index, i, played, velocity, 0);
    }
}

void Tracks::NoteOff(uint32_t index, uint32_t note, uint16_t direct_release)
{
    Track& track = tracks_[index];
    if (note >= 8)
    {
        return;
    }

    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (mgr.exists && mgr.channels[note] != Channel::kNone)
        {
            voices_.Release(mgr.channels[note], direct_release);
            voices_.Free(mgr.channels[note]);
            mgr.channels[note] = Channel::kNone;
        }
    }
}

void Tracks::NoteOnGate(uint32_t index, uint32_t key, uint32_t velocity, uint32_t time, uint32_t flags)
{
    // The note's flags: bit 7 sweeps from the latest key, bit 6 ties the next gate note to this one.
    uint32_t gate = ((flags >> 5) & 2) | ((flags >> 7) & 1);
    if (time == 0)
    {
        gate |= 4;
    }
    GateOn(index, key, velocity, gate, static_cast<float>(time));

    Track& track = tracks_[index];
    if (time != 0)
    {
        track.timer = static_cast<int32_t>(time);
    }
    else
    {
        track.wait_note_finish = true;
    }
}

void Tracks::GateOn(uint32_t index, uint32_t key, uint32_t velocity, uint32_t flags, float time)
{
    Track& track = tracks_[index];
    const uint32_t played = key + static_cast<uint32_t>(TransposeSum(index));
    if (track.gate_rate != 100)
    {
        time = (static_cast<float>(track.gate_rate) / 100.0f) * time;
    }

    uint32_t length = 0;
    if ((track.flags & Track::kGateByTempo) != 0)
    {
        length = ToUint32(time / track.interval);
    }
    else
    {
        const float scaled = time * (120.0f / static_cast<float>(track.timebase));
        length = ToUint32(scaled * (static_cast<float>(kSubFrames) / 10.0f));
    }

    const uint32_t timer = (flags & 6) != 0 ? 0 : length;
    const uint32_t latest = track.latest_key;
    const uint32_t note_key = (flags & 1) != 0 ? latest : played;
    const int32_t sweep = static_cast<int32_t>(played - latest);
    for (uint32_t i = 0; i < track.mgr_count; i++)
    {
        ChannelMgr& mgr = track.mgrs[i];
        if (!mgr.exists)
        {
            continue;
        }

        uint32_t& channel = mgr.channels[0];
        if ((track.flags & Track::kGateTie) != 0)
        {
            if (channel != Channel::kNone)
            {
                Channel& c = voices_.ChannelAt(channel);
                c.velocity = static_cast<uint16_t>(velocity);
                c.update_timer = timer;
                c.key = static_cast<int16_t>(note_key - c.wave.base_key);
            }
        }
        else
        {
            if (channel != Channel::kNone)
            {
                voices_.Release(channel, 0);
                voices_.Free(channel);
                channel = Channel::kNone;
            }
            if (!Muted(index))
            {
                channel = ChannelStart(index, i, note_key, velocity, timer);
            }
        }

        if ((flags & 1) != 0 && channel != Channel::kNone)
        {
            // setKeySweepTarget: a sweep from the latest key to this one over the note's length, or at once.
            Channel& c = voices_.ChannelAt(channel);
            float delta = static_cast<float>(sweep);
            if (length != 0)
            {
                c.key_sweep_target = delta;
                delta = 0.0f;
            }
            c.key_sweep = delta;
            c.key_sweep_count = length;
        }
    }

    track.latest_key = static_cast<uint8_t>(played);
    track.flags = static_cast<uint8_t>((track.flags & ~Track::kGateTie) | ((flags << 3) & Track::kGateTie));
}

uint32_t Tracks::ChannelStart(uint32_t index, uint32_t mgr_index, uint32_t key, uint32_t velocity, uint32_t timer)
{
    Track& track = tracks_[index];
    notes_[index]++;
    ChannelMgr& mgr = track.mgrs[mgr_index];
    const InstrumentBank* bank = track.bank < banks_.size() ? banks_[track.bank] : nullptr;
    const auto priority = static_cast<uint16_t>(track.note_on_priority | track.release_priority << 8);
    const uint32_t channel = BankNoteOn(bank, track.program, key, velocity, priority, Owner(index, mgr_index));
    if (channel == Channel::kNone)
    {
        // The game asks the bank again for its debug output. The instrument's random effects step again. For a bank
        // that isn't loaded, the port reads through a null pointer there and stops. The model goes on without the note.
        if (bank != nullptr)
        {
            InstParam param;
            bank->GetInstParam(track.program, key & 0xff, velocity & 0xff, param, inst_random_);
        }
        else
        {
            missing_bank_notes_++;
        }

        return Channel::kNone;
    }

    Channel& c = voices_.ChannelAt(channel);
    c.update_timer = timer;
    c.skip_samples = track.skip_samples;
    c.vibrato.SetDelay(track.vibrato_delay);
    c.tremolo.SetDelay(track.tremolo_delay);
    c.params = mgr.params;
    c.mix_configs = track.mix_configs;

    // overwriteOsc: the track's envelopes replace the instrument's.
    for (uint32_t i = 0; i < track.oscillators.size(); i++)
    {
        if (track.oscillator_tables[i])
        {
            c.oscillators[i].InitStart(&track.oscillators[i]);
        }
    }
    if (track.direct_release != 0)
    {
        c.oscillators[0].SetDirectRelease(track.direct_release);
    }

    return channel;
}

uint32_t Tracks::BankNoteOn(const InstrumentBank* bank, uint32_t program, uint32_t key, uint32_t velocity,
                            uint16_t priority, uint32_t owner)
{
    if (program >= 0xf0)
    {
        return NoteOnOsc(program - 0xf0, key, velocity, priority, owner);
    }
    if (bank == nullptr)
    {
        return Channel::kNone;
    }

    InstParam param;
    if (!bank->GetInstParam(program, key & 0xff, velocity & 0xff, param, inst_random_))
    {
        return Channel::kNone;
    }
    const BankWave* wave = bank->Waves() != nullptr ? bank->Waves()->Find(param.wave_id) : nullptr;
    if (wave == nullptr)
    {
        return Channel::kNone;
    }

    // The game's pool of channels never runs out in its music. Without a channel, the model drops the note.
    const uint32_t channel = voices_.NewChannel(owner);
    if (channel == Channel::kNone)
    {
        return Channel::kNone;
    }

    Channel& c = voices_.ChannelAt(channel);
    c.priority = priority;
    c.wave = wave->info;
    c.wave_address = wave->address;
    c.type = param.type;
    c.sound_params.pitch = (wave->info.sample_rate / voices_.DacRate()) * param.pitch;
    if (!param.fixed_key)
    {
        c.key = static_cast<int16_t>((key & 0xff) - wave->info.base_key);
    }
    c.velocity = static_cast<uint16_t>(velocity & 0xff);
    c.sound_params.volume = param.volume;
    c.sound_params.fxmix = param.fxmix;
    c.sound_params.pan = param.pan;
    c.sound_params.dolby = param.dolby;
    for (uint32_t i = 0; i < param.oscillator_count && i < c.oscillators.size(); i++)
    {
        c.oscillators[i].InitStart(param.oscillators[i]);
    }
    c.oscillators[0].SetDirectRelease(param.direct_release);

    return voices_.Play(this, channel) ? channel : Channel::kNone;
}

uint32_t Tracks::NoteOnOsc(uint32_t wave, uint32_t key, uint32_t velocity, uint16_t priority, uint32_t owner)
{
    const uint32_t channel = voices_.NewChannel(owner);
    if (channel == Channel::kNone)
    {
        return Channel::kNone;
    }

    Channel& c = voices_.ChannelAt(channel);
    c.priority = priority;
    c.wave_address = wave;
    c.type = Channel::kOsc;
    c.key = static_cast<int16_t>((key & 0xff) - c.wave.base_key);
    c.velocity = static_cast<uint16_t>(velocity & 0xff);
    c.sound_params.pitch = kOscillatorRate / voices_.DacRate();
    c.oscillators[0].InitStart(&kOscEnv);

    return voices_.Play(this, channel) ? channel : Channel::kNone;
}

void Tracks::OnChannel(uint32_t owner, Channel::Event event, uint32_t channel, DspBlock* block)
{
    const uint32_t index = owner / 4;
    if (index >= tracks_.size())
    {
        return;
    }

    Track& track = tracks_[index];
    ChannelMgr& mgr = track.mgrs[owner % 4];
    Channel& c = voices_.ChannelAt(channel);

    switch (event)
    {
    case Channel::kEventPlay:
    case Channel::kEventStart:
        {
            c.params = mgr.params;
            c.vibrato.SetDepth(track.vibrato_depth);
            c.vibrato.SetPitch(track.vibrato_pitch);
            c.tremolo.SetDepth(track.tremolo_depth);
            c.tremolo.SetPitch(track.tremolo_pitch);
            if ((track.filter_mode & 0x20) != 0)
            {
                block->SetIirFilterParam(track.iir);
            }
            if ((track.filter_mode & 0x1f) != 0)
            {
                block->SetFir8FilterParam(track.fir);
            }
            block->SetFilterMode(track.filter_mode);
            block->SetDistFilter(ToInt32(track.params[5].value * 32767.0f));
            break;
        }

    case Channel::kEventStop:
        for (uint32_t& slot : mgr.channels)
        {
            if (slot == channel)
            {
                slot = Channel::kNone;
                break;
            }
        }
        break;

    case Channel::kEventTimer:
        voices_.Release(channel, 0);
        voices_.Free(channel);
        mgr.channels[0] = Channel::kNone;
        break;
    }
}

} // namespace ntdlib::jaudio2

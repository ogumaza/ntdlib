// SPDX-License-Identifier: MIT

// Pikmin 2's conductors (see performer.h): PSAutoBgm in Pikmin 2's main.dol (the USA release's addresses).
// PSGame::seqCpuSync (0x803372e0) passes calls 0x300, 0x600 and 0x800 to the conductor (Conductor::seqCpuSync_AutoBgm,
// 0x803392e8). It passes 0x900 to 0xd00 to the module that the calling track's last call 0x800 started, and 0x2e00 to
// the first track's beat (0x8033d374).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ntdlib/jaudio/calc.h"
#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{

int Performer::ChildNumber(int t) const
{
    const Track& tr = TrackAt(t);
    if (tr.parent < 0)
    {
        return -1;
    }
    const std::array<int, kChildren>& siblings = TrackAt(tr.parent).children;
    const auto it = std::ranges::find(siblings, t);
    return it != siblings.end() ? static_cast<int>(it - siblings.begin()) : -1;
}

uint32_t Performer::ConductorRandom(uint32_t n)
{
    // PSSystem::oRandom steps as a linear congruential generator. Its top 23 bits make a float in [0, 1), and the
    // product with n, in single precision, gives the draw (0x8033b13c).
    last_draw_ = tick_;
    conductor_random_ = ntdlib::jaudio::NextFastRandom(conductor_random_);
    const float unit = ntdlib::jaudio::FastRandomUnit(conductor_random_);
    return static_cast<uint32_t>(static_cast<float>(n) * unit);
}

std::optional<uint16_t> Performer::ConductorCall(int t, uint16_t id)
{
    switch (id)
    {
    case 0x300:
        return ConductorTables(t);

    case 0x600:
    case 0x800:
        return ConductorTrackCall(t, id);

    case 0x900:
    case 0xa00:
    case 0xb00:
    case 0xc00:
    case 0xd00:
        return ConductorModuleCall(t, id);

    case 0x2e00:
        return ConductorBeat();

    case 0xe00:
        // SeqTrackRoot::init (0x803426b0) clears the beat.
        if (TrackAt(t).parent < 0)
        {
            beat_top_ = false;
        }
        return std::nullopt;

    default:
        return std::nullopt;
    }
}

uint16_t Performer::ConductorTables(int t)
{
    // The first track's tempo, and the sequence's two tables of lists, from the track's ports 2 to 5
    // (Conductor::createTables, 0x80339404). The answer is the number of tracks to open.
    const Conductor& conductor = *sound_.conductor;
    Track& tr = TrackAt(t);
    tr.tempo = conductor.tempo;
    if (tr.parent < 0)
    {
        tempo_ = conductor.tempo;
        tr.dirty = true;
        EmitTempo();
    }
    conductor_phrases_ = static_cast<uint32_t>(tr.ports[2]) << 16 | tr.ports[3];
    conductor_lengths_ = static_cast<uint32_t>(tr.ports[4]) << 16 | tr.ports[5];
    return static_cast<uint16_t>(conductor.tracks.size());
}

uint16_t Performer::ConductorTrackCall(int t, uint16_t id)
{
    const Conductor& conductor = *sound_.conductor;
    const int number = ChildNumber(t);
    if (number < 0 || static_cast<std::size_t>(number) >= conductor.tracks.size())
    {
        throw FormatError("a track that the conductor doesn't list calls it");
    }
    const ConductorTrack& track = conductor.tracks[static_cast<std::size_t>(number)];
    ConductorTrackState& state = conductor_tracks_[static_cast<std::size_t>(number)];
    const ConductorTrack* followed = nullptr;
    if (track.follows != 0x10)
    {
        if (track.follows >= number)
        {
            throw FormatError("a track of the conductor follows a later track");
        }
        followed = &conductor.tracks[track.follows];
    }

    if (id == 0x600)
    {
        // Track::seqCpuSync_AutoBgm_Track (0x80339c38): the track starts before its first module, and its modules find
        // their lists again. The answer is the wait before the track starts.
        state.module = 0xff;
        for (ConductorModuleState& m : state.modules)
        {
            m.tables = false;
        }
        const uint8_t half = followed != nullptr ? followed->half_beat : track.half_beat;
        return static_cast<uint16_t>((half == 1 ? 60 : 0) + track.start_beats * 120);
    }

    return NextConductorModule(t, number, followed);
}

uint16_t Performer::NextConductorModule(int t, int number, const ConductorTrack* followed)
{
    // The track's next module, or its last again (Track::incCurModule, 0x80339da8). Without one, the answer is 0.
    const ConductorTrack& track = sound_.conductor->tracks[static_cast<std::size_t>(number)];
    ConductorTrackState& state = conductor_tracks_[static_cast<std::size_t>(number)];
    Track& tr = TrackAt(t);
    if (state.module == 0xff)
    {
        state.module = 0;
    }
    else if (state.module + 1u < track.modules.size())
    {
        state.module++;
    }
    if (state.module >= track.modules.size())
    {
        return 0;
    }
    tr.conductor_track = number;

    // Module::seqCpuSync_AutoBgm_Module (0x8033ab20): the module's bank and program at port 3, its volume and pan, its
    // transpose, and the start of its rounds. The answer is the wait before the module starts.
    const ConductorModule& mod = track.modules[state.module];
    ConductorModuleState& m = state.modules[state.module];
    WritePortApp(t, 3, static_cast<uint32_t>(mod.bank) << 8 | mod.program);
    SetParam(t, 0, static_cast<float>(std::min<uint8_t>(mod.volume, 127)) / 127.0f, 0);
    SetParam(t, 3, static_cast<float>(std::min<uint8_t>(mod.pan, 127)) / 127.0f, 0);
    tr.transpose = static_cast<int8_t>(mod.transpose);
    m.beats = 0;
    m.cycle = 0;
    m.phrase = 0xffff;
    m.steps = {};
    if (mod.phrases == 0 && mod.rests == 0)
    {
        throw FormatError("a module of the conductor has no phrases and no rests");
    }
    if (!m.tables)
    {
        FindConductorLists(mod, m);
    }
    uint8_t half = mod.half_beat;
    if (followed != nullptr)
    {
        half = followed->modules.empty() ? uint8_t{0} : followed->modules[0].half_beat;
    }

    return static_cast<uint16_t>((half == 1 ? 60 : 0) + mod.start_beats * 120);
}

void Performer::FindConductorLists(const ConductorModule& mod, ConductorModuleState& m)
{
    // Module::setTableAddress (0x8033ad38): the module's lists, and the entries before each list's end.
    m.phrase_list = Data24(conductor_phrases_ + 3u * static_cast<uint16_t>(mod.phrase_list));
    m.phrase_count = 0;
    while (Data24(m.phrase_list + 3u * m.phrase_count) != 0xffffff)
    {
        if (++m.phrase_count == 0xff)
        {
            throw FormatError("a list of phrases of the conductor has no end");
        }
    }
    m.length_list = Data24(conductor_lengths_ + 3u * static_cast<uint16_t>(mod.length_list));
    m.length_count = 0;
    while (Data16(m.length_list + 2u * m.length_count) != 0xffff)
    {
        if (++m.length_count == 0xff)
        {
            throw FormatError("a list of lengths of the conductor has no end");
        }
    }
    m.tables = true;
}

uint16_t Performer::ConductorModuleCall(int t, uint16_t id)
{
    const Conductor& conductor = *sound_.conductor;
    Track& tr = TrackAt(t);
    if (tr.conductor_track < 0)
    {
        throw FormatError("a track calls the conductor's module before the module starts");
    }
    // The calls move the conductor on. A pass that only rests still changes what the track plays next.
    Changed(t);
    const std::size_t number = static_cast<std::size_t>(tr.conductor_track);
    const ConductorTrack& track = conductor.tracks[number];
    ConductorTrackState& state = conductor_tracks_[number];
    const ConductorModule& mod = track.modules[state.module];
    ConductorModuleState& m = state.modules[state.module];
    const uint8_t count = m.cycle == 0 ? mod.phrases : mod.rests;
    switch (id)
    {
    case 0x900:
        // Module::cycleLoop (0x8033aef4): 1 moves on to the next module once the module's beats have passed. Otherwise
        // the round takes its next step. 2 starts the other round, 3 starts a round of phrases and 0 takes another
        // step.
        if (state.module + 1u < track.modules.size() && m.beats >= mod.beats)
        {
            return 1;
        }
        if (count != 0)
        {
            m.steps[m.cycle]++;
            if (m.steps[m.cycle] > count)
            {
                m.steps[m.cycle] = 0;
            }
        }
        if (count == 0 || m.steps[m.cycle] == 0)
        {
            m.cycle = static_cast<uint8_t>(m.cycle ^ 1);
            return 2;
        }
        return m.steps[m.cycle] == 1 && m.cycle == 0 ? 3 : 0;
    case 0xa00:
        // CycleBase::cycleTop (0x8033b048).
        m.taken[m.cycle] = 0;
        return 0;
    case 0xb00:
        m.length = 0xffff;
        return 0;
    case 0xc00:
        return ConductorPlay(t, number);
    default:
        // CycleBase::checkCloser (0x8033b214): 1 lets the round's last phrase sound on.
        if (mod.in_order == 1 && m.taken[m.cycle] != m.length_count)
        {
            return 0;
        }
        return m.steps[m.cycle] == count && m.cycle == 0 ? 1 : 0;
    }
}

uint16_t Performer::ConductorBeat()
{
    // AutoBgmSeqTrackRoot::beatUpdate (0x8033d374): every other call is a beat, and each track's module counts it. The
    // answer is the conductor's time between calls.
    beat_top_ = !beat_top_;
    if (beat_top_)
    {
        for (std::size_t i = 0; i < conductor_tracks_.size(); i++)
        {
            ConductorTrackState& state = conductor_tracks_[i];
            if (state.module < state.modules.size())
            {
                state.modules[state.module].beats++;
            }
        }
    }

    return sound_.conductor->poll;
}

uint16_t Performer::ConductorLength(int t, std::size_t number, bool in_order)
{
    // A length for the round: the next of the module's list, or one at random, or the followed track's last. No code
    // of the game makes a melody arrangement (PSAutoBgm::MeloArr_RandomAvoid has no constructor), so the bit that
    // would skip a phrase (OnCycle::avoidCheck, 0x8033ba98) stays clear.
    const ConductorTrack& track = sound_.conductor->tracks[number];
    ConductorTrackState& state = conductor_tracks_[number];
    ConductorModuleState& m = state.modules[state.module];
    uint16_t length = 0;
    if (track.follows != 0x10)
    {
        if (in_order)
        {
            m.taken[m.cycle]++;
        }
        const ConductorTrackState& other = conductor_tracks_[track.follows];
        length = other.module < other.modules.size() ? other.modules[other.module].length : 0;
    }
    else
    {
        const uint32_t index = in_order ? m.taken[m.cycle]++ : ConductorRandom(m.length_count) & 0xffff;
        length = static_cast<uint16_t>(Data16(m.length_list + 2u * index));
    }
    m.length = length;
    WritePortApp(t, 8, length);
    return length;
}

void Performer::ConductorTip(int t, std::size_t number)
{
    // OnCycle::setTip (0x8033b7c8): the phrase to play, at random, or the followed track's last. A phrase that the
    // last two draws both gave is drawn again from the others. The phrase's offset goes to ports 6 and 7.
    const ConductorTrack& track = sound_.conductor->tracks[number];
    ConductorTrackState& state = conductor_tracks_[number];
    ConductorModuleState& m = state.modules[state.module];
    uint16_t phrase = 0;
    if (track.follows != 0x10)
    {
        const ConductorTrackState& other = conductor_tracks_[track.follows];
        phrase = other.module < other.modules.size() ? other.modules[other.module].phrase : 0;
    }
    else if (m.phrase_count > 1)
    {
        phrase = static_cast<uint16_t>(ConductorRandom(m.phrase_count));
        if (m.history_count == 2 && m.history[0] == m.history[1] && m.history[0] == phrase)
        {
            const uint16_t same = m.history[0];
            phrase = static_cast<uint16_t>(ConductorRandom(m.phrase_count - 1u));
            if (phrase >= same)
            {
                phrase++;
            }
        }
        m.history[0] = m.history_count == 2 ? m.history[1] : m.history[0];
        m.history[m.history_count == 0 ? 0 : 1] = phrase;
        m.history_count = static_cast<uint8_t>(std::min(m.history_count + 1, 2));
    }
    m.phrase = phrase;
    const uint32_t offset = Data24(m.phrase_list + 3u * phrase);
    WritePortApp(t, 6, offset >> 16);
    WritePortApp(t, 7, offset & 0xffff);
}

uint16_t Performer::ConductorPlay(int t, std::size_t number)
{
    // CycleBase::play (0x8033b058) and OnCycle::play (0x8033b468). A round of phrases answers 2 or 0, after it sets a
    // phrase and a length. A round of rests answers 3 or 1, after it sets a length. A round that takes its lengths in
    // order answers 4 once it has taken them all.
    const ConductorModule& mod = sound_.conductor->tracks[number].modules[conductor_tracks_[number].module];
    ConductorModuleState& m = conductor_tracks_[number].modules[conductor_tracks_[number].module];
    const bool in_order = mod.in_order == 1;
    if (in_order && m.taken[m.cycle] == m.length_count)
    {
        return 4;
    }
    if (m.cycle == 0)
    {
        if (in_order)
        {
            ConductorLength(t, number, true);
            ConductorTip(t, number);
            return 2;
        }
        ConductorTip(t, number);
        ConductorLength(t, number, false);
        return 0;
    }
    ConductorLength(t, number, in_order);
    return in_order ? 3 : 1;
}

bool Performer::ConductorDone() const
{
    // A conductor moves each track through its modules, then draws its last module's phrases at random without end. A
    // piece with a conductor plays on until each track has played its last module for the module's beats.
    if (!sound_.conductor)
    {
        return true;
    }
    for (std::size_t i = 0; i < conductor_tracks_.size(); i++)
    {
        const ConductorTrack& track = sound_.conductor->tracks[i];
        const ConductorTrackState& state = conductor_tracks_[i];
        if (track.modules.empty())
        {
            continue;
        }
        if (state.module + 1u != track.modules.size() ||
            state.modules[state.module].beats < track.modules[state.module].beats)
        {
            return false;
        }
    }

    return true;
}

} // namespace ntdlib::sequence_export::jaudio

// SPDX-License-Identifier: MIT

// Checks the model of Bezel Engine's music sync without game data: a stream file with regions and markers, and a
// jump setting, made here. The stream plays an intro, a part A and a part B, then loops back to A. A beat of 4/4 at 120
// beats a minute starts at 0 and again at A. B changes to 3/4 at 90. Chords fall on bar lines and inside bars. The
// expected values come from runs of the game's routines (fn_c9870, fn_cb18c and fn_103b80 in Super Mario Party
// Jamboree's main) on the same markers, a frame of 240 samples at a time. In the first stream, the frame after the jump
// back lands on A's first sample. The routines then miss the beat and the chord there, and the counters run on in 3/4.
// In the second, B is 100 samples longer, and the frame after the jump lands past A's start.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nx/bex/music_sync.h"

using namespace xsflib::testing;

namespace
{

namespace bex = ntdlib::nx::bex;

void PutName(std::vector<uint8_t>& b, std::size_t at, const std::string& name)
{
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        b[at + i] = static_cast<uint8_t>(name[i]);
    }
}

const std::vector<bex::StreamMarker> kMarkers = {
    {0, "MRK_BEAT#4/4#120"},     {0, "MRK_CHORD#101"},      {1000, "MRK_GVAR#8#1"},    {96000, "MRK_BEAT#4/4#120"},
    {96000, "MRK_CHORD#105"},    {144000, "MRK_CHORD#203"}, {192000, "MRK_CHORD#110"}, {250000, "CUE"},
    {288000, "MRK_BEAT#3/4#90"}, {288000, "MRK_CHORD#701"}};

// The regions, with B ending at `b_end`.
std::vector<bex::StreamRegion> Regions(uint32_t b_end)
{
    return {{0, 96000, "R_INTRO"}, {96000, 288000, "R_A"}, {288000, b_end, "R_B"}, {b_end, 400000, "R_END"}};
}

// A stream file of the Switch with INFO, REGN and MRKR, laid out as sar/stream_file.h and music_sync.h describe them.
std::vector<uint8_t> StreamFile(const std::vector<bex::StreamRegion>& regions, uint32_t sample_rate = 48000)
{
    std::vector<uint8_t> info(0x40);
    PutName(info, 0, "INFO");
    xsflib::StoreLe32(&info[4], 0x40);
    xsflib::StoreLe16(&info[8], 0x4100);
    xsflib::StoreLe32(&info[12], 0x18);
    xsflib::StoreLe32(&info[0x20 + 4], sample_rate);
    xsflib::StoreLe32(&info[0x20 + 12], 400000);

    std::vector<uint8_t> regn(0x20 + 0x100 * regions.size());
    PutName(regn, 0, "REGN");
    xsflib::StoreLe32(&regn[4], static_cast<uint32_t>(regn.size()));
    for (std::size_t i = 0; i < regions.size(); ++i)
    {
        xsflib::StoreLe32(&regn[0x20 + 0x100 * i], regions[i].start);
        xsflib::StoreLe32(&regn[0x24 + 0x100 * i], regions[i].end);
        PutName(regn, 0x20 + 0x100 * i + 0xC0, regions[i].name);
    }

    std::vector<uint8_t> mrkr(12 + 0x48 * kMarkers.size());
    PutName(mrkr, 0, "MRKR");
    xsflib::StoreLe32(&mrkr[4], static_cast<uint32_t>(mrkr.size()));
    xsflib::StoreLe32(&mrkr[8], static_cast<uint32_t>(kMarkers.size()));
    for (std::size_t i = 0; i < kMarkers.size(); ++i)
    {
        xsflib::StoreLe32(&mrkr[12 + 0x48 * i], kMarkers[i].position);
        PutName(mrkr, 12 + 0x48 * i + 8, kMarkers[i].name);
    }

    std::vector<uint8_t> file(0x40);
    PutName(file, 0, "FSTM");
    xsflib::StoreLe16(&file[4], 0xFEFF);
    xsflib::StoreLe16(&file[6], 0x40);
    xsflib::StoreLe32(&file[8], 0x00060400);
    xsflib::StoreLe16(&file[0x10], 3);
    const std::vector<std::pair<uint16_t, const std::vector<uint8_t>*>> blocks = {
        {0x4000, &info}, {0x4003, &regn}, {0x4005, &mrkr}};
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        xsflib::StoreLe16(&file[0x14 + 12 * i], blocks[i].first);
        xsflib::StoreLe32(&file[0x18 + 12 * i], static_cast<uint32_t>(file.size()));
        xsflib::StoreLe32(&file[0x1C + 12 * i], static_cast<uint32_t>(blocks[i].second->size()));
        file.insert(file.end(), blocks[i].second->begin(), blocks[i].second->end());
    }
    xsflib::StoreLe32(&file[0xC], static_cast<uint32_t>(file.size()));

    return file;
}

// A string of MessagePack.
std::vector<uint8_t> Str(const std::string& s)
{
    std::vector<uint8_t> b = {static_cast<uint8_t>(0xA0 | s.size())};
    b.insert(b.end(), s.begin(), s.end());
    return b;
}

void Append(std::vector<uint8_t>& to, const std::vector<uint8_t>& b)
{
    to.insert(to.end(), b.begin(), b.end());
}

// A region sequence as a map of 16-bit size, with empty strings after its regions.
std::vector<uint8_t> Sequence(uint8_t key, const std::string& label, const std::string& next,
                              const std::vector<std::string>& regions)
{
    std::vector<uint8_t> b = {0xDE, 0x00, 0x07};
    Append(b, Str("key"));
    b.push_back(key);
    Append(b, Str("label"));
    Append(b, Str(label));
    Append(b, Str("param1"));
    Append(b, Str(next));
    Append(b, Str("param2"));
    Append(b, Str("FALSE"));
    for (int p = 3; p <= 5; ++p)
    {
        Append(b, Str("param" + std::to_string(p)));
        Append(b, Str(p - 3 < static_cast<int>(regions.size()) ? regions[p - 3] : ""));
    }

    return b;
}

std::vector<uint8_t> JumpSetting()
{
    std::vector<uint8_t> b = {0x82};
    Append(b, Str("system"));
    b.push_back(0x81);
    Append(b, Str("ParamCount"));
    b.push_back(18);
    Append(b, Str("SM_TEST"));
    b.push_back(0x94);
    Append(b, Sequence(0, "SEQ_INTRO", "SEQ_A", {"R_INTRO"}));
    Append(b, Sequence(1, "SEQ_A", "SEQ_B", {"R_A"}));
    Append(b, Sequence(2, "SEQ_B", "SEQ_A", {"R_B"}));
    Append(b, Sequence(3, "SEQ_STOP", "FALSE", {"R_END"}));

    return b;
}

// A beat that procedure 2 reports: its frame, the count of quarter notes, the chord and the tempo.
struct Reported
{
    uint32_t frame;
    int16_t count;
    int16_t chord;
    int16_t tempo;
};

// Checks the writes of procedures 2 to 4 against the beats that the game's routines reported. Procedure 4 runs after
// each reported beat in the game. Its last write up to the beat's frame gives the tempo there.
void CheckWrites(const std::vector<bex::ProcedureWrite>& writes, const std::vector<Reported>& expected,
                 const std::string& what)
{
    std::vector<Reported> got;
    int16_t tempo = 0;
    std::size_t i = 0;
    bool start_ok = writes.size() >= 2 && writes[0] == bex::ProcedureWrite{0, 4, 15, expected[0].tempo} &&
                    writes[1] == bex::ProcedureWrite{0, 3, 13, expected[0].count};
    for (; i < writes.size(); ++i)
    {
        const bex::ProcedureWrite& w = writes[i];
        if (w.procedure == 4)
        {
            tempo = w.value;
        }
        else if (w.procedure == 2 && w.variable == 14 && i + 2 < writes.size())
        {
            const bool shaped = w.value == -1 && writes[i + 1].frame == w.frame && writes[i + 1].procedure == 2 &&
                                writes[i + 1].variable == 13 && writes[i + 2].frame == w.frame &&
                                writes[i + 2].procedure == 2 && writes[i + 2].variable == 12;
            start_ok = start_ok && shaped;
            int16_t at_beat = tempo;
            for (std::size_t j = i + 3; j < writes.size() && writes[j].frame == w.frame; ++j)
            {
                if (writes[j].procedure == 4)
                {
                    at_beat = writes[j].value;
                }
            }
            got.push_back({w.frame, writes[i + 1].value, writes[i + 2].value, at_beat});
            i += 2;
        }
    }

    bool same = got.size() + 1 == expected.size();
    for (std::size_t k = 0; same && k < got.size(); ++k)
    {
        const Reported& e = expected[k + 1];
        same = got[k].frame == e.frame && got[k].count == e.count && got[k].chord == e.chord && got[k].tempo == e.tempo;
    }
    Check(start_ok && same,
          what + ": the procedures' writes match the game's routines (" + std::to_string(got.size()) + " beats)");
}

void TestReaders()
{
    const std::vector<uint8_t> file = StreamFile(Regions(384000));
    const bex::StreamCues cues = bex::ReadStreamCues(file);
    XSFLIB_CHECK(cues.sample_rate == 48000);
    XSFLIB_CHECK(cues.regions == Regions(384000));
    XSFLIB_CHECK(cues.markers == kMarkers);

    const std::vector<bex::RegionSequence> sequences = bex::ReadJumpSetting(JumpSetting());
    const std::vector<bex::RegionSequence> expected = {{"SEQ_INTRO", "SEQ_A", {"R_INTRO"}},
                                                       {"SEQ_A", "SEQ_B", {"R_A"}},
                                                       {"SEQ_B", "SEQ_A", {"R_B"}},
                                                       {"SEQ_STOP", "FALSE", {"R_END"}}};
    XSFLIB_CHECK(sequences == expected);

    const bex::RegionOrder order = bex::PlayOrder(cues, sequences);
    XSFLIB_CHECK((order.regions == std::vector<std::size_t>{0, 1, 2}) && order.loop == 1);

    bool refused = false;
    try
    {
        bex::ReadStreamCues(std::span<const uint8_t>(file).first(0x30));
    }
    catch (const xsflib::FormatError&)
    {
        refused = true;
    }
    Check(refused, "a stream file cut inside its blocks is refused");
}

void TestLandingOnTheTarget()
{
    const std::vector<uint8_t> file = StreamFile(Regions(384000));
    const bex::StreamCues cues = bex::ReadStreamCues(file);
    const bex::RegionOrder order = bex::PlayOrder(cues, bex::ReadJumpSetting(JumpSetting()));
    const std::vector<bex::SyncFrame> frames = bex::RunMusicSync(cues, order, 240);
    XSFLIB_CHECK(frames.size() == 2800);
    if (frames.size() != 2800)
    {
        return;
    }

    using Counters = std::array<int32_t, 8>;
    XSFLIB_CHECK(frames[0].counters == Counters{} && frames[0].chord == 0xFFFF && frames[0].tempo == 120);
    XSFLIB_CHECK((frames[1].counters == Counters{1, 1, 1, 1, 1, 1, 1, 1}));
    XSFLIB_CHECK((frames[1199].counters == Counters{1, 2, 4, 8, 12, 16, 24, 32}) && frames[1199].chord == 110);
    XSFLIB_CHECK((frames[1600].counters == Counters{1, 2, 3, 6, 9, 12, 18, 24}) && frames[1600].chord == 701 &&
                 frames[1600].tempo == 90);
    XSFLIB_CHECK((frames[2799].counters == Counters{1, 2, 3, 6, 9, 12, 18, 24}) && frames[2799].chord == 701);

    const std::vector<Reported> expected = {
        {0, 0, -1, 120},    {1, 1, -1, 120},     {100, 2, -1, 120},   {200, 3, -1, 120},  {300, 4, -1, 120},
        {400, 1, 105, 120}, {500, 2, 105, 120},  {600, 3, 203, 120},  {700, 4, 203, 120}, {800, 1, 110, 120},
        {900, 2, 110, 120}, {1000, 3, 110, 120}, {1100, 4, 110, 120}, {1200, 1, 701, 90}, {1334, 2, 701, 90},
        {1467, 3, 701, 90}, {1800, 1, 203, 90},  {1934, 2, 203, 90},  {2067, 3, 110, 90}, {2400, 1, 701, 90},
        {2534, 2, 701, 90}, {2667, 3, 701, 90}};
    CheckWrites(bex::UserProcedureWrites(frames), expected, "landing on the jump's target");
}

void TestLandingPastTheTarget()
{
    const std::vector<uint8_t> file = StreamFile(Regions(384100));
    const bex::StreamCues cues = bex::ReadStreamCues(file);
    const bex::RegionOrder order = bex::PlayOrder(cues, bex::ReadJumpSetting(JumpSetting()));
    const std::vector<bex::SyncFrame> frames = bex::RunMusicSync(cues, order, 240);
    XSFLIB_CHECK(frames.size() == 2801);
    if (frames.size() != 2801)
    {
        return;
    }

    using Counters = std::array<int32_t, 8>;
    XSFLIB_CHECK((frames[1600].counters == Counters{1, 2, 3, 6, 9, 12, 18, 24}) && frames[1600].chord == 701);
    XSFLIB_CHECK((frames[1601].counters == Counters{1, 1, 1, 1, 1, 1, 1, 1}) && frames[1601].chord == 105 &&
                 frames[1601].tempo == 120);
    XSFLIB_CHECK((frames[2799].counters == Counters{1, 2, 3, 6, 9, 12, 18, 24}) && frames[2799].chord == 701);

    const std::vector<Reported> expected = {
        {0, 0, -1, 120},     {1, 1, -1, 120},     {100, 2, -1, 120},   {200, 3, -1, 120},   {300, 4, -1, 120},
        {400, 1, 105, 120},  {500, 2, 105, 120},  {600, 3, 203, 120},  {700, 4, 203, 120},  {800, 1, 110, 120},
        {900, 2, 110, 120},  {1000, 3, 110, 120}, {1100, 4, 110, 120}, {1200, 1, 701, 90},  {1334, 2, 701, 90},
        {1467, 3, 701, 90},  {1601, 1, 105, 120}, {1701, 2, 105, 120}, {1801, 3, 203, 120}, {1901, 4, 203, 120},
        {2001, 1, 110, 120}, {2101, 2, 110, 120}, {2201, 3, 110, 120}, {2301, 4, 110, 120}, {2401, 1, 701, 90},
        {2534, 2, 701, 90},  {2668, 3, 701, 90}};
    CheckWrites(bex::UserProcedureWrites(frames), expected, "landing past the jump's target");
}

// Procedure 2 reads a counter through the table at 0x15d6798. Variable 14 = 1 reads the halves, and 5 has no counter.
void TestCounterTable()
{
    std::vector<bex::SyncFrame> frames(3);
    frames[0].has_beat = true;
    frames[1].has_beat = true;
    frames[1].counters = {1, 2, 3, 4, 5, 6, 7, 8};
    frames[2] = frames[1];
    const std::vector<bex::ProcedureWrite> halves = bex::UserProcedureWrites(frames, 1);
    const std::vector<bex::ProcedureWrite> expected = {
        {0, 4, 15, 0}, {0, 3, 13, 0}, {1, 2, 14, -1}, {1, 2, 13, 1}, {1, 2, 12, -1}};
    XSFLIB_CHECK(halves == expected);

    bool refused = false;
    try
    {
        bex::UserProcedureWrites(frames, 5);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
}

// A play over a stream (PlaySynced) runs the sync a frame of the game at a time: 60 a second, 800 samples at 48 kHz.
// Its writes come in the first frame of 5 ms at or after the game's frame. The pass and the loop take 840 frames of the
// game, 2800 of 5 ms. The writes go on for the extra frames while the loop plays again. A stream at 44 kHz has no whole
// number of samples in a 60th of a second, and is refused. The table recognizes no other archive, not even one of the
// right size.
void TestPlaySynced()
{
    const bex::SyncedPiece piece = {"SQ_TEST", 4, {}};
    const std::vector<uint8_t> file = StreamFile(Regions(384000));
    const bex::SyncedPlay play = bex::PlaySynced(piece, file, JumpSetting(), 300);
    const bex::StreamCues cues = bex::ReadStreamCues(file);
    const bex::RegionOrder order = bex::PlayOrder(cues, bex::ReadJumpSetting(JumpSetting()));
    std::vector<bex::SyncFrame> frames = bex::RunMusicSync(cues, order, 800, 3);
    XSFLIB_CHECK(bex::RunMusicSync(cues, order, 800, 2).size() == 840 && frames.size() > 931);
    frames.resize(931);
    std::vector<bex::ProcedureWrite> expected = bex::UserProcedureWrites(frames);
    for (bex::ProcedureWrite& write : expected)
    {
        write.frame = static_cast<uint32_t>((write.frame * 10 + 2) / 3);
    }
    XSFLIB_CHECK(play.frames == 2800 && play.writes == expected);
    XSFLIB_CHECK(!play.writes.empty() && play.writes.back().frame > 2800 && play.writes.back().frame <= 3100);

    bool refused = false;
    try
    {
        bex::PlaySynced(piece, StreamFile(Regions(384000), 44000), JumpSetting(), 300);
    }
    catch (const xsflib::FormatError&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
    XSFLIB_CHECK(bex::SyncedPieces(file).empty() && bex::SyncedPieces(std::vector<uint8_t>(462'848)).empty());
}

} // namespace

int main()
{
    TestReaders();
    TestLandingOnTheTarget();
    TestLandingPastTheTarget();
    TestCounterTable();
    TestPlaySynced();

    return xsflib::testing::Summary("nx_bex_music_sync");
}

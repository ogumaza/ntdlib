// SPDX-License-Identifier: MIT

// Tests of sequence_export on synthetic archives: the end of a performance, held sounds, LFOs, the main loop and the
// sequence variables. Each test runs on every console whose engine it describes, with the settings of that console (see
// sequence_export_test.h).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "seqlib/midi/encoder.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;

namespace
{

void TestClosingTickReachesTheReleaseTail()
{
    // Release 100 and key 60 for one tick, then volume 0 and FIN in the next. Like the end of a tick, closing the track
    // gives the releasing note the track's settings. The tail is therefore silent in the game and requires CC7 0 in
    // MIDI.
    const Performance p =
        PerformOnBank({0xd3, 0x64, 0x3c, 0x64, 0x01, 0xc1, 0x00, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    const auto silenced = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kVolume && e.value == 0 && e.tick == 1;
    };
    XSFLIB_CHECK(std::ranges::any_of(p.tracks[0], silenced));
}

void TestSilenceAtTheTimeLimitEndsWithTheLastSound()
{
    // Key 60 plays for 48 ticks (0.5 s), then the track waits 100,000 ticks (17 minutes), past the 15-minute limit. The
    // track has been silent since the note. The performance therefore treats the sequence as finished and ends with the
    // note.
    const Performance p =
        PerformOnBank({0x3c, 0x64, 0x30, 0x80, 0x86, 0x8d, 0x20, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}

void TestOneShotWavePlaysOutAtItsPitch()
{
    // Bend key 60 up by 127/128 of a 12-semitone range. That shortens a 4 s one-shot to about 2 s. Compare waiting for
    // completion with disabling notewait and setting tempo 0 immediately. The wave must finish at the same time.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xff});
    files[1].data = Fseq({0xc7, 0x00, 0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(4 * 32000, std::vector<int16_t>(4 * 32000, 1000))});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance waited = Perform(archive, 0, banks, {});
    const Performance stopped = Perform(archive, 1, banks, {});

    XSFLIB_CHECK(waited.seconds > 2.0 && waited.seconds < 2.1 && !stopped.holds &&
                 std::fabs(stopped.seconds - waited.seconds) < 0.011);
}

void TestTimedSweepGlidesOnWhenTheSequenceStops()
{
    // With notewait off, hold key 60 with about 1 s of portamento from key 48, then set tempo 0. The millisecond sweep
    // must continue. In the second sequence, hold key 60, play key 72 for one tick at release 127 with about 2 s of
    // portamento from key 48, then stop timing at tick 1. Key 72 has ended. Its sweep must stop too.
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x17, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[1].data = Fseq({0xc7, 0x00, 0x3c, 0x64, 0x00, 0xd3, 0x7f, 0xc9, 0x30, 0xcf, 0x17,
                          0x48, 0x64, 0x01, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance gliding = Perform(archive, 0, banks, {});
    const Performance ended = Perform(archive, 1, banks, {});

    auto pitch = [](const Performance& p)
    {
        std::vector<std::pair<uint32_t, float>> events;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kPitch)
            {
                events.emplace_back(e.tick, e.semitones);
            }
        }

        return events;
    };
    const auto glide = pitch(gliding);
    const auto after_end = pitch(ended);

    XSFLIB_CHECK(gliding.holds && glide.size() > 50 && glide.front().second == -12.0f && glide.back().second == 0.0f &&
                 glide.back().first < 100);
    XSFLIB_CHECK(ended.holds && !after_end.empty() && after_end.back().second == 0.0f && after_end.back().first <= 2);
}

void TestTimedSweepMovesEveryFrame()
{
    // With note wait off, hold key 60 with portamento from key 48 over 120 ms (time 8). Each sound frame moves the
    // sweep on by an envelope step. A step is 3 ms on the Wii U and 5 ms on the Switch and the 3DS. The glide therefore
    // takes 40 frames on the Wii U and 24 on the others. A tick at tempo 120 takes 10.4 ms, over several of the game's
    // frames, and the sweep moves on in each. Every frame from the note's frame to the one in which it reaches the key
    // sends a pitch, placed where the frame starts.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0x3c, 0x64, 0x00, 0x80, 0x30, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<std::pair<double, float>> pitches = PitchPlaces(p);

    XSFLIB_CHECK(GlidesEveryFrame(pitches) && !pitches.empty() && pitches[0].first == 0.0);
}

void TestTimedSweepGoesOnAfterItsTrackCloses()
{
    // Track 0 opens track 1 and waits 48 ticks. Track 1 holds key 60 at release 100 with portamento from key 48 over
    // 120 ms, and closes 2 ticks later. Closing releases the note. Like the game's channels, the note's tail goes on
    // gliding to the key. The second sequence has track 1 alone. Closing that track ends the sequence, and the tail's
    // pitches go on after the last tick.
    const std::vector<uint8_t> kGlide = {0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0xd3,
                                         0x64, 0x3c, 0x64, 0x00, 0x80, 0x02, 0xff};
    std::vector<uint8_t> with_track_0 = Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00000b), 0x80, 0x30, 0xff});
    with_track_0.insert(with_track_0.end(), kGlide.begin(), kGlide.end());
    std::vector<ArchiveFile> files(4);
    files[0].data = Fseq(with_track_0);
    files[1].data = Fseq(kGlide);
    files[2].data = OneRegionFbnk({{0x05000000, 0}});
    files[3].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance closed = Perform(archive, 0, banks, {});
    const Performance ended = Perform(archive, 1, banks, {});

    XSFLIB_CHECK(GlidesEveryFrame(PitchPlaces(closed, 1)));
    const std::vector<std::pair<double, float>> after_end = PitchPlaces(ended);
    XSFLIB_CHECK(GlidesEveryFrame(after_end) && after_end.back().first > 2.0 && ended.end_tick >= 12);
}

void TestHeldNoteHoldsUntilStopped()
{
    // Key 60 for 10,000 ticks with note wait on, 104 s at tempo 120. sequence_export counts a note of 9,600 ticks or
    // more as one that the game stops itself. The sequence therefore holds and ends 10 s after it last changes. A note
    // of 9,599 ticks plays out.
    const Performance held = PerformOnBank({0x3c, 0x64, 0xce, 0x10, 0xff}, OneRegionFbnk({{0x05000000, 0}}));
    const Performance played = PerformOnBank({0x3c, 0x64, 0xca, 0x7f, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(held.holds && !held.truncated && held.seconds >= 10.0 && held.seconds < 10.1);
    XSFLIB_CHECK(!played.holds && played.seconds > 99.0);
}

void TestLateChangeAfterTheSoundIsLeftOut()
{
    // Key 60 for 48 ticks (0.5 s at tempo 120), a wait, then tempo 60 and the end. In the first sequence the wait takes
    // 944 ticks: the change comes 9.8 s after the sound and ends the performance, at 10.3 s. In the second it takes
    // 1,200 ticks: the change comes 12.5 s after the sound, more than kHoldSeconds (EndsAtLastSound). The performance
    // then ends with the sound, and nothing comes after its end.
    const Performance near = PerformOnBank(Code({0x3c, 0x64, 0x30, 0x80, 0x87, 0x30, 0xe1, U16(60), 0xff}),
                                           OneRegionFbnk({{0x05000000, 0}}));
    const Performance far = PerformOnBank(Code({0x3c, 0x64, 0x30, 0x80, 0x89, 0x30, 0xe1, U16(60), 0xff}),
                                          OneRegionFbnk({{0x05000000, 0}}));

    const auto after_end = [&far](const TrackEvent& e)
    {
        return e.tick > far.end_tick;
    };
    XSFLIB_CHECK(!near.loop && !near.holds && std::fabs(near.seconds - 10.333) < 0.02);
    XSFLIB_CHECK(!far.loop && !far.holds && !far.truncated && NoteCount(far) == 1 && far.seconds < 1.0);
    XSFLIB_CHECK(far.tempo.empty() || far.tempo.back().tick <= far.end_tick);
    for (const std::vector<TrackEvent>& events : far.tracks)
    {
        XSFLIB_CHECK(std::ranges::none_of(events, after_end));
    }
}

// Performs `commands` as PerformOnBank does. The console's settings apply, with `sends` and `reverb_moves_loops`.
Performance PerformWithSends(const std::vector<uint8_t>& commands, Sends sends, bool reverb_moves_loops)
{
    Settings settings = console->settings;
    settings.sends = sends;
    settings.reverb_moves_loops = reverb_moves_loops;

    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(commands);
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {{.banks = {0x03000000}}}, .banks = {1}, .wave_archives = {2}}), settings.archive);
    BankSet banks(archive, archive.Sounds()[0], settings);
    return ntdlib::sequence_export::Perform(archive, 0, banks, {}, settings);
}

// Whether `a` and `b` end at the same point and have the same events apart from CC91.
bool SameEndAndEventsApartFromCc91(const Performance& a, const Performance& b)
{
    const auto same = [](const TrackEvent& x, const TrackEvent& y)
    {
        return x.tick == y.tick && x.kind == y.kind && x.key == y.key && x.value == y.value && x.preset == y.preset &&
               x.semitones == y.semitones && x.note == y.note && x.part == y.part && x.after == y.after;
    };
    const auto is_reverb = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kReverb;
    };
    for (std::size_t t = 0; t < a.tracks.size(); t++)
    {
        std::vector<TrackEvent> x = a.tracks[t];
        std::vector<TrackEvent> y = b.tracks[t];
        std::erase_if(x, is_reverb);
        std::erase_if(y, is_reverb);
        if (!std::ranges::equal(x, y, same))
        {
            return false;
        }
    }

    return a.end_tick == b.end_tick && a.seconds == b.seconds && a.holds == b.holds &&
           a.loop.has_value() == b.loop.has_value();
}

void TestReverbOutOfTheSearchesMovesNoEnd()
{
    // Each sequence changes send A after the last change of anything else. The first plays key 60 for 48 ticks (0.5 s).
    // It sets send A to 64 at tick 432 (4.5 s). The second turns tie on and holds key 60 for 2400 ticks (25 s). It sets
    // send A to 64 and jumps back to the note. That sound holds forever.
    //
    // With Settings::reverb_moves_loops, the change of CC91 counts. The first performance then ends at the change. The
    // second holds for 10 s after the change. It ends at 35 s. Without the setting, the conversion with send A as CC91
    // ends where the conversion without sends ends. The two have the same events apart from CC91. No CC91 comes after
    // the end.
    struct Case
    {
        std::vector<uint8_t> commands;
        bool holds = false;
        double counted_end = 0; // the end in seconds with Settings::reverb_moves_loops
    };
    const std::vector<Case> cases = {
        {{0x3c, 0x64, 0x30, 0x80, 0x83, 0x00, 0xd9, 0x40, 0x80, 0x30, 0xff}, false, 4.5},
        {Code({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0xd9, 0x40, 0x89, U24(0x000002)}), true, 35.0},
    };
    for (const Case& c : cases)
    {
        const Performance none = PerformWithSends(c.commands, Sends::kNone, true);
        const Performance counted = PerformWithSends(c.commands, Sends::kReverb, true);
        const Performance left_out = PerformWithSends(c.commands, Sends::kReverb, false);

        XSFLIB_CHECK(counted.holds == c.holds && std::fabs(counted.seconds - c.counted_end) < 0.02);
        XSFLIB_CHECK(left_out.holds == c.holds && SameEndAndEventsApartFromCc91(left_out, none));
        for (const std::vector<TrackEvent>& events : left_out.tracks)
        {
            XSFLIB_CHECK(std::ranges::none_of(events, [&](const TrackEvent& e)
                                              { return e.kind == EventKind::kReverb && e.tick > left_out.end_tick; }));
        }
    }
}

void TestNegativeWaitBlocksTheTrackForEver()
{
    // Track 0 opens track 1, then waits as many ticks as track variable 0 holds (index 32). Track variables start at -1
    // (SequenceTrack::InitParam, 0x022CABAC, and MmlParser::GetVariablePtr, 0x022CB600). A track reads commands only
    // while its wait is 0. A negative wait never counts down (SequenceTrack::ParseNextTick, 0x022B6F14). Track 0
    // therefore never reads on. Track 1 plays key 60 for 24 ticks with note wait on and jumps back. The sound repeats
    // each pass of track 1, every 24 ticks. In the second sequence, track 0 holds key 60 with note wait off and no
    // length, then waits the same way. The sound never changes again. The sound holds until the game stops it.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance looping = PerformOnBank(Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x00000c), 0xa1, 0x80, 0x20,
                                                    0xff, 0x3c, 0x64, 0x18, 0x89, U24(0x00000c)}),
                                              bank);
    const Performance held = PerformOnBank({0xc7, 0x00, 0x3c, 0x64, 0x00, 0xa1, 0x80, 0x20, 0xff}, bank);

    XSFLIB_CHECK(looping.loop && looping.loop->end - looping.loop->start == 24 && looping.loop->start % 24 == 0 &&
                 !looping.truncated);
    XSFLIB_CHECK(held.holds && !held.loop && !held.truncated);
}

void TestHoldStartsOnceTheVolumesStopMoving()
{
    // A time prefix makes volume2 (0xd5) fall from 127 to 32 over 1920 ticks (20 s). Then key 60 plays for 10,000
    // ticks with note wait on. The track waits that note out. The sound holds until the game stops it. But the ramp
    // moves volume2 a step each tick up to tick 1920 (MmlParser::CommandProc, 0x022CBF1C, and
    // SequenceTrack::ParseNextTick, 0x022B6F14). The sound changes until then. CC11 therefore reaches 32 at tick 1920.
    // The main volume (0xc2) ramps the same way, a step each tick (SequenceSoundPlayer::ParseNextTick, 0x022B70AC).
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance volume2 =
        PerformOnBank(Code({0xa3, 0xd5, 0x20, U16(0x0780), 0x3c, 0x64, 0xce, 0x10, 0xff}), bank);
    const Performance main_volume =
        PerformOnBank(Code({0xa3, 0xc2, 0x20, U16(0x0780), 0x3c, 0x64, 0xce, 0x10, 0xff}), bank);

    const auto last_expression = [](const Performance& p)
    {
        const std::vector<std::pair<uint32_t, int>> values = PartControls(p, EventKind::kExpression, 0);
        return values.empty() ? std::pair<uint32_t, int>{} : values.back();
    };
    const std::pair<uint32_t, int> kRampEnd = {1920, 32};
    XSFLIB_CHECK(volume2.holds && last_expression(volume2) == kRampEnd);
    XSFLIB_CHECK(main_volume.holds && last_expression(main_volume) == kRampEnd);
}

void TestLfoSetAfterANoteInItsTickReachesItsPreset()
{
    // With note wait off and LFO depth 40, play key 60, then set the LFO speed to 10 in the same tick. The game gives
    // the note the track's LFO settings at the end of the sound frame. The note's preset therefore has speed 10
    // (3.90625 Hz), and nothing is reported. The 3DS's presets keep the speed as the sequence gives it. The other
    // consoles' presets keep it in hertz.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xca, 0x28, 0x3c, 0x64, 0x60, 0xcb, 0x0a, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    const float speed = console->settings.engine == Engine::kCtr ? 10.0f : 3.90625f;

    XSFLIB_CHECK(preset && preset->lfo_speed == speed && p.approximations.empty());
}

void TestVolumeLfoChangedAtDepth0IsReportedWhenHeard()
{
    // With note wait off, a volume LFO and LFO depth 0, play key 60 for 192 ticks. 48 ticks in, set the LFO speed to
    // 10, and 48 ticks later raise the depth to 40. The note's preset keeps speed 16 (6.25 Hz), and the game plays its
    // LFO at speed 10. That is reported once. The 3DS's presets keep the speed as the sequence gives it. The other
    // consoles' presets keep it in hertz.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xcc, 0x01, 0xca, 0x00, 0x3c, 0x64, 0x81, 0x40, 0x80,
                                         0x30, 0xcb, 0x0a, 0x80, 0x30, 0xca, 0x28, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    const auto moved = p.approximations.find("LFO parameters changed during a note (using its note-on settings)");
    const float speed = console->settings.engine == Engine::kCtr ? 16.0f : 6.25f;

    XSFLIB_CHECK(preset && preset->lfo_speed == speed && moved != p.approximations.end() && moved->second == 1);
}

void TestPitchLfoChangedDuringANoteGoesOutAsPitchBends()
{
    // As in TestVolumeLfoChangedAtDepth0IsReportedWhenHeard, with a pitch LFO. The note plays on a part of its own,
    // with a preset without an LFO. The part's pitch bends follow the game's LFO, and nothing is reported. The game's
    // LFO peaks at depth / 128 * range semitones: 40 / 128 at range 1. Taken once a sound frame, the 3.90625 Hz sine
    // comes within 1% of its peak. The bends start once the depth is up, 96 ticks in.
    const Performance p = PerformOnBank({0xc7, 0x00, 0xca, 0x00, 0x3c, 0x64, 0x81, 0x40, 0x80, 0x30, 0xcb, 0x0a, 0x80,
                                         0x30, 0xca, 0x28, 0x80, 0x60, 0xff},
                                        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    // The pitch bends of the note's part, and the first tick at which they leave 0.
    int part = -1;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            part = e.part;
            break;
        }
    }
    float high = 0.0f;
    float low = 0.0f;
    uint32_t first = 0;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == part && e.semitones != 0.0f)
        {
            first = first == 0 ? e.tick : first;
            high = std::max(high, e.semitones);
            low = std::min(low, e.semitones);
        }
    }
    const bool reported = std::ranges::any_of(
        p.approximations, [](const auto& approximation) { return approximation.first.starts_with("LFO parameters"); });
    const float kPeak = 40.0f / 128.0f;

    XSFLIB_CHECK(preset && preset->lfo_type == 3 && !reported);
    XSFLIB_CHECK(first >= 96 && high > kPeak * 0.99f && high < kPeak * 1.001f && low < -kPeak * 0.99f &&
                 low > -kPeak * 1.001f);
}

void TestRandomLfoCurveInPitchBendsIsReportedOnce()
{
    // A pitch LFO with the random curve (0xBE 4) at depth 40. The LFO's speed changes while the note sounds (0xCB 10).
    // The note plays on a part of its own, as in TestPitchLfoChangedDuringANoteGoesOutAsPitchBends. The part's pitch
    // bends follow the curve. CurveRandom (0x022BB610) gives a number from 0 to 1, times the depth and the range: from
    // 0 to 40 / 128 semitones here. The numbers come from a copy of the random generator. The note reports the curve
    // once. The report of a curve other than a sine is for an LFO in a preset, and doesn't apply.
    const Performance p = PerformOnBank(
        {0xbe, 0x04, 0xc7, 0x00, 0xca, 0x28, 0x3c, 0x64, 0x81, 0x40, 0x80, 0x30, 0xcb, 0x0a, 0x80, 0x81, 0x10, 0xff},
        OneRegionFbnk({{0x05000000, 0}}));

    const PresetKey* preset = FirstNotePreset(p);
    int part = -1;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            part = e.part;
            break;
        }
    }
    float high = 0.0f;
    float low = 0.0f;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == part)
        {
            high = std::max(high, e.semitones);
            low = std::min(low, e.semitones);
        }
    }
    const auto random =
        p.approximations.find("random LFO curve in pitch bends (its numbers can differ from those in the game)");
    const bool other_curve = std::ranges::any_of(
        p.approximations, [](const auto& approximation) { return approximation.first.starts_with("LFO curve"); });

    XSFLIB_CHECK(preset && preset->lfo_type == 3 && random != p.approximations.end() && random->second == 1 &&
                 !other_curve);
    XSFLIB_CHECK(low == 0.0f && high > 0.0f && high < 40.0f / 128.0f * 1.001f);
}

void TestDeepTremoloIsKeptTo6dB()
{
    // A volume LFO (type 1) at depth 127 swings 6 dB per unit of range. The game can't raise a note more than 6 dB. At
    // range 2, CC1 drops from 64 to 32. That swings 6 dB in the SoundFont, and the change is reported. At range 1, CC1
    // stays at 64. A pitch LFO at range 2 keeps 64 too.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance deep = PerformOnBank({0xcc, 0x01, 0xcd, 0x02, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}, bank);
    const Performance shallow = PerformOnBank({0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}, bank);
    const Performance vibrato = PerformOnBank({0xcc, 0x00, 0xcd, 0x02, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}, bank);

    auto modulation = [](const Performance& p)
    {
        std::vector<int> values;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kModulation)
            {
                values.push_back(e.value);
            }
        }

        return values;
    };
    const char* const kReport = "volume LFO deeper than 6 dB (limited to 6 dB: it dips less than in the game)";
    XSFLIB_CHECK(modulation(deep) == std::vector<int>{32} && deep.approximations.count(kReport) == 1);
    XSFLIB_CHECK(modulation(shallow) == std::vector<int>{64} && shallow.approximations.count(kReport) == 0);
    XSFLIB_CHECK(modulation(vibrato) == std::vector<int>{64} && vibrato.approximations.count(kReport) == 0);
}

void TestNoteReleasedInItsAttackKeepsTheGamesLevel()
{
    // With attack 20, key 60 plays for 5 ticks, 52.1 ms. Its release comes after the envelope updates of the whole
    // frames in that time: 17 of 3 ms, or 10 of 5 ms. The game is below -30 dB then. The SoundFont attack, about 330 ms
    // long, has reached -16 dB. A second sequence plays the note for 200 ticks, past the attack. A SoundFont attack
    // that ramps up over t * 10^(-L / 20) reaches the game's level L dB at the release, t into the note. Only the first
    // note gets one.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance early = PerformOnBank({0xd0, 0x14, 0x3c, 0x64, 0x05, 0xff}, bank);
    const Performance late = PerformOnBank({0xd0, 0x14, 0x3c, 0x64, 0x81, 0x48, 0xff}, bank);

    EnvelopeValues env;
    env.attack = 20;
    const auto frames = static_cast<int64_t>(5 * kTickMs / console->settings.frame_ms);
    const double level_db = HeldLevel(env, frames) / 10.0;
    const double attack_ms =
        static_cast<double>(frames) * console->settings.frame_ms * std::pow(10.0, -level_db / 20.0);
    XSFLIB_CHECK(level_db < -30.0 && attack_ms > Sf2AttackMs(env));
    XSFLIB_CHECK(early.presets.size() == 1 && early.presets[0].attack_timecents &&
                 *early.presets[0].attack_timecents == ToTimecents(attack_ms, -12000, 8000));
    XSFLIB_CHECK(late.presets.size() == 1 && !late.presets[0].attack_timecents);
}

void TestTrackWaitingForAVariableGoesOn()
{
    // Track 0 opens track 1, waits 96 ticks and sets player variable 0 to 1. Track 1 polls without waits, then plays
    // key 60. The 10,000-command limit must yield each tick so the note can start at tick 96.
    const Performance p = PerformOnBank(
        Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000010), 0x80, 0x60, 0xf0, 0x80, 0x00, U16(0x0001), 0xff, 0xf0, 0x95,
              0x00, U16(0x0001), 0xa2, 0x89, U24(0x000010), 0x3c, 0x64, 0x30, 0xff}),
        OneRegionFbnk({{0x05000000, 0}}));

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    XSFLIB_CHECK(note_ons == std::vector<uint32_t>{96} && p.warnings.empty());
}

void TestLoopStartSendsTheVolumeTheGameRestores()
{
    // With note wait off, set volume 100, play key 60 for 6 ticks and wait 12. The loop then sets volume 100 again,
    // plays key 60, and after half its length sets volume 50. The first pass requires no volume event at its start, but
    // the game goes back to 100 there. The loop start therefore sends it for a MIDI player that loops on the markers.
    // The first loop is 48 ticks long and passes on to the next pass; the second is 10 s long, and the performance
    // stops at its end.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance short_loop =
        PerformOnBank(Code({0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64,
                            0x3c, 0x64, 0x18, 0x80, 0x18, 0xc1, 0x32, 0x80, 0x18, 0x89, U24(0x000009)}),
                      bank);
    const Performance long_loop =
        PerformOnBank(Code({0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64, 0x3c,
                            0x64, 0x18, 0x80, 0x83, 0x60, 0xc1, 0x32, 0x80, 0x83, 0x60, 0x89, U24(0x000009)}),
                      bank);

    auto sent_at_start = [](const Performance& p)
    {
        std::vector<uint8_t> at_zero;
        std::vector<uint8_t> at_start;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kVolume && e.tick == 0)
            {
                at_zero.push_back(e.value);
            }
            else if (e.kind == EventKind::kVolume && p.loop && e.tick == p.loop->start)
            {
                at_start.push_back(e.value);
            }
        }

        return p.loop && p.loop->start == 12 && at_zero.size() == 1 && at_start == at_zero;
    };
    XSFLIB_CHECK(sent_at_start(short_loop));
    XSFLIB_CHECK(sent_at_start(long_loop) && long_loop.loop->end == 972);
}

void TestLoopMarksAPassThatPlaysAsTheNext()
{
    // With note wait off, set volume 127 before the loop. The loop plays key 60, sets volume 64 halfway through and
    // jumps back. Its first pass plays the note at 127 and later passes at 64. The first pass sends 127 at its start. A
    // MIDI player that loops on that pass therefore plays every repeat too loud. The second pass plays as the next
    // does. In the first sequence a pass is 48 ticks, and the performance runs on to 10 s anyway. In the second a pass
    // is 10 s. The second pass plays as the third. The performance therefore plays the second pass there too. The
    // second pass ends a little before 20 s. On the Wii U and the Switch, ticks come early (UpdateTick).
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const Performance short_loop = PerformOnBank(
        Code({0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, U24(0x000004)}),
        bank);
    const Performance long_loop = PerformOnBank(Code({0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x83, 0x60, 0xc1,
                                                      0x40, 0x80, 0x83, 0x60, 0x89, U24(0x000004)}),
                                                bank);

    XSFLIB_CHECK(short_loop.loop && short_loop.loop->start == 48 && short_loop.loop->end == 96);
    XSFLIB_CHECK(long_loop.loop && long_loop.loop->start == 960 && long_loop.loop->end == 1920 &&
                 long_loop.loops == 1 && long_loop.seconds > 19.99 && long_loop.seconds < 21.0);
}

void TestLoopDoesNotStrikeATiedNoteAgain()
{
    // Turn tie on. The loop ties key 60 for a tick, pans left, ties it again for a tick, pans right and jumps back. The
    // first pass strikes the note, and later passes only tie it again. The loop is therefore the second pass. A MIDI
    // player repeats that pass without striking the note every 2 ticks.
    const Performance p = PerformOnBank(
        Code({0xc8, 0x01, 0x3c, 0x64, 0x01, 0xc0, 0x20, 0x3c, 0x64, 0x01, 0xc0, 0x60, 0x89, U24(0x000002)}),
        OneRegionFbnk({{0x05000000, 0}}));

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    XSFLIB_CHECK(note_ons == 1 && p.loop && p.loop->start == 2 && p.loop->end == 4);
}

void TestLoopThatNoPassRepeatsKeepsItsFirstPass()
{
    // Track 0 opens track 1, then plays key 60 every 960 ticks (10 s); track 1 plays key 64 every 959. Track 1's notes
    // come a tick earlier in each of track 0's passes. No pass therefore plays as the next. The loop stays on the first
    // pass, and the performance doesn't play another pass to find one.
    const Performance p = PerformOnBank(
        Code({0x88,          0x01, U24(0x000011), 0xc7, 0x00, 0x3c, 0x64, 0x18, 0x80, 0x87, 0x40,         0x89,
              U24(0x000007), 0xc7, 0x00,          0x40, 0x64, 0x18, 0x80, 0x87, 0x3f, 0x89, U24(0x000013)}),
        OneRegionFbnk({{0x05000000, 0}}), 3);

    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 960 && p.seconds < 12.0);
}

void TestPassOnAnotherPartPlaysAsTheNext()
{
    // With attack and release 122, glide from key 67 to key 83 over 30 ticks, wait 10 and jump back. Each note glides
    // while the last one's release still sounds. The notes therefore take turns on two parts, and a part that a note
    // moves to takes every setting again. The note of a pass and the note of the next pass are on different parts with
    // the same settings. So the first pass plays as the next.
    const Performance p =
        PerformOnBank({0xd0, 0x7a, 0xd3, 0x7a, 0xc9, 0x43, 0x53, 0x5a, 0x1e, 0x80, 0x0a, 0x89, 0x00, 0x00, 0x00},
                      OneRegionFbnk({{0x05000000, 0}}));

    const std::vector<int> parts = NoteParts(p);
    XSFLIB_CHECK(parts.size() > 3 && parts[0] == 0 && parts[1] == 1 && parts[2] == 0);
    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 40);
}

void TestSequenceThatFinishesHasNoLoop()
{
    // Set variable 0 to 0. Play key 60 for 24 ticks, add 1 to the variable, and jump back while it's below 3, then end.
    // The passes count as a loop on the way, but the sequence ends after three notes. It therefore has no loop.
    const Performance p =
        PerformOnBank(Code({0xf0, 0x80,        0x00, 0x00, 0x00, 0x3c,        0x64, 0x18, 0xf0,          0x81,
                            0x00, U16(0x0001), 0xf0, 0x94, 0x00, U16(0x0003), 0xa2, 0x89, U24(0x000005), 0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    XSFLIB_CHECK(note_ons == 3 && !p.loop && p.loops == 0);
}

void TestVariableOptionSetsAVariableBeforeTheStart()
{
    // Wait a tick and check again while player variable 0 isn't 1, then play key 60 and end. Variables start at -1. The
    // sequence therefore plays nothing until the game sets the variable. PerformOptions::variables sets it first.
    const std::vector<uint8_t> code =
        Code({0xf0, 0x95, 0x00, U16(0x0001), 0xa2, 0x80, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xff});
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    PerformOptions options;
    options.variables[0] = 1;

    const Performance unset = PerformOnBank(code, bank);
    const Performance set = PerformOnBank(code, bank, 1, options);

    auto note_ons = [](const Performance& p)
    {
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };
    XSFLIB_CHECK(note_ons(unset) == 0 && note_ons(set) == 1);
}

void TestTrackVariableOptionSetsATracksVariableBeforeTheStart()
{
    // The same sequence, waiting for track 0's variable 0 (index 32) to hold 1. Track variables start at -1.
    // PerformOptions::track_variables sets it after the start, before the first tick. A value for track 1 changes
    // nothing: the sequence's start doesn't allocate that track. Nor does a value for another of track 0's variables.
    const std::vector<uint8_t> code =
        Code({0xf0, 0x95, 0x20, U16(0x0001), 0xa2, 0x80, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xff});
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const auto note_ons = [&code, &bank](const std::vector<TrackVariable>& variables)
    {
        PerformOptions options;
        options.track_variables = variables;
        const Performance p = PerformOnBank(code, bank, 1, options);
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };

    XSFLIB_CHECK(note_ons({}) == 0);
    XSFLIB_CHECK(note_ons({{.track = 0, .index = 0, .value = 1}}) == 1);
    XSFLIB_CHECK(note_ons({{.track = 1, .index = 0, .value = 1}}) == 0);
    XSFLIB_CHECK(note_ons({{.track = 0, .index = 1, .value = 1}}) == 0);
}

void TestTrackVariableOptionSetsTheTracksVariable()
{
    // Wait a tick and check again while track variable 2 (34 in the commands) isn't 1. Then play key 60 and end. Super
    // Mario 3D Land's note melodies wait at each beat in the same way. Track variables start at -1.
    // PerformOptions::variables sets the variable of every track after the player allocates the tracks and before the
    // first tick. A change in a later frame sets it too: tick 21 sees a change in the frame that holds 216 ms.
    const std::vector<uint8_t> code =
        Code({0xf0, 0x95, 0x22, U16(0x0001), 0xa2, 0x80, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xff});
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    PerformOptions at_start;
    at_start.variables[34] = 1;
    PerformOptions later;
    later.changes = {{static_cast<uint64_t>(216.0 / console->settings.frame_ms), 34, 1}};

    const auto first_note = [](const Performance& p)
    {
        const auto note = std::ranges::find(p.tracks[0], EventKind::kNoteOn, &TrackEvent::kind);
        return note != p.tracks[0].end() ? std::optional<uint32_t>(note->tick) : std::nullopt;
    };
    XSFLIB_CHECK(!first_note(PerformOnBank(code, bank)));
    XSFLIB_CHECK(first_note(PerformOnBank(code, bank, 1, at_start)) == 0u);
    XSFLIB_CHECK(first_note(PerformOnBank(code, bank, 1, later)) == 21u);
}

void TestVariableChangeComesBeforeTheTicksOfItsFrame()
{
    // Wait a tick and check again while variable `variable` isn't 1, then play key 60 and end. A tick lasts 10.4 ms at
    // 120 BPM. A change in the frame that holds 216 ms comes after tick 20. Tick 20 is in an earlier frame. Tick 21
    // sees the change. A change in frame 0 comes before the first tick. The idle passes before a change at 15.005 s
    // don't end the performance. Tick 1441 sees that one.
    const auto first_note = [](uint8_t variable, double ms)
    {
        PerformOptions options;
        options.changes = {{static_cast<uint64_t>(ms / console->settings.frame_ms), variable, 1}};
        const Performance p = PerformOnBank(Code({0xf0, 0x95, variable, U16(0x0001), 0xa2, 0x80, 0x01, 0xa2, 0x89,
                                                  U24(0x000000), 0x3c, 0x64, 0x18, 0xff}),
                                            OneRegionFbnk({{0x05000000, 0}}), 1, options);
        const auto note = std::ranges::find(p.tracks[0], EventKind::kNoteOn, &TrackEvent::kind);
        return note != p.tracks[0].end() ? std::optional<uint32_t>(note->tick) : std::nullopt;
    };

    XSFLIB_CHECK(first_note(0, 216.0) == 21u);
    XSFLIB_CHECK(first_note(16, 216.0) == 21u);
    XSFLIB_CHECK(first_note(0, 0.0) == 0u);
    XSFLIB_CHECK(first_note(0, 15005.0) == 1441u);
}

void TestMainLoopStartsAfterTheLastVariableChange()
{
    // Play key 72 for 48 ticks while variable 0 is 1, else key 60, and jump back. The game sets the variable at
    // 20.25 s, in the middle of the pass from tick 1920. That pass plays key 60 to its end. The next pass plays key 72,
    // and it's the main loop. Without the change, the first pass is.
    const std::vector<uint8_t> code = Code({0xf0, 0x90, 0x00, U16(0x0001), 0xa2, 0x89, U24(0x000011), 0x3c, 0x64, 0x30,
                                            0x89, U24(0x000000), 0x48, 0x64, 0x30, 0x89, U24(0x000000)});
    PerformOptions options;
    options.changes = {{static_cast<uint64_t>(20250.0 / console->settings.frame_ms), 0, 1}};

    const Performance unchanged = PerformOnBank(code, OneRegionFbnk({{0x05000000, 0}}));
    const Performance changed = PerformOnBank(code, OneRegionFbnk({{0x05000000, 0}}), 1, options);

    const std::vector<int> keys = NoteKeys(changed);
    XSFLIB_CHECK(unchanged.loop && unchanged.loop->start == 0 && unchanged.loop->end == 48);
    XSFLIB_CHECK(changed.loop && changed.loop->start == 1968 && changed.loop->end == 2016 && changed.loops == 1);
    XSFLIB_CHECK(keys.size() == 42 && keys[40] == 60 && keys[41] == 72);
}

void TestProcedureChangeComesInsideTheCall()
{
    // In each tick, set variable 14 to 4, call user procedure 2, play key 60 if variable 14 then holds -1, wait a tick
    // and jump back. A write of -1 in procedure 2 from the frame that holds 216 ms takes effect in the call of tick 21,
    // and key 60 plays once. The next tick sets the variable to 4 again. A change before the frame of 216 ms comes
    // before tick 21's set, and the set replaces it. A write in another procedure never takes effect. The idle passes
    // before a write at 15.005 s don't end the performance. Tick 1441 takes that one.
    const std::vector<uint8_t> code = Code({0xf0, 0x80, 0x0e, U16(0x0004), 0xf0, 0xe0, U16(0x0002), 0xf0, 0x90, 0x0e,
                                            U16(0xffff), 0xa2, 0x3c, 0x64, 0x18, 0x80, 0x01, 0x89, U24(0x000000)});
    const auto notes = [&code](const PerformOptions& options)
    {
        const Performance p = PerformOnBank(code, OneRegionFbnk({{0x05000000, 0}}), 1, options);
        std::vector<uint32_t> ticks;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                ticks.push_back(e.tick);
            }
        }

        return ticks;
    };
    const auto frame = [](double ms)
    {
        return static_cast<uint64_t>(ms / console->settings.frame_ms);
    };

    PerformOptions in_procedure;
    in_procedure.procedure_changes = {{frame(216.0), 2, 14, -1}};
    PerformOptions timed;
    timed.changes = {{frame(216.0), 14, -1}};
    PerformOptions other;
    other.procedure_changes = {{frame(216.0), 3, 14, -1}};
    PerformOptions late;
    late.procedure_changes = {{frame(15005.0), 2, 14, -1}};

    XSFLIB_CHECK(notes(in_procedure) == std::vector<uint32_t>{21});
    XSFLIB_CHECK(notes(timed).empty() && notes(other).empty());
    XSFLIB_CHECK(notes(late) == std::vector<uint32_t>{1441});
}

void TestProcedureModelRunsInsideTheCall()
{
    // Set track variable 14 to 5, call user procedure 0, and play key 60 if the variable then holds 7. The model of the
    // procedure writes 7 inside the call. Without the model, the call changes nothing, and nothing plays.
    const std::vector<uint8_t> draw = Code({0xf0, 0x80, 0x2e, U16(0x0005), 0xf0, 0xe0, U16(0x0000), 0xf0, 0x90, 0x2e,
                                            U16(0x0007), 0xa2, 0x3c, 0x64, 0x18, 0x80, 0x18, 0xff});
    PerformOptions with_model;
    with_model.user_procedures =
        [](uint16_t procedure, std::span<int16_t, 16>, std::span<int16_t, 16>, std::span<int16_t, 16> track)
    {
        if (procedure == 0 && track[14] == 5)
        {
            track[14] = 7;
        }

        return false;
    };
    XSFLIB_CHECK(NoteKeys(PerformOnBank(draw, OneRegionFbnk({{0x05000000, 0}}), 1, with_model)) ==
                 std::vector<int>{60});
    XSFLIB_CHECK(NoteKeys(PerformOnBank(draw, OneRegionFbnk({{0x05000000, 0}}))).empty());

    // Set variable 3 to 0, call procedure 2, then play key 60 if variable 3 holds 2 and key 62 if global variable 1
    // holds 9. The model writes 1 to variable 3 and 9 to global variable 1. The game's write of 2 to variable 3 in the
    // same call comes after the model's.
    const std::vector<uint8_t> order =
        Code({0xf0, 0x80, 0x03, U16(0x0000), 0xf0, 0xe0, U16(0x0002), 0xf0, 0x90, 0x03, U16(0x0002), 0xa2,
              0x3c, 0x64, 0x18, 0xf0,        0x90, 0x11, U16(0x0009), 0xa2, 0x3e, 0x64, 0x18,        0xff});
    PerformOptions both;
    both.procedure_changes = {{0, 2, 3, 2}};
    both.user_procedures =
        [](uint16_t procedure, std::span<int16_t, 16> local, std::span<int16_t, 16> global, std::span<int16_t, 16>)
    {
        if (procedure == 2)
        {
            local[3] = 1;
            global[1] = 9;
        }

        return false;
    };
    XSFLIB_CHECK(NoteKeys(PerformOnBank(order, OneRegionFbnk({{0x05000000, 0}}), 1, both)) ==
                 (std::vector<int>{60, 62}));
}

void TestWaitingForAVariableIsNoLoop()
{
    // Track 1 waits 96 ticks and sets variable 0. Track 0 repeatedly sets volume and polls the variable without waits.
    // The 10,000-command limit stops partway through a pass. Later ticks then jump back to commands visited on the
    // previous tick. These are polling passes, not the main loop. After the variable is set, track 0 loops a 48-tick
    // key 60; that is the main loop.
    const Performance p =
        PerformOnBank(Code({0xfe,          U16(0x0003), 0x88, 0x01, U24(0x00001b), 0xc1, 0x7f,        0xf0, 0x95,
                            0x00,          U16(0x0001), 0xa2, 0x89, U24(0x000008), 0x3c, 0x64,        0x30, 0x89,
                            U24(0x000014), 0x80,        0x60, 0xf0, 0x80,          0x00, U16(0x0001), 0xff}),
                      OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(p.loop && p.loop->start == 97 && p.loop->end == 145 && p.warnings.empty());
}

void TestPollingInASubroutineAfterAOneShotIsNoLoop()
{
    // Call a subroutine that plays key 60 for 12 ticks, then checks each tick whether variable 0 is 1, and returns when
    // it is. Nothing sets the variable. The subroutine therefore polls for ever. The conditional jump could leave that
    // loop, and its passes play no note. The loop therefore doesn't count. The performance ends with the note and has
    // no loop markers. The same loop counts in three other subroutines: one that holds a note of 9,600 ticks on a
    // looping wave with note wait off, one that ties keys 60 and 62 in each pass, and one whose only conditional
    // command clamps variable 0 at 0 and can't leave the loop. That one holds a note of 2,000 ticks with note wait off.
    // A loop that sounds nothing doesn't count either (SilentLoop).
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});

    const Performance one_shot =
        PerformOnBank(Code({0x8a, U24(0x000005), 0xff, 0x3c, 0x64, 0x0c, 0xf0, 0x90, 0x00, U16(0x0001), 0xa2, 0x89,
                            U24(0x000018), 0x80, 0x01, 0x89, U24(0x000008), 0xfd}),
                      bank);
    const Performance held = PerformOnBank(
        Code({0xc7, 0x00,        0x8a, U24(0x000007), 0xff,          0x3c, 0x64, 0xcb, 0x00,          0xf0, 0x90,
              0x00, U16(0x0001), 0xa2, 0x89,          U24(0x00001b), 0x80, 0x01, 0x89, U24(0x00000b), 0xfd}),
        bank);
    const Performance melody =
        PerformOnBank(Code({0xc8, 0x01, 0xc7,        0x00, 0x8a, U24(0x000009), 0xff, 0x3c,          0x64,
                            0x07, 0x80, 0x07,        0x3e, 0x64, 0x07,          0x80, 0x07,          0xf0,
                            0x90, 0x00, U16(0x0001), 0xa2, 0x89, U24(0x000021), 0x89, U24(0x000009), 0xfd}),
                      bank);
    const Performance clamp = PerformOnBank(
        Code({0x8a, U24(0x000005), 0xff, 0xc7, 0x00, 0x3c, 0x64, 0x8f, 0x50, 0x80, 0x01,          0xf0, 0x94,
              0x00, 0x00,          0x00, 0xa2, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x89, U24(0x00000b), 0xfd}),
        bank);

    XSFLIB_CHECK(!one_shot.loop && one_shot.loops == 0 && one_shot.seconds < 1.0);
    XSFLIB_CHECK(held.loop && held.loop->end == held.loop->start + 1 && held.seconds > 9.0);
    XSFLIB_CHECK(melody.loop && melody.loop->end == melody.loop->start + 14 && melody.seconds < 20.0);
    XSFLIB_CHECK(clamp.loop && clamp.loop->end == clamp.loop->start + 1 && clamp.seconds < 20.0);
}

void TestLoopLeftBeforeASilentTimeLimitIsDropped()
{
    // After key 60, count variable 1 up from -1 to 8 in passes of a tick. Those passes count as a loop. Then call a
    // subroutine that polls variable 0 for ever (see TestPollingInASubroutineAfterAOneShotIsNoLoop). The performance
    // reaches the time limit in silence. That ends it as if it had finished, and a sequence that finishes has no main
    // loop.
    const Performance p =
        PerformOnBank(Code({0x3c, 0x64,          0x0c, 0xf0, 0x81,        0x01,          U16(0x0001), 0x80,
                            0x01, 0xf0,          0x94, 0x01, U16(0x0008), 0xa2,          0x89,        U24(0x000003),
                            0x8a, U24(0x000019), 0xff, 0xf0, 0x90,        0x00,          U16(0x0001), 0xa2,
                            0x89, U24(0x000029), 0x80, 0x01, 0x89,        U24(0x000019), 0xfd}),
                      OneRegionFbnk({{0x05000000, 0}}));

    XSFLIB_CHECK(!p.loop && p.loops == 0 && !p.truncated && p.seconds < 1.0);
}

void TestCountedLoopPastTheTimeLimitPlaysAsAnEndlessLoop()
{
    // Play key 60 for 1920 ticks (20 s) in a loop of 64 passes, then end. The passes take 21 minutes and 20 s, past the
    // time limit. The performance reaches the limit inside the counted loop. It runs again with the loop endless and
    // plays as the same loop with a count of 0: the first pass is the main loop. A loop of 2 passes ends before the
    // limit, and the sequence ends after its passes.
    const auto loop = [](uint8_t count)
    {
        return PerformOnBank(Code({0xd4, count, 0x3c, 0x64, 0x8f, 0x00, 0xfc, 0xff}), OneRegionFbnk({{0x05000000, 0}}));
    };
    const Performance counted = loop(64);
    const Performance endless = loop(0);
    const Performance twice = loop(2);

    MidiReport report;
    XSFLIB_CHECK(endless.loop && endless.loop->start == 0 && endless.loop->end == 1920 && endless.loops == 1 &&
                 !endless.truncated && endless.seconds < 20.1);
    XSFLIB_CHECK(!counted.truncated && counted.loops == 1 && counted.seconds == endless.seconds &&
                 WriteMidi(counted, "test", report) == WriteMidi(endless, "test", report));
    XSFLIB_CHECK(!twice.loop && twice.loops == 0 && !twice.truncated && NoteCount(twice) == 2 &&
                 twice.seconds >= 40.0 && twice.seconds < 41.0);
}

void TestNestedCountedLoopsPastTheTimeLimit()
{
    // Play key 60 for 960 ticks (10 s) in an inner loop of 2 passes, inside an outer loop of 64 passes, then end. The
    // outer passes take 21 minutes and 20 s, past the time limit. The performance reaches the limit inside both loops.
    // It runs again with the track's outermost counted loop endless. The length analysis takes the same loop. The inner
    // loop still ends after 2 passes. The main loop is a pass of the outer loop, 1920 ticks long. The sequence plays as
    // the same outer loop with a count of 0. With every counted loop endless, the inner loop would be the main loop.
    const auto loops = [](uint8_t outer)
    {
        return PerformOnBank(Code({0xd4, outer, 0xd4, 0x02, 0x3c, 0x64, 0x87, 0x40, 0xfc, 0xfc, 0xff}),
                             OneRegionFbnk({{0x05000000, 0}}));
    };
    const Performance counted = loops(64);
    const Performance endless = loops(0);

    MidiReport report;
    XSFLIB_CHECK(endless.loop && endless.loop->start == 0 && endless.loop->end == 1920 && !endless.truncated &&
                 NoteCount(endless) == 2);
    XSFLIB_CHECK(!counted.truncated && counted.loop && counted.loop->end == 1920 &&
                 counted.seconds == endless.seconds &&
                 WriteMidi(counted, "test", report) == WriteMidi(endless, "test", report));
}

void TestTiedMelodyBelowHearingIsNoLoop()
{
    // With sustain 0, decay 100 and hold 0, call a subroutine that ties keys 60 and 62 on for 7 ticks each, and returns
    // once variable 0 is 1 (see TestPollingInASubroutineAfterAOneShotIsNoLoop). Nothing sets the variable. The legatos
    // don't start the envelope again, and the channel sinks below hearing (kInaudibleSustain). From then on, the passes
    // play no note that can be heard. The conditional jump could leave the loop. The passes therefore don't count as a
    // loop. The MIDI note and the performance end where the channel has faded to kFadedAttenuation. The performance has
    // no loop markers.
    const Performance p = PerformOnBank(
        Code({0xd2,          0x00, 0xd1, 0x64, 0xb1,        0x00, 0xc8, 0x01,          0xc7, 0x00,          0x8a,
              U24(0x00000f), 0xff, 0x3c, 0x64, 0x07,        0x80, 0x07, 0x3e,          0x64, 0x07,          0x80,
              0x07,          0xf0, 0x90, 0x00, U16(0x0001), 0xa2, 0x89, U24(0x000027), 0x89, U24(0x00000f), 0xfd}),
        OneRegionFbnk({{0x05000000, 0}}));

    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    const double faded_ms = static_cast<double>(FramesToLevel(kEnvelope, -static_cast<float>(kFadedAttenuation)) + 1) *
                            console->settings.frame_ms;
    const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
    const double off_ms = off == p.tracks[0].end() ? 0.0 : off->tick * kTickMs;
    XSFLIB_CHECK(!p.loop && p.loops == 0 && !p.truncated && NoteCount(p) == 1);
    XSFLIB_CHECK(off_ms >= faded_ms && off_ms < faded_ms + kTickMs + console->settings.frame_ms);
    XSFLIB_CHECK(p.seconds * 1000.0 >= faded_ms &&
                 p.seconds * 1000.0 < faded_ms + kTickMs + console->settings.frame_ms);
}

void TestLegatoBelowHearingKeepsItsMidiNote()
{
    // With tie on, sustain 0, decay 100 and hold 0, play key 60 with note wait off. After 100 ticks the channel has
    // sunk below hearing (kInaudibleSustain) but hasn't faded to kFadedAttenuation. Its MIDI note still sounds. Then
    // tie key 62 on, once after setting release 64 and once at velocity 127. The game's legato doesn't start the
    // envelope again. Neither legato starts a new MIDI note, and velocity 127 doesn't start the note higher on a later
    // pass.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    auto at_ms = [&](int attenuation)
    {
        return static_cast<double>(FramesToLevel(kEnvelope, -static_cast<float>(attenuation)) + 1) *
               console->settings.frame_ms;
    };

    const Performance released = PerformOnBank({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64,
                                                0x01, 0x80, 0x64, 0xd3, 0x40, 0x3e, 0x64, 0x01, 0x80, 0x30, 0xff},
                                               bank);
    const Performance louder = PerformOnBank({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c,
                                              0x64, 0x01, 0x80, 0x64, 0x3e, 0x7f, 0x01, 0x80, 0x30, 0xff},
                                             bank);

    const auto on = std::ranges::find(louder.tracks[0], EventKind::kNoteOn, &TrackEvent::kind);
    XSFLIB_CHECK(at_ms(kInaudibleSustain) < 100 * kTickMs && 100 * kTickMs < at_ms(kFadedAttenuation));
    XSFLIB_CHECK(NoteCount(released) == 1 &&
                 !released.approximations.contains("legato (tie or mono) played as a new note"));
    XSFLIB_CHECK(NoteCount(louder) == 1 && on != louder.tracks[0].end() && on->value == 100);
}

void TestPollAfterTheSoundIsNoLoop()
{
    // Track 0 opens track 1, then checks each tick whether variable 0 is 1, and leaves the loop when it is. Nothing
    // sets the variable. Track 1 plays key 60 for 12 ticks and ends. Once the note has ended, track 0's passes count as
    // the main loop. They sound nothing (SilentLoop). The performance therefore ends with the note and has no loop
    // markers. When track 1 holds a note of 9,600 ticks instead, the same passes are the main loop under the note.
    const std::vector<uint8_t> bank = OneRegionFbnk({{0x05000000, 0}});
    const std::vector<uint8_t> root = Code({0xfe, U16(0x0003), 0x88, 0x01, U24(0x000019), 0x80, 0x01, 0xf0, 0x90, 0x00,
                                            U16(0x0001), 0xa2, 0x89, U24(0x000018), 0x89, U24(0x000008), 0xff});
    std::vector<uint8_t> one_shot = root;
    one_shot.insert(one_shot.end(), {0x3c, 0x64, 0x0c, 0xff});
    std::vector<uint8_t> held = root;
    held.insert(held.end(), {0x3c, 0x64, 0xcb, 0x00, 0xff});

    const Performance after = PerformOnBank(one_shot, bank);
    const Performance under = PerformOnBank(held, bank);

    XSFLIB_CHECK(!after.loop && after.loops == 0 && !after.truncated && NoteCount(after) == 1);
    XSFLIB_CHECK(after.seconds < 1.0);
    XSFLIB_CHECK(under.loop && under.loop->end == under.loop->start + 1 && under.seconds > 9.0);
}

void TestSilentPassesThatChangeSettingsEndWithTheSound()
{
    // Play key 60 for 12 ticks, then set the volume to 127 and to 64 in passes of 48 ticks. The passes change the track
    // and sound nothing (SilentLoop). The performance ends with the note. The volume changes after it are left out.
    const Performance p =
        PerformOnBank(Code({0x3c, 0x64, 0x0c, 0xc1, 0x7f, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, U24(0x000003)}),
                      OneRegionFbnk({{0x05000000, 0}}));

    const auto after_end = [&p](const TrackEvent& e)
    {
        return e.tick > p.end_tick;
    };
    XSFLIB_CHECK(!p.loop && p.loops == 0 && NoteCount(p) == 1 && p.seconds < 1.0);
    XSFLIB_CHECK(std::ranges::none_of(p.tracks[0], after_end));
}

void TestSequenceWithoutNotesEndsAtItsStart()
{
    // A sequence that only waits a tick and jumps back sounds nothing (SilentLoop). It ends at tick 0 with no loop
    // markers. Its MIDI file and SoundFont are still valid.
    std::vector<ArchiveFile> files(1);
    files[0].data = Fseq({0x80, 0x01, 0x89, 0x00, 0x00, 0x00});
    const SoundArchive archive = SoundArchive::Load(Fsar(files, {.sequences = {{0, 0}}}), console->settings.archive);

    const Conversion c = ConvertSequence(archive, 0);

    const MidiView m = ParseMidi(c.midi);
    const auto at_start = [](uint64_t end)
    {
        return end == 0;
    };
    XSFLIB_CHECK(!c.performance.loop && c.performance.loops == 0 && c.performance.end_tick == 0);
    XSFLIB_CHECK(c.performance.seconds == 0.0 && !c.performance.truncated);
    XSFLIB_CHECK(m.ok && !m.track_ends.empty() && std::ranges::all_of(m.track_ends, at_start));
    XSFLIB_CHECK(Sf2Problem(c.sf2).empty());
}

// The tests that hold on the Wii U, the Switch and the 3DS.
const Test kWiiUSwitchAndCtrTests[] = {
    TestClosingTickReachesTheReleaseTail,
    TestSilenceAtTheTimeLimitEndsWithTheLastSound,
    TestOneShotWavePlaysOutAtItsPitch,
    TestTimedSweepGlidesOnWhenTheSequenceStops,
    TestTimedSweepMovesEveryFrame,
    TestTimedSweepGoesOnAfterItsTrackCloses,
    TestHeldNoteHoldsUntilStopped,
    TestLateChangeAfterTheSoundIsLeftOut,
    TestReverbOutOfTheSearchesMovesNoEnd,
    TestLfoSetAfterANoteInItsTickReachesItsPreset,
    TestVolumeLfoChangedAtDepth0IsReportedWhenHeard,
    TestPitchLfoChangedDuringANoteGoesOutAsPitchBends,
    TestDeepTremoloIsKeptTo6dB,
    TestNoteReleasedInItsAttackKeepsTheGamesLevel,
    TestTrackWaitingForAVariableGoesOn,
    TestLoopStartSendsTheVolumeTheGameRestores,
    TestLoopMarksAPassThatPlaysAsTheNext,
    TestLoopDoesNotStrikeATiedNoteAgain,
    TestLoopThatNoPassRepeatsKeepsItsFirstPass,
    TestPassOnAnotherPartPlaysAsTheNext,
    TestSequenceThatFinishesHasNoLoop,
    TestVariableOptionSetsAVariableBeforeTheStart,
    TestTrackVariableOptionSetsATracksVariableBeforeTheStart,
    TestTrackVariableOptionSetsTheTracksVariable,
    TestVariableChangeComesBeforeTheTicksOfItsFrame,
    TestMainLoopStartsAfterTheLastVariableChange,
    TestProcedureChangeComesInsideTheCall,
    TestProcedureModelRunsInsideTheCall,
    TestWaitingForAVariableIsNoLoop,
    TestPollingInASubroutineAfterAOneShotIsNoLoop,
    TestLoopLeftBeforeASilentTimeLimitIsDropped,
    TestCountedLoopPastTheTimeLimitPlaysAsAnEndlessLoop,
    TestNestedCountedLoopsPastTheTimeLimit,
    TestTiedMelodyBelowHearingIsNoLoop,
    TestLegatoBelowHearingKeepsItsMidiNote,
    TestPollAfterTheSoundIsNoLoop,
    TestSilentPassesThatChangeSettingsEndWithTheSound,
    TestSequenceWithoutNotesEndsAtItsStart,
};

// The tests that hold on the Wii U and the Switch.
const Test kWiiUAndSwitchTests[] = {
    TestNegativeWaitBlocksTheTrackForEver,
    TestHoldStartsOnceTheVolumesStopMoving,
    TestRandomLfoCurveInPitchBendsIsReportedOnce,
};

// The tests of `lists`, in order.
std::vector<Test> Join(std::initializer_list<std::span<const Test>> lists)
{
    std::vector<Test> tests;
    for (const std::span<const Test> list : lists)
    {
        tests.insert(tests.end(), list.begin(), list.end());
    }

    return tests;
}

} // namespace

int main()
{
    RunOn(WiiU(), Join({kWiiUSwitchAndCtrTests, kWiiUAndSwitchTests}));
    RunOn(Switch(), Join({kWiiUSwitchAndCtrTests, kWiiUAndSwitchTests}));
    RunOn(Ctr(), kWiiUSwitchAndCtrTests);

    return xsflib::testing::Summary("sequence_export_loops");
}

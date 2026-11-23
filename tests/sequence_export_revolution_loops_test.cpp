// SPDX-License-Identifier: MIT

// Tests of sequence_export on the Wii's engine (Engine::kRevolution), with the Wii's settings (Wii() in
// sequence_export_test.h) and synthetic RSAR archives (sequence_export_revolution_test.h): the end of a performance,
// the voices, the main loop, the variables, and the builds of 2006 and 2008.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/reader.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_build.h"
#include "sequence_export/wii_pan.h"
#include "sequence_export_revolution_test.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export::wii_tests;

namespace
{

void TestClosingTickReachesTheReleaseTail()
{
    // Release 100 and key 60 for one tick, then volume 0 and FIN in the next. Like the end of a tick, closing the track
    // gives the released note the track's settings. The tail is then silent in the game. MIDI requires CC7 0 for that.
    const Performance p = PerformCommands({0xd3, 0x64, 0x3c, 0x64, 0x01, 0xc1, 0x00, 0xff});

    const std::vector<std::pair<uint32_t, int>> volumes = Controls(p, EventKind::kVolume);
    XSFLIB_CHECK(std::ranges::find(volumes, std::pair<uint32_t, int>{1, 0}) != volumes.end());
}

void TestSilenceAtTheTimeLimitEndsWithTheLastSound()
{
    // Key 60 plays for 48 ticks (0.5 s), then the track waits 100,000 ticks (17 minutes), past the 15-minute limit. The
    // track has been silent since the note. The performance therefore treats the sequence as finished and ends with the
    // note.
    const Performance p = PerformCommands({0x3c, 0x64, 0x30, 0x80, 0x86, 0x8d, 0x20, 0xff});

    XSFLIB_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}

void TestOneShotWavePlaysOutAtItsPitch()
{
    // Bend key 60 up by 127/128 of a 12-semitone range. The bend plays a one-shot wave of 4 s in about 2 s. With note
    // wait off, waiting for the wave and setting tempo 0 at once end the wave at the same time.
    const std::vector<int16_t> kSamples(4 * 32000, 1000);
    const SoundArchive archive =
        SequenceArchive({{0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xff},
                         {0xc7, 0x00, 0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff}},
                        {.wave = Rwav(4 * 32000, kSamples)});

    const Performance waited = PerformSound(archive, 0);
    const Performance stopped = PerformSound(archive, 1);

    XSFLIB_CHECK(waited.seconds > 2.0 && waited.seconds < 2.1 && !stopped.holds &&
                 std::fabs(stopped.seconds - waited.seconds) < 0.011);
}

// The banks of a recording: every note plays `region` and its one-shot wave of `samples` silent samples at
// `sample_rate`.
class OneRegion
{
public:
    OneRegion(const ntdlib::sar::VelocityRegion& region, uint32_t samples, uint32_t sample_rate) : region_(region)
    {
        pcm_.sample_rate = sample_rate;
        pcm_.loop_end = samples;
        pcm_.channels.emplace_back(samples, int16_t{0});
    }

    const ntdlib::sar::VelocityRegion* FindRegion(int, uint32_t, int, int) const
    {
        return &region_;
    }

    const ntdlib::sar::Pcm* GetWave(int, uint32_t)
    {
        return &pcm_;
    }

private:
    ntdlib::sar::VelocityRegion region_;
    ntdlib::sar::Pcm pcm_;
};

// The recording engine on the Wii's model, with a build of NW4R SND, `channels` channels and `voices` voices. It keeps
// the channels that lose their voices to other notes (OnChannelDropped).
class WiiRecording
    : public ntdlib::sequence_export::RecordingEngine<ntdlib::nwsnd::RevolutionPolicy, WiiRecording, OneRegion>
{
public:
    WiiRecording(OneRegion& banks, SndBuild build, int channels = ntdlib::nwsnd::RevolutionPolicy::kChannelCount,
                 int voices = ntdlib::nwsnd::RevolutionPolicy::kChannelCount)
        : RecordingEngine(banks, channels, voices), build_(build)
    {
    }

    const SndBuild& Build() const
    {
        return build_;
    }

    void OnChannelDropped(int track, Channel& channel)
    {
        dropped_.emplace_back(track, channel.place);
    }

    // The track and the place of each channel that lost its voice.
    const std::vector<std::pair<int, ntdlib::nwsnd::ChannelId>>& Dropped() const
    {
        return dropped_;
    }

private:
    SndBuild build_;
    std::vector<std::pair<int, ntdlib::nwsnd::ChannelId>> dropped_;
};

constexpr SndBuild kBuild2007{.date = 20070601};
constexpr SndBuild kBuild2008{.date = 20080620};

// Starts `sequence` on `engine` as a sound whose track 0 starts at the sequence's start, with the engine's build. The
// player reads `sequence` in place. It has to outlive the engine.
void PlaySequence(WiiRecording& engine, const std::vector<uint8_t>& sequence)
{
    ntdlib::sar::SoundInfo info;
    info.banks = {0};
    engine.Play(info, sequence);
    engine.GetPlayer().SetBuild(engine.Build());
}

// The frames from a note's frame to the frame that frees its channel, for key `key` on a one-shot wave of `samples`
// samples at `sample_rate` and a root key of 60. Returns -1 if the note never starts or never ends.
int FramesWithTheNote(int key, uint32_t samples, uint32_t sample_rate)
{
    const std::vector<uint8_t> sequence = {static_cast<uint8_t>(key), 0x7f, 0x60, 0x80, 0x60, 0xff};
    OneRegion banks({}, samples, sample_rate);
    WiiRecording engine(banks, kBuild2007);
    PlaySequence(engine, sequence);

    int note_frame = -1;
    for (int frame = 0; frame < 400; ++frame)
    {
        engine.RunFrame();
        const int channels = engine.Channels().ActiveCount();
        if (note_frame < 0 && channels > 0)
        {
            note_frame = frame;
        }
        else if (note_frame >= 0 && channels == 0)
        {
            return frame - note_frame;
        }
    }

    return -1;
}

void TestRecordingEndsAOneShotNoteAsTheWiiDoes()
{
    // AX plays a note's wave from the frame after the note's frame, 96 samples a frame at a ratio of 1. NW4R SND frees
    // the channel at the end of the frame after the one in which the wave runs out. 960 samples at 32 kHz play for 10
    // frames, at key 72 for 5, and at 16 kHz for 20.
    const int root = FramesWithTheNote(60, 960, 32000);
    const int octave_up = FramesWithTheNote(72, 960, 32000);
    const int half_rate = FramesWithTheNote(60, 960, 16000);

    XSFLIB_CHECK(root == 11);
    XSFLIB_CHECK(octave_up == 6);
    XSFLIB_CHECK(half_rate == 21);
}

// The first channel of `sequence` on a recording with `region` and build `build`, once its note has started.
ntdlib::nwsnd::Channel<WiiRecording::Policy> FirstChannel(const std::vector<uint8_t>& sequence,
                                                          const ntdlib::sar::VelocityRegion& region, SndBuild build)
{
    OneRegion banks(region, 32000, 32000);
    WiiRecording engine(banks, build);
    PlaySequence(engine, sequence);
    while (engine.Channels().ActiveCount() == 0 && engine.Frame() < 100)
    {
        engine.RunFrame();
    }

    return engine.Channels().At(engine.Channels().InUse().front());
}

void TestNoteTakesAVoiceFromAPlayingChannel()
{
    // Two channels and one voice. Keys 60 and 62 start a tick apart with notewait off. The second note takes the first
    // note's voice, and the first note's channel reports the drop (OnChannelDropped).
    const std::vector<uint8_t> kSequence = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01,
                                            0x3e, 0x64, 0x60, 0x80, 0x60, 0xff};
    OneRegion banks({}, 32000, 32000);
    WiiRecording engine(banks, kBuild2007, 2, 1);
    PlaySequence(engine, kSequence);

    for (int frame = 0; frame < 20; ++frame)
    {
        engine.RunFrame();
    }

    XSFLIB_CHECK(engine.Dropped().size() == 1 && engine.Dropped()[0].first == 0);
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 1);
}

void TestRecordingSetsANoteUpAsTheWiiDoes()
{
    // Velocity 100 on a region of volume 100: the velocity over 127, squared, times the volume over 127. The region's
    // pan of 0 and an initial pan of +2 (0xDC 0x42), each over 63, then added: nw::snd's sum over 63 differs in the
    // last bit (0xbf7befbf). The region's key group and note-off type come with the builds of 2008.
    ntdlib::sar::VelocityRegion region;
    region.volume = 100;
    region.pan = 0;
    region.key_group = 3;
    region.ignore_note_off = true;
    const std::vector<uint8_t> kSequence = {0xdc, 0x42, 0x3c, 0x64, 0x60, 0x80, 0x60, 0xff};

    const auto early = FirstChannel(kSequence, region, kBuild2007);
    const auto late = FirstChannel(kSequence, region, kBuild2008);

    XSFLIB_CHECK(std::bit_cast<uint32_t>(early.velocity_volume) == 0x3ef9f407 && early.init_volume == 1.0f);
    XSFLIB_CHECK(std::bit_cast<uint32_t>(early.init_pan) == 0xbf7befc0 && early.init_span == 0.0f);
    XSFLIB_CHECK(early.key_group == 0 && !early.ignore_note_off);
    XSFLIB_CHECK(late.key_group == 3 && late.ignore_note_off);
}

void TestTimedSweepGlidesOnWhenTheSequenceStops()
{
    // With note wait off, hold key 60 with about 1 s of portamento from key 48, then set tempo 0. The sweep, timed in
    // milliseconds, goes on. The second sequence holds key 60, plays key 72 for a tick at release 127 with about 2 s of
    // portamento from key 48, and stops its timing at tick 1. Key 72 has ended by then. Its sweep stops too.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x17, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0x3c, 0x64, 0x00, 0xd3, 0x7f, 0xc9, 0x30, 0xcf, 0x17,
                          0x48, 0x64, 0x01, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff}});

    const Performance gliding = PerformSound(archive, 0);
    const Performance ended = PerformSound(archive, 1);

    const std::vector<std::pair<uint32_t, float>> glide = Pitches(gliding);
    const std::vector<std::pair<uint32_t, float>> after_end = Pitches(ended);
    XSFLIB_CHECK(gliding.holds && glide.size() > 50 && glide.front().second == -12.0f && glide.back().second == 0.0f &&
                 glide.back().first < 100);
    XSFLIB_CHECK(ended.holds && !after_end.empty() && after_end.back().second == 0.0f && after_end.back().first <= 2);
}

void TestTimedSweepMovesEveryFrame()
{
    // With note wait off, hold key 60 with 120 ms of portamento from key 48 (time 8). A tick at tempo 120 takes 10.4
    // ms, over three of the game's 3 ms sound frames, and the sweep moves on in each: every frame from the note's frame
    // to the one in which it reaches the key sends a pitch, placed where the frame starts.
    const Performance p = PerformCommands({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0x3c, 0x64, 0x00, 0x80, 0x30, 0xff});

    const std::vector<std::pair<double, float>> pitches = PitchPlaces(p);

    XSFLIB_CHECK(GlidesEveryFrame(pitches) && !pitches.empty() && pitches[0].first == 0.0);
}

void TestTimedSweepGoesOnAfterItsTrackCloses()
{
    // Track 0 opens track 1 and waits 48 ticks. Track 1 holds key 60 at release 100 with 120 ms of portamento from key
    // 48. Track 1 closes 2 ticks later. That releases the note. The tail goes on gliding to the key. The game's
    // channels do the same. The second sequence is track 1's alone. Track 1 ends the sequence when it closes, and the
    // tail's pitches go on after the last tick.
    const std::vector<uint8_t> kGlide = {0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0xd3,
                                         0x64, 0x3c, 0x64, 0x00, 0x80, 0x02, 0xff};
    std::vector<uint8_t> with_track_0 = {0x88, 0x01, 0x00, 0x00, 0x08, 0x80, 0x30, 0xff};
    with_track_0.insert(with_track_0.end(), kGlide.begin(), kGlide.end());
    const SoundArchive archive = SequenceArchive({with_track_0, kGlide}, {.track_flags = 3});

    const Performance closed = PerformSound(archive, 0);
    const Performance ended = PerformSound(archive, 1);

    XSFLIB_CHECK(GlidesEveryFrame(PitchPlaces(closed, 1)));
    const std::vector<std::pair<double, float>> after_end = PitchPlaces(ended);
    XSFLIB_CHECK(GlidesEveryFrame(after_end) && after_end.back().first > 2.0 && ended.end_tick >= 12);
}

void TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays()
{
    // Track 0 opens track 1 and then only waits 2 ticks and jumps back. Track 1 plays key 60 every 48 ticks, 30 times,
    // for 15 s, and ends. Track 0's passes change nothing while track 1 plays. They aren't passes of the main loop
    // until track 1 has ended. The sequence plays all of track 1. The idle loop after it sounds nothing (SilentLoop).
    // The sequence therefore ends with track 1's last note and has no loop markers.
    const Performance p = PerformCommands({0x88, 0x01, 0x00, 0x00, 0x0b, 0x80, 0x02, 0x89, 0x00, 0x00, 0x05,
                                           0xc7, 0x00, 0xd4, 0x1e, 0x3c, 0x64, 0x18, 0x80, 0x30, 0xfc, 0xff},
                                          {.track_flags = 3});

    XSFLIB_CHECK(NoteOns(p, 1).size() == 30 && !p.loop && p.loops == 0 && p.seconds > 14.75 && p.seconds < 15.0);
}

void TestTrackSettingAVariableAnotherReadsOwnsTheLoop()
{
    // Track 0 opens track 1, then waits 48 ticks and sets player variable 0 to 1, over and over. Track 1 checks the
    // variable every tick. When the variable is set, track 1 clears it and plays key 60. Track 0 changes no sound, but
    // track 1 reads the variable that track 0 sets. Track 0's 48-tick passes are then passes of the main loop.
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x10, 0x80, 0x30, 0xf0, 0x80, 0x00, 0x00, 0x01, 0x89, 0x00,
                         0x00, 0x05, 0xc7, 0x00, 0x80, 0x01, 0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00,
                         0x00, 0x12, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0x89, 0x00, 0x00, 0x12},
                        {.track_flags = 3});

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    const bool every_48 = std::ranges::all_of(note_ons, [](uint32_t tick) { return tick % 48 == 0; });
    XSFLIB_CHECK(p.warnings.empty() && note_ons.size() == 19 && every_48);
    XSFLIB_CHECK(p.loop && p.loop->end - p.loop->start == 48);
}

void TestVariableSetAfterAJumpBackBelongsToTheNewPass()
{
    // As in TestTrackSettingAVariableAnotherReadsOwnsTheLoop, but track 0 sets the variable first in each pass, in the
    // tick it jumps back. That change belongs to the pass that the jump starts. Track 0's 48-tick passes still own the
    // main loop, not track 1's one-tick polls.
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x10, 0xf0, 0x80, 0x00, 0x00, 0x01, 0x80, 0x30, 0x89, 0x00,
                         0x00, 0x05, 0xc7, 0x00, 0x80, 0x01, 0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00,
                         0x00, 0x12, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0x89, 0x00, 0x00, 0x12},
                        {.track_flags = 3});

    XSFLIB_CHECK(p.loop && p.loop->end - p.loop->start == 48);
}

void TestTrackWaitingInSilenceDoesNotHoldOffTheLoop()
{
    // Track 0 opens track 1, holds key 60 for 2,000 ticks with note wait off, and then only waits 2 ticks and jumps
    // back. Track 1 plays key 60 for 24 ticks, then waits 3,000 ticks (31 s) in silence and ends. A track that has
    // sounded nothing and changed nothing for 10 s doesn't count as playing on. Track 0's idle passes count from then.
    // The sequence stops about 10 s after track 1's note. Its loop is one of those passes. Track 0's note still sounds
    // there. The loop therefore isn't silent (SilentLoop).
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x11, 0xc7, 0x00, 0x3c, 0x64, 0x8f, 0x50, 0x80, 0x02,
                         0x89, 0x00, 0x00, 0x0b, 0xc7, 0x00, 0x3c, 0x64, 0x18, 0x80, 0x97, 0x38, 0xff},
                        {.track_flags = 3});

    XSFLIB_CHECK(p.seconds < 15.0 && p.loop && p.loop->start > 24 && p.loop->end - p.loop->start == 2);
}

void TestPollAfterTheSoundIsNoLoop()
{
    // Track 0 opens track 1, then checks each tick whether variable 0 is 1, and leaves the loop when it is. Nothing
    // sets the variable. Track 1 plays key 60 for 12 ticks and ends. Once the note has ended, track 0's passes count as
    // the main loop. They sound nothing (SilentLoop). The performance therefore ends with the note and has no loop
    // markers. When track 1 holds a note of 9,600 ticks instead, the same passes are the main loop under the note.
    const std::vector<uint8_t> kRoot = {0x88, 0x01, 0x00, 0x00, 0x16, 0x80, 0x01, 0xf0, 0x90, 0x00, 0x00,
                                        0x01, 0xa2, 0x89, 0x00, 0x00, 0x15, 0x89, 0x00, 0x00, 0x05, 0xff};
    std::vector<uint8_t> one_shot = kRoot;
    one_shot.insert(one_shot.end(), {0x3c, 0x64, 0x0c, 0xff});
    std::vector<uint8_t> held = kRoot;
    held.insert(held.end(), {0x3c, 0x64, 0xcb, 0x00, 0xff});

    const Performance after = PerformCommands(one_shot, {.track_flags = 3});
    const Performance under = PerformCommands(held, {.track_flags = 3});

    XSFLIB_CHECK(!after.loop && after.loops == 0 && !after.truncated && NoteCount(after) == 1);
    XSFLIB_CHECK(after.seconds < 1.0);
    XSFLIB_CHECK(under.loop && under.loop->end == under.loop->start + 1 && under.seconds > 9.0);
}

void TestSilentPassesThatChangeSettingsEndWithTheSound()
{
    // Play key 60 for 12 ticks, then set the volume to 127 and to 64 in passes of 48 ticks. The passes change the track
    // and sound nothing (SilentLoop). The performance ends with the note. The volume changes after it are left out.
    const Performance p =
        PerformCommands({0x3c, 0x64, 0x0c, 0xc1, 0x7f, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, 0x00, 0x00, 0x03});

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
    const SoundArchive archive = SequenceArchive({{0x80, 0x01, 0x89, 0x00, 0x00, 0x00}});

    const Conversion c = ConvertSequence(archive, 0);

    const seqlib::midi::File m = seqlib::midi::Read(c.midi);
    const auto at_start = [](const seqlib::midi::Track& track)
    {
        return track.end_tick == 0;
    };
    XSFLIB_CHECK(!c.performance.loop && c.performance.loops == 0 && c.performance.end_tick == 0);
    XSFLIB_CHECK(c.performance.seconds == 0.0 && !c.performance.truncated);
    XSFLIB_CHECK(seqlib::midi::Problems(c.midi).empty() && !m.tracks.empty() &&
                 std::ranges::all_of(m.tracks, at_start));
    XSFLIB_CHECK(seqlib::sf2::Check(c.sf2).empty());
}

void TestLoopStartSendsTheVolumeTheGameRestores()
{
    // With note wait off, set volume 100, play key 60 for 6 ticks and wait 12. The loop then sets volume 100 again,
    // plays key 60, and after half its length sets volume 50. The first pass requires no volume event at its start, but
    // the game goes back to 100 there. The loop start therefore sends it for a MIDI player that loops on the markers.
    // The first loop is 48 ticks long and passes on to the next pass; the second is 10 s long, and the performance
    // stops at its end.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64, 0x3c,
                          0x64, 0x18, 0x80, 0x18, 0xc1, 0x32, 0x80, 0x18, 0x89, 0x00, 0x00, 0x09},
                         {0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64, 0x3c, 0x64,
                          0x18, 0x80, 0x83, 0x60, 0xc1, 0x32, 0x80, 0x83, 0x60, 0x89, 0x00, 0x00, 0x09}});

    const Performance short_loop = PerformSound(archive, 0);
    const Performance long_loop = PerformSound(archive, 1);

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
    // is 10 s. The second pass plays as the third. The performance therefore plays the second pass there too.
    const SoundArchive archive = SequenceArchive(
        {{0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, 0x00, 0x00, 0x04},
         {0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x83, 0x60, 0xc1, 0x40, 0x80, 0x83, 0x60, 0x89, 0x00, 0x00,
          0x04}});

    const Performance short_loop = PerformSound(archive, 0);
    const Performance long_loop = PerformSound(archive, 1);

    XSFLIB_CHECK(short_loop.loop && short_loop.loop->start == 48 && short_loop.loop->end == 96);
    XSFLIB_CHECK(long_loop.loop && long_loop.loop->start == 960 && long_loop.loop->end == 1920 &&
                 long_loop.loops == 1 && long_loop.seconds > 20.0 && long_loop.seconds < 21.0);
}

void TestLoopDoesNotStrikeATiedNoteAgain()
{
    // Turn tie on. The loop ties key 60 for a tick, pans left, ties it again for a tick, pans right and jumps back. The
    // first pass strikes the note, and later passes only tie it again. The loop is therefore the second pass. A MIDI
    // player repeats that pass without striking the note every 2 ticks.
    const Performance p = PerformCommands(
        {0xc8, 0x01, 0x3c, 0x64, 0x01, 0xc0, 0x20, 0x3c, 0x64, 0x01, 0xc0, 0x60, 0x89, 0x00, 0x00, 0x02});

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    XSFLIB_CHECK(note_ons == 1 && p.loop && p.loop->start == 2 && p.loop->end == 4);
}

void TestLoopThatNoPassRepeatsKeepsItsFirstPass()
{
    // Track 0 opens track 1, then plays key 60 every 960 ticks (10 s); track 1 plays key 64 every 959. Track 1's notes
    // come a tick earlier in each of track 0's passes. No pass therefore plays as the next. The loop stays on the first
    // pass, and the performance doesn't play another pass to find one.
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x11, 0xc7, 0x00, 0x3c, 0x64, 0x18, 0x80, 0x87, 0x40, 0x89, 0x00,
                         0x00, 0x07, 0xc7, 0x00, 0x40, 0x64, 0x18, 0x80, 0x87, 0x3f, 0x89, 0x00, 0x00, 0x13},
                        {.track_flags = 3});

    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 960 && p.seconds < 12.0);
}

void TestPassOnAnotherPartPlaysAsTheNext()
{
    // With attack and release 122, glide from key 67 to key 83 over 30 ticks, wait 10 and jump back. Each note glides
    // while the last one's release still sounds. The notes therefore take turns on two parts, and a part that a note
    // moves to takes every setting again. The note of a pass and the note of the next pass are on different parts with
    // the same settings. So the first pass plays as the next.
    const Performance p =
        PerformCommands({0xd0, 0x7a, 0xd3, 0x7a, 0xc9, 0x43, 0x53, 0x5a, 0x1e, 0x80, 0x0a, 0x89, 0x00, 0x00, 0x00});

    const std::vector<int> parts = NoteParts(p);
    XSFLIB_CHECK(parts.size() > 3 && parts[0] == 0 && parts[1] == 1 && parts[2] == 0);
    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 40);
}

void TestSequenceThatFinishesHasNoLoop()
{
    // Set variable 0 to 0. Play key 60 for 24 ticks, add 1 to the variable, and jump back while it's below 3, then end.
    // The passes count as a loop on the way, but the sequence ends after three notes. It therefore has no loop.
    const Performance p = PerformCommands({0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xf0, 0x81, 0x00, 0x00,
                                           0x01, 0xf0, 0x94, 0x00, 0x00, 0x03, 0xa2, 0x89, 0x00, 0x00, 0x05, 0xff});

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    XSFLIB_CHECK(note_ons == 3 && !p.loop && p.loops == 0);
}

void TestVariableOptionSetsAVariableBeforeTheStart()
{
    // Wait a tick and check again while player variable 0 isn't 1, then play key 60 and end. Variables start at -1. The
    // sequence therefore plays nothing until the game sets the variable. PerformOptions::variables sets it first.
    const SoundArchive archive = SequenceArchive(
        {{0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x80, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xff}});
    BankSet banks(archive, archive.Sounds()[0]);
    PerformOptions options;
    options.variables[0] = 1;

    const Performance unset = Perform(archive, 0, banks, {});
    const Performance set = Perform(archive, 0, banks, options);

    auto note_ons = [](const Performance& p)
    {
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };
    XSFLIB_CHECK(note_ons(unset) == 0 && note_ons(set) == 1);
}

void TestRetryWithoutAWaitIsNoLoop()
{
    // Draw variable 0 at random from 0 to 3. While it's 0, jump back without a wait and draw again. Then play a 48-tick
    // key 60 and jump back to the draw. The retries aren't passes of a loop. Each note's pass is.
    const Performance p = PerformCommands({0xf0, 0x86, 0x00, 0x00, 0x03, 0xf0, 0x90, 0x00, 0x00, 0x00, 0xa2,
                                           0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x30, 0x89, 0x00, 0x00, 0x00});

    XSFLIB_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 48 && p.warnings.empty());
}

void TestVariablesStartAsTheGamesDo()
{
    // A player variable (0) and a track variable (32) start at -1; a global variable (16) is 0 until the game sets one.
    // Each comparison that holds plays a note.
    const Performance p =
        PerformCommands({0xf0, 0x90, 0x00, 0xff, 0xff, 0xa2, 0x3c, 0x64, 0x30, 0xf0, 0x90, 0x10, 0x00, 0x00,
                         0xa2, 0x3e, 0x64, 0x30, 0xf0, 0x90, 0x20, 0xff, 0xff, 0xa2, 0x40, 0x64, 0x30, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}, {48, 62, 100}, {96, 64, 100}};
    XSFLIB_CHECK(NoteOns(p) == kNotes);
}

void TestTrackStartsMoreNotesATickThanTheGameHasVoices()
{
    // AX has 96 voices. The Wii has no --voices. With note wait off, 100 key-60 notes in a tick therefore all start.
    // With tie on, 97 notes and then key 72 play on one channel. The channel's last legato, the 97th, requires no
    // approximation. Program 5 doesn't exist, and 96 notes on it take no voices. A held note on program 0 after them
    // still plays. And 97 tied notes at velocity 1 are followed by one at 127 with a new release. That release would
    // require another preset. The MIDI note continues in its preset and at its level instead. The whole sequence
    // doesn't get quieter.
    const SoundArchive archive = SequenceArchive(
        {{0xc7, 0x00, 0xd4, 0x64, 0x3c, 0x64, 0x30, 0xfc, 0x80, 0x30, 0xff},
         {0xc7, 0x00, 0xc8, 0x01, 0xd4, 0x61, 0x3c, 0x64, 0x30, 0xfc, 0x48, 0x64, 0x30, 0x80, 0x30, 0xff},
         {0xc7, 0x00, 0x81, 0x05, 0xd4, 0x60, 0x3c, 0x64, 0x30, 0xfc, 0x81, 0x00, 0xc7, 0x01, 0x3c, 0x64, 0x00, 0xff},
         {0xc7, 0x00, 0xc8, 0x01, 0x3c, 0x01, 0x30, 0xd4, 0x60, 0x3c, 0x01,
          0x30, 0xfc, 0xd3, 0x40, 0x3c, 0x7f, 0x30, 0x80, 0x30, 0xff}});

    const Performance flood = PerformSound(archive, 0);
    const Performance tied = PerformSound(archive, 1);
    const Performance held = PerformSound(archive, 2);
    const Performance loud = PerformSound(archive, 3);

    const std::string kLegatosPastCap =
        "more than 96 legatos in a tick (keeping the MIDI preset and limiting gain to the starting level)";
    const std::string kPastCap = " new channels on a track in one tick (excess omitted; the game keeps the newest)";
    const bool none_left_out =
        std::ranges::none_of(flood.approximations, [&kPastCap](const auto& a) { return a.first.ends_with(kPastCap); });
    const std::vector<std::pair<uint32_t, float>> tied_pitch = Pitches(tied);
    XSFLIB_CHECK(NoteOns(flood).size() == 100 && none_left_out);
    XSFLIB_CHECK(NoteOns(tied).size() == 1 && !tied_pitch.empty() && tied_pitch.back().second == 12.0f &&
                 !tied.approximations.contains(kLegatosPastCap));
    XSFLIB_CHECK(NoteOns(held).size() == 1 && held.holds);
    XSFLIB_CHECK(NoteOns(loud).size() == 1 && loud.level_cut_db == 0.0 &&
                 loud.approximations.contains(kLegatosPastCap));
}

void TestTrackPlayingNotesWithoutWaitingIsStopped()
{
    // Loop a one-tick key 60 with note wait off. The track never waits. That would hang the game. The performer stops
    // the track in its first tick. Its notes are released before they've sounded.
    const Performance p = PerformCommands({0xc7, 0x00, 0x3c, 0x64, 0x01, 0x89, 0x00, 0x00, 0x02});

    const std::vector<std::string> kStopped = {"track 0 stopped after running without waits"};
    XSFLIB_CHECK(p.warnings == kStopped && NoteCount(p) == 0 && p.end_tick <= 1);
}

void TestSoundsPanCurveAndModeAreReported()
{
    // Pan a note left with four sound settings: pan curve 2, balance mode with a mono wave (which it doesn't affect),
    // balance mode with a stereo wave, and the default curve and mode.
    std::vector<ArchiveFile> files(3);
    files[0].data = Rseq({0xc0, 0x20, 0x3c, 0x64, 0x30, 0xff});
    files[1].data = Rbnk({TestInstrument{}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    files[2].data = Rbnk({TestInstrument{}});
    files[2].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 2, true)});
    const std::vector<SequenceSound> kSounds = {
        {.bank = 0, .pan_curve = 2}, {.bank = 0, .pan_mode = 1}, {.bank = 1, .pan_mode = 1}, {.bank = 0}};
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = kSounds, .banks = {1, 2}}));

    const Performance curve = PerformSound(archive, 0);
    const Performance mono_balance = PerformSound(archive, 1);
    const Performance stereo_balance = PerformSound(archive, 2);
    const Performance usual = PerformSound(archive, 3);

    const std::string kCurve = "custom pan curve (using the default)";
    const std::string kBalance = "track pan on stereo waves in balance mode (they pan as in dual mode)";
    XSFLIB_CHECK(curve.approximations.contains(kCurve) && !curve.approximations.contains(kBalance));
    XSFLIB_CHECK(stereo_balance.approximations.contains(kBalance) && !stereo_balance.approximations.contains(kCurve));
    XSFLIB_CHECK(mono_balance.approximations.empty() && usual.approximations.empty());
}

void TestTempoAndSweepFollowTheBuild()
{
    // Tempo 2047 and a sweep of 64. Builds from 2008 keep tempos to 1023 and give the sweep in 64ths of a semitone;
    // older builds take the tempo as it is and the sweep in 32nds.
    const std::vector<uint8_t> kCommands = {0xe1, 0x07, 0xff, 0xe3, 0x00, 0x40, 0x3c, 0x64, 0x30, 0xff};

    const Performance late = PerformCommands(kCommands);
    const Performance early = PerformCommands(kCommands, {.version = 0x0103});

    const std::vector<std::pair<uint32_t, float>> late_sweep = Pitches(late), early_sweep = Pitches(early);
    XSFLIB_CHECK(late.since_march_2008 && !early.since_march_2008);
    XSFLIB_CHECK(late.tempo.back().bpm == 1023.0 && early.tempo.back().bpm == 2047.0);
    XSFLIB_CHECK(!late_sweep.empty() && late_sweep.front().second == 1.0f);
    XSFLIB_CHECK(!early_sweep.empty() && early_sweep.front().second == 2.0f);
}

void TestMonophonicAndVelocityRangeComeWith2008()
{
    // Monophonic on, velocity range 64, note wait off, and keys 60 and 62 at velocity 100. Builds from 2008 play one
    // channel, at velocity 100 * 64 / 127. That channel takes the second key as a legato. Older builds have neither
    // command and play both notes at velocity 100.
    const std::vector<uint8_t> kCommands = {0xb2, 0x01, 0xb3, 0x40, 0xc7, 0x00, 0x3c, 0x64,
                                            0x30, 0x3e, 0x64, 0x30, 0x80, 0x60, 0xff};

    const Performance late = PerformCommands(kCommands);
    const Performance early = PerformCommands(kCommands, {.version = 0x0103});

    const std::vector<std::tuple<uint32_t, int, int>> kLate = {{0, 60, 50}};
    const std::vector<std::tuple<uint32_t, int, int>> kEarly = {{0, 60, 100}, {0, 62, 100}};
    XSFLIB_CHECK(NoteOns(late) == kLate && NoteOns(early) == kEarly);
}

void TestRegionsNoteOffTypeAndKeyGroupComeWith2008()
{
    // A region that ignores note-offs, with decay 100 and sustain 0, plays key 60 for one tick. In builds from 2008,
    // the note sounds on, and its MIDI note ends where the note has faded 60 dB (kFadedAttenuation, at about 1.31 s).
    // Older builds release it at tick 1. A region of key group 1 plays key 60 and, 24 ticks later, key 62. In builds
    // from 2008, key 62 releases key 60.
    const std::vector<uint8_t> kShort = {0x3c, 0x64, 0x01, 0x80, 0x87, 0x40, 0xff};
    const std::vector<uint8_t> kTwoKeys = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x18,
                                           0x3e, 0x64, 0x60, 0x80, 0x60, 0xff};
    const TestRegion kIgnoring = {.decay = 100, .sustain = 0, .note_off_type = 1};
    const TestRegion kGrouped = {.key_group = 1};

    const Performance ignoring = PerformCommands(kShort, {.region = kIgnoring});
    const Performance releasing = PerformCommands(kShort, {.version = 0x0103, .region = kIgnoring});
    const Performance grouped = PerformCommands(kTwoKeys, {.region = kGrouped});
    const Performance ungrouped = PerformCommands(kTwoKeys, {.version = 0x0103, .region = kGrouped});

    const std::vector<std::pair<uint32_t, EventKind>> ignored = NoteEvents(ignoring);
    const std::vector<std::pair<uint32_t, EventKind>> kReleased = {{0, EventKind::kNoteOn}, {1, EventKind::kNoteOff}};
    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    const int64_t faded_frames = FramesToLevel(kEnvelope, -static_cast<float>(kFadedAttenuation));
    const double faded_ms = static_cast<double>(faded_frames + 1) * kFrameMs;
    XSFLIB_CHECK(ignored.size() == 2 && ignored[1].first * kTickMs >= faded_ms &&
                 ignored[1].first * kTickMs < faded_ms + kTickMs + kFrameMs);
    XSFLIB_CHECK(NoteEvents(releasing) == kReleased);
    const std::vector<std::pair<uint32_t, EventKind>> kGroupedNotes = {
        {0, EventKind::kNoteOn}, {24, EventKind::kNoteOff}, {24, EventKind::kNoteOn}, {120, EventKind::kNoteOff}};
    const std::vector<std::pair<uint32_t, EventKind>> kUngroupedNotes = {
        {0, EventKind::kNoteOn}, {24, EventKind::kNoteOn}, {96, EventKind::kNoteOff}, {120, EventKind::kNoteOff}};
    XSFLIB_CHECK(NoteEvents(grouped) == kGroupedNotes && NoteEvents(ungrouped) == kUngroupedNotes);
}

void TestBuildOf2006CountsTicksIn416ths()
{
    // The build of October 2006 adds the tempo to a count of 416ths of a tick each 3 ms frame, and runs the whole
    // ticks. At tempo 120, tick 0 runs in frame 0, tick 1 in frame 4 (12 ms) and tick 2 in frame 7 (21 ms). MIDI's
    // tempo of 120 * 20000 / 416 / 48 beats a minute puts the ticks at 10.4 and 20.8 ms. Their events go to the frames'
    // starts, 1.6 and 0.2 ms later. The build of 2007 runs a tick every 10.4 ms. Its events go to the starts of frames
    // 3 and 6, before the ticks.
    const std::vector<uint8_t> kCommands = {0x3c, 0x64, 0x01, 0x3e, 0x64, 0x01, 0x40, 0x64, 0x01, 0xff};
    const SndBuild kOctober2006{.october_2006 = true};
    const double kOctoberTickMs = 3.0 * 416.0 / 120.0;

    const Performance old_build = PerformCommands(kCommands, {.version = 0x0101, .build = kOctober2006});
    const Performance later = PerformCommands(kCommands, {.version = 0x0101});

    XSFLIB_CHECK(old_build.tempo.size() == 1 &&
                 std::abs(old_build.tempo[0].bpm - 120.0 * 20000.0 / 416.0 / 48.0) < 1e-9);
    XSFLIB_CHECK(old_build.frame_ticks.size() >= 3 && old_build.frame_ticks[0] == 0.0 &&
                 std::abs(old_build.frame_ticks[1] - (1.0 + (12.0 - kOctoberTickMs) / kOctoberTickMs)) < 1e-9 &&
                 std::abs(old_build.frame_ticks[2] - (2.0 + (21.0 - 2.0 * kOctoberTickMs) / kOctoberTickMs)) < 1e-9);
    XSFLIB_CHECK(later.frame_ticks.size() >= 3 && later.frame_ticks[1] < 1.0 && later.frame_ticks[2] < 2.0);
}

void TestBuildOf2006HasNoTimebase()
{
    // Timebase 96 at tick 0. The build of October 2006 has no timebase, and MIDI keeps 48 ticks a quarter note. The
    // build of 2007 gives MIDI the timebase.
    const std::vector<uint8_t> kCommands = {0xb0, 0x60, 0x3c, 0x64, 0x30, 0xff};

    const Performance old_build =
        PerformCommands(kCommands, {.version = 0x0101, .build = SndBuild{.october_2006 = true}});
    const Performance later = PerformCommands(kCommands, {.version = 0x0101});

    XSFLIB_CHECK(old_build.timebase == 48 && later.timebase == 96);
}

void TestBuildOf2006RaisesPan1AndHasOnlyDualMode()
{
    // Pan 1. The build of October 2006 raises it to 2: -62/63 of the way to the left. The build of 2007 pans it hard
    // left. The build of October 2006 also plays every stereo wave in dual mode. The build of 2007 plays the stereo
    // waves of an RSAR 1.1 archive in balance mode.
    auto pans = [](const Performance& p)
    {
        std::vector<int> values;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kPan)
            {
                values.push_back(e.value);
            }
        }

        return values;
    };
    const std::vector<uint8_t> kPan1 = {0xc0, 0x01, 0x3c, 0x64, 0x30, 0xff};
    const std::vector<uint8_t> kPan2 = {0xc0, 0x02, 0x3c, 0x64, 0x30, 0xff};

    const Performance old_build = PerformCommands(kPan1, {.version = 0x0101, .build = SndBuild{.october_2006 = true}});
    const Performance later = PerformCommands(kPan1, {.version = 0x0101});
    const Performance raised = PerformCommands(kPan2, {.version = 0x0101});

    XSFLIB_CHECK(!pans(old_build).empty() && pans(old_build) == pans(raised) && pans(old_build) != pans(later));
    XSFLIB_CHECK(!old_build.balance && later.balance);
}

void TestBuildOf2006StopsAReleaseAtVolume0()
{
    // Release 64, and key 60 for one tick. Two ticks after the release, the track's volume falls to 0. The build of
    // October 2006 stops the released note there: its volume times its envelope is 0. MIDI cuts its release tail. In
    // the build of 2007, the tail runs on in silence.
    const std::vector<uint8_t> kCommands = {0xd3, 0x40, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xc1, 0x00, 0x80, 0x60, 0xff};
    auto cuts = [](const Performance& p)
    {
        auto cut = [](const TrackEvent& e)
        {
            return e.kind == EventKind::kSoundOff;
        };
        return std::ranges::count_if(p.tracks[0], cut);
    };

    const Performance old_build =
        PerformCommands(kCommands, {.version = 0x0101, .build = SndBuild{.october_2006 = true}});
    const Performance later = PerformCommands(kCommands, {.version = 0x0101});

    XSFLIB_CHECK(cuts(old_build) == 1 && cuts(later) == 0);
}

// The tests on the Wii.
const Test kTests[] = {
    TestClosingTickReachesTheReleaseTail,
    TestSilenceAtTheTimeLimitEndsWithTheLastSound,
    TestOneShotWavePlaysOutAtItsPitch,
    TestRecordingEndsAOneShotNoteAsTheWiiDoes,
    TestRecordingSetsANoteUpAsTheWiiDoes,
    TestNoteTakesAVoiceFromAPlayingChannel,
    TestTimedSweepGlidesOnWhenTheSequenceStops,
    TestTimedSweepMovesEveryFrame,
    TestTimedSweepGoesOnAfterItsTrackCloses,
    TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays,
    TestTrackSettingAVariableAnotherReadsOwnsTheLoop,
    TestVariableSetAfterAJumpBackBelongsToTheNewPass,
    TestTrackWaitingInSilenceDoesNotHoldOffTheLoop,
    TestPollAfterTheSoundIsNoLoop,
    TestSilentPassesThatChangeSettingsEndWithTheSound,
    TestSequenceWithoutNotesEndsAtItsStart,
    TestLoopStartSendsTheVolumeTheGameRestores,
    TestLoopMarksAPassThatPlaysAsTheNext,
    TestLoopDoesNotStrikeATiedNoteAgain,
    TestLoopThatNoPassRepeatsKeepsItsFirstPass,
    TestPassOnAnotherPartPlaysAsTheNext,
    TestSequenceThatFinishesHasNoLoop,
    TestVariableOptionSetsAVariableBeforeTheStart,
    TestRetryWithoutAWaitIsNoLoop,
    TestVariablesStartAsTheGamesDo,
    TestTrackStartsMoreNotesATickThanTheGameHasVoices,
    TestTrackPlayingNotesWithoutWaitingIsStopped,
    TestSoundsPanCurveAndModeAreReported,
    TestTempoAndSweepFollowTheBuild,
    TestMonophonicAndVelocityRangeComeWith2008,
    TestRegionsNoteOffTypeAndKeyGroupComeWith2008,
    TestBuildOf2006CountsTicksIn416ths,
    TestBuildOf2006HasNoTimebase,
    TestBuildOf2006RaisesPan1AndHasOnlyDualMode,
    TestBuildOf2006StopsAReleaseAtVolume0,
};

} // namespace

int main()
{
    RunOn(Wii(), kTests);

    return xsflib::testing::Summary("sequence_export_revolution_loops");
}

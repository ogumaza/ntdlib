// SPDX-License-Identifier: MIT

// The conversion tests of what only the 3DS does: legatos that leave out the region's volume, one-shot ends at the
// DSP's positions, the random generator's 17 steps before a sound starts, effect sends that reach nothing, and the
// DSP's interpolation in SFZ. sequence_export_test.cpp runs the tests that hold on other consoles as well. The
// recording engine's voice layer for the 3DS ends a one-shot at the same frame, and drops the voice with the lowest
// priority from a full pool. On the core, a legato leaves the channel's envelope where it is. With the game's voices, a
// new note takes the voice of the oldest note of the lowest priority. A note that every voice outranks plays nothing.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/sfz.h"
#include "seqlib/midi/performance_writer.h"
#include "sequence_export_test.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;

namespace
{

// nw::snd's model of the 3DS on the recording engine, with the 3DS's voices.
class CtrCore : public RecordingEngine<ntdlib::nwsnd::CtrPolicy, CtrCore, BankSet, CtrVoiceLayer>
{
public:
    using RecordingEngine::RecordingEngine;
};

// Performs `commands` with `voice_limit` voices. If `channel_priority` is given, the sound sets it: the channels'
// priority | release priority fix << 8.
Performance PerformOnVoices(const std::vector<uint8_t>& commands, int voice_limit,
                            std::optional<uint32_t> channel_priority = std::nullopt)
{
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(commands);
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SequenceSound sound = {.banks = {0x03000000}, .channel_priority = channel_priority};
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files, {.sequences = {sound}, .banks = {1}, .wave_archives = {2}}), console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    return Perform(archive, 0, banks, {.voice_limit = voice_limit});
}

// The MIDI notes of `p`'s track 0, in order: each note's key and the ticks of its note-on and note-off.
std::vector<std::tuple<int, uint32_t, uint32_t>> PlayedNotes(const Performance& p)
{
    std::vector<std::tuple<int, uint32_t, uint32_t>> notes;
    std::map<uint32_t, std::size_t> playing; // by note ID
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            playing[e.note] = notes.size();
            notes.emplace_back(e.key, e.tick, e.tick);
        }
        else if (e.kind == EventKind::kNoteOff && playing.contains(e.note))
        {
            std::get<2>(notes[playing[e.note]]) = e.tick;
        }
    }

    return notes;
}

void TestLegatoLeavesOutTheRegionsVolume()
{
    // With volume2 64 and tie enabled, play key 60 at velocity 100 on a volume-64 region, then key 62 at 100 and key 64
    // at 127. nw::snd continues the channel with velocity gain but without region volume. The MIDI note starts at 127,
    // the highest velocity the legatos reach, with CC11 taking it down to 100. The note continues through both legatos.
    // There CC11 also compensates for the zone's region volume by 127 / 64 in power.
    const Performance p =
        PerformOnBank({0xd5, 0x40, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0x40, 0x7f, 0x30, 0xff},
                      OneRegionFbnk({{0x05000000, 0}}, 64));

    const LegatoOutput out = ReadLegatoOutput(p);
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 50}, {48, 71}, {96, 90}};
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    XSFLIB_CHECK(out.expression == kExpression && out.notes == kNotes && p.level_cut_db == 0.0);
}

void TestLegatoKeepsTheChannelsEnvelope()
{
    // With no hold, the fastest attack and decay and the highest sustain, play key 64 with tie on, then key 59 as a
    // legato. Turning tie off then releases the channel. Before the legato, one version sets the track's decay to 100
    // and its sustain to 0. nw::snd sets them on the tied channel (SequenceTrack::NoteOn). The channel's envelope is in
    // its sustain by then, and a sustain keeps its level. Both versions release from full level, and their sound ends
    // in the same tick. The performer's model of the envelope ran the note from its start with the new values, and
    // ended the first version sooner.
    const auto end_tick = [](bool new_decay)
    {
        std::vector<uint8_t> commands = {0xb1, 0x00, 0xd0, 0x7f, 0xd1, 0x7f, 0xd2, 0x7f,
                                         0xd3, 0x6e, 0xc8, 0x01, 0x40, 0x7f, 0x30};
        if (new_decay)
        {
            commands.insert(commands.end(), {0xd1, 0x64, 0xd2, 0x00});
        }
        commands.insert(commands.end(), {0x3b, 0x7f, 0x30, 0xc8, 0x00, 0xff});

        return PerformOnBank(commands, OneRegionFbnk({{0x05000000, 0}})).end_tick;
    };

    XSFLIB_CHECK(end_tick(true) == end_tick(false));
}

void TestOneShotWaveEndsAsTheDspPlaysIt()
{
    // Play a 6399-sample one-shot at four times the DSP rate (640 samples/frame), then wait for completion. The DSP
    // position is 1 + (160 n - 1) * 4. That adds 3 samples to the effective length. Playback takes 11 frames after
    // note-on, not 10. The channel disappears in frame 13 (63.5 ms); execution resumes at tick 7 (72.9 ms), the first
    // tick in that frame.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0x3c, 0x7f, 0x00, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(6399, std::vector<int16_t>(6399, 1000), 1, false, 4 * 32728)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<uint32_t> note_offs;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            note_offs.push_back(e.tick);
        }
    }

    XSFLIB_CHECK(note_offs == std::vector<uint32_t>{7} && p.end_tick == 7);
}

void TestRandomArgumentDrawsAfterTheStepsBeforeTheStart()
{
    // Set the volume to a random value from 0 to 127 and play a note. As on the 3DS, the performer steps the generator
    // 17 times before the sequence starts. The draw takes the 18th value after 0x12345678, and (128 r) >> 16 gives 113.
    // The first value would give 58.
    const Performance p =
        PerformOnBank({0xa0, 0xc1, 0x00, 0x00, 0x00, 0x7f, 0x3c, 0x64, 0x30, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    std::vector<int> volumes;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volumes.push_back(e.value);
        }
    }

    XSFLIB_CHECK(volumes == std::vector<int>{113});
}

void TestEffectSendsReachNothing()
{
    // Send 127 to effect bus A and 64 to B, then play a note. Pokemon X runs no effect on either bus. CC91 and CC93
    // therefore stay at 0, and the performance reports both sends.
    const Performance p =
        PerformOnBank({0xd9, 0x7f, 0xda, 0x40, 0x3c, 0x64, 0x30, 0xff}, OneRegionFbnk({{0x05000000, 0}}));

    const auto no_send = [](const TrackEvent& e)
    {
        return (e.kind != EventKind::kReverb && e.kind != EventKind::kChorus) || e.value == 0;
    };
    const bool all_zero = std::ranges::all_of(p.tracks[0], no_send);

    XSFLIB_CHECK(all_zero);
    XSFLIB_CHECK(p.approximations.contains("effect send A (omitted)") &&
                 p.approximations.contains("effect send B (omitted)"));
}

void TestSfzRegionFollowsTheDspsInterpolation()
{
    // A region that selects linear interpolation (1) on the DSP gets sfizz's linear one, and one that selects none (2)
    // plays the nearest sample. The SoundFont can't follow either. The performance therefore reports them. A region
    // that selects the polyphase filter (0) keeps sfizz's default.
    auto convert = [](uint8_t interpolation)
    {
        std::vector<ArchiveFile> files(3);
        files[0].data = Fseq({0x3c, 0x64, 0x18, 0xff});
        files[1].data = OneRegionFbnk({{0x05000000, 0}}, 127, 64, false, interpolation);
        files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true, 32728)});
        const SoundArchive archive =
            SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                               console->settings.archive);
        BankSet banks(archive, archive.Sounds()[0], console->settings);
        const Performance p = Perform(archive, 0, banks, {});
        MidiReport midi;
        midi.bend_ranges[0] = 2;
        const SfzInstruments sfz =
            BuildSfz(p, midi, banks, "TITLE", std::vector<std::string>(p.presets.size(), "preset"));

        const std::string text = sfz.files.size() == 1 ? sfz.files[0].text : "";
        return std::tuple{
            text.find(" sample_quality=1") != std::string::npos, text.find(" sample_quality=0") != std::string::npos,
            p.approximations.contains("linear sample interpolation (SoundFont uses the synth's interpolation)"),
            p.approximations.contains("uninterpolated sample playback (SoundFont uses the synth's interpolation)")};
    };

    const auto linear = convert(1);
    const auto none = convert(2);
    const auto polyphase = convert(0);

    XSFLIB_CHECK(linear == std::tuple(true, false, true, false));
    XSFLIB_CHECK(none == std::tuple(false, true, false, true));
    XSFLIB_CHECK(polyphase == std::tuple(false, false, false, false));
}

void TestRecordingEngineEndsTheOneShotAtTheSameFrame()
{
    // The one-shot of TestOneShotWaveEndsAsTheDspPlaysIt, on the recording engine. The note starts in frame 0, and the
    // DSP plays the wave from frame 1 to frame 11. The voice update of frame 12 frees the channel: it sounds after 12
    // frames, and not after 13. The note's volume is its velocity squared times the region's volume, times 1/127^3.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0x3c, 0x7f, 0x00, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(6399, std::vector<int16_t>(6399, 1000), 1, false, 4 * 32728)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    const ntdlib::sar::Sequence sequence =
        ntdlib::sar::Sequence::Parse(archive.FileData(archive.Sounds()[0].file_id), console->settings.archive);
    CtrCore core(banks, 64, 23);
    core.Play(archive.Sounds()[0], sequence.data, 0);

    std::size_t after_12 = 0;
    float volume = 0.0f;
    for (int frame = 0; frame < 12; ++frame)
    {
        core.RunFrame();
    }
    after_12 = core.Channels().InUse().size();
    if (after_12 == 1)
    {
        volume = core.Channels().At(core.Channels().InUse().front()).velocity_volume;
    }
    core.RunFrame();

    XSFLIB_CHECK(after_12 == 1 && core.Channels().InUse().empty());
    XSFLIB_CHECK(volume == static_cast<float>(127 * 127 * 127) * ntdlib::nwsnd::CtrPolicy::kOneOver127Cubed);
}

void TestOneShotLeadFollowsTheRate()
{
    // A portamento takes a note from key 66 down to key 49 in its first 13 frames. The one-shot wave has 7,935 samples
    // at 22,050 Hz, and the region's original key is 60. The DSP's position runs ahead of the sample that it reads by
    // the rate, less 1. That lead follows each change of rate. The voice update of frame 130 frees the channel. With
    // the lead of the start rate, the wave would end a frame later.
    files::Region region;
    region.original_key = 60;
    const files::VelocityTable velocities{files::VelocityTable::kDirect, 0, {{127, region}}};
    const files::KeyTable keys{files::KeyTable::kDirect, 0, {{127, velocities}}};
    const std::vector<std::pair<uint32_t, uint32_t>> waves = {{0x05000000, 0}};
    std::vector<ArchiveFile> archive_files(3);
    archive_files[0].data = Fseq({0xd3, 100, 0xc9, 66, 0xcf, 5, 49, 110, 4, 0xff}); // release, porta, its time, note
    archive_files[1].data = files::Bank(console->settings.archive.flavor, {waves, {keys}});
    archive_files[2].data = Fwar({Pcm16Fwav(7935, std::vector<int16_t>(7935, 1000), 1, false, 22050)});
    const SoundArchive archive = SoundArchive::Load(
        Fsar(archive_files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    const ntdlib::sar::Sequence sequence =
        ntdlib::sar::Sequence::Parse(archive.FileData(archive.Sounds()[0].file_id), console->settings.archive);
    CtrCore core(banks, 64, 23);
    core.Play(archive.Sounds()[0], sequence.data, 0);

    for (int frame = 0; frame < 130; ++frame)
    {
        core.RunFrame();
    }
    const std::size_t after_130 = core.Channels().InUse().size();
    core.RunFrame();

    XSFLIB_CHECK(after_130 == 1 && core.Channels().InUse().empty());
}

void TestRecordingEngineDropsTheLowestPriorityVoice()
{
    // Two notes of the same priority in one tick, with note waits off, on a pool of one voice. The second note's voice
    // takes the place of the first note's voice. The first note's channel goes, and the second note sounds.
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq({0xc7, 0x00, 0x3c, 0x7f, 0x30, 0x40, 0x7f, 0x30, 0x80, 0x60, 0xff});
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                           console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    const ntdlib::sar::Sequence sequence =
        ntdlib::sar::Sequence::Parse(archive.FileData(archive.Sounds()[0].file_id), console->settings.archive);
    CtrCore core(banks, 64, 1);
    core.Play(archive.Sounds()[0], sequence.data, 0);

    core.RunFrame();

    const auto& in_use = core.Channels().InUse();
    XSFLIB_CHECK(in_use.size() == 1 && core.Channels().At(in_use.front()).key == 0x40);
}

void TestNoteTakesTheVoiceOfTheLowestPriority()
{
    // Play five sequences on two voices, with note waits off. Each note holds for 96 ticks unless the sequence says
    // otherwise. In the first sequence, keys 60, 62 and 64 start a tick apart. Key 64 finds both voices in use. It
    // takes the voice of key 60, the oldest note of the same priority. Key 60 then ends at tick 2. In the second, keys
    // 60 and 62 start at tick 0, and keys 64, 65, 67 and 69 start at tick 1. Each of the four takes the voice of the
    // oldest note. Keys 64 and 65 lose their voices in the frame they start in. Their notes leave nothing in the MIDI.
    // Key 65's channel takes the place that key 60's channel left in the same tick. In the third, key 64 starts at tick
    // 1 with a lower priority. Both voices outrank it. Key 64 plays nothing. In the fourth, key 62 starts at tick 1
    // with a lower priority. Key 64 starts at tick 2 with the first priority again. Key 64 takes the voice of key 62,
    // the newer note. A change of the track's priority leaves the playing notes' priorities as they are. In the fifth,
    // key 62 plays for one tick from tick 1, with a slow release. A released voice drops to priority 1. At tick 3, key
    // 64 takes the voice of key 62's release. The MIDI note has ended already, and the stop is reported. With the
    // sound's release priority fix, the release keeps its priority. Key 64 then takes the voice of key 60. Without the
    // drops, every note holds.
    const std::vector<uint8_t> kApart = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0x3e, 0x64,
                                         0x60, 0x80, 0x01, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff};
    const std::vector<uint8_t> kOneTick = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x3e, 0x64, 0x60, 0x80, 0x01, 0x40, 0x64, 0x60,
                                           0x41, 0x64, 0x60, 0x43, 0x64, 0x60, 0x45, 0x64, 0x60, 0x80, 0x60, 0xff};
    const std::vector<uint8_t> kLower = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x3e, 0x64, 0x60, 0x80,
                                         0x01, 0xc6, 0x20, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff};
    const std::vector<uint8_t> kMixed = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc6, 0x20, 0x3e, 0x64,
                                         0x60, 0x80, 0x01, 0xc6, 0x40, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff};
    const std::vector<uint8_t> kReleased = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xd3, 0x40, 0x3e,
                                            0x64, 0x01, 0x80, 0x02, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff};
    constexpr uint32_t kReleasePriorityFix = 64 | 1 << 8;

    const auto played = [](const std::vector<uint8_t>& commands, int voice_limit)
    {
        return PlayedNotes(PerformOnVoices(commands, voice_limit));
    };
    const auto stop_reported = [](const Performance& p)
    {
        return p.approximations.contains("immediate channel stop (MIDI release tail remains)");
    };

    using Notes = std::vector<std::tuple<int, uint32_t, uint32_t>>;
    const Notes kApartTaken = {{60, 0, 2}, {62, 1, 97}, {64, 2, 98}};
    const Notes kApartAll = {{60, 0, 96}, {62, 1, 97}, {64, 2, 98}};
    const Notes kOneTickTaken = {{60, 0, 1}, {62, 0, 1}, {67, 1, 97}, {69, 1, 97}};
    const Notes kOneTickAll = {{60, 0, 96}, {62, 0, 96}, {64, 1, 97}, {65, 1, 97}, {67, 1, 97}, {69, 1, 97}};
    const Notes kLowerDropped = {{60, 0, 96}, {62, 0, 96}};
    const Notes kLowerAll = {{60, 0, 96}, {62, 0, 96}, {64, 1, 97}};
    const Notes kMixedTaken = {{60, 0, 96}, {62, 1, 2}, {64, 2, 98}};
    const Notes kMixedAll = {{60, 0, 96}, {62, 1, 97}, {64, 2, 98}};
    const Notes kReleasedAll = {{60, 0, 96}, {62, 1, 2}, {64, 3, 99}};
    const Notes kReleasedHeldTaken = {{60, 0, 3}, {62, 1, 2}, {64, 3, 99}};
    const Performance released = PerformOnVoices(kReleased, 2);
    const Performance released_all = PerformOnVoices(kReleased, 0);
    XSFLIB_CHECK(played(kApart, 2) == kApartTaken && played(kApart, 0) == kApartAll);
    XSFLIB_CHECK(played(kOneTick, 2) == kOneTickTaken && played(kOneTick, 0) == kOneTickAll);
    XSFLIB_CHECK(played(kLower, 2) == kLowerDropped && played(kLower, 0) == kLowerAll);
    XSFLIB_CHECK(played(kMixed, 2) == kMixedTaken && played(kMixed, 0) == kMixedAll);
    XSFLIB_CHECK(PlayedNotes(released) == kReleasedAll && stop_reported(released));
    XSFLIB_CHECK(PlayedNotes(released_all) == kReleasedAll && !stop_reported(released_all));
    XSFLIB_CHECK(PlayedNotes(PerformOnVoices(kReleased, 2, kReleasePriorityFix)) == kReleasedHeldTaken);
}

const Test kTests[] = {
    TestLegatoLeavesOutTheRegionsVolume,
    TestLegatoKeepsTheChannelsEnvelope,
    TestOneShotWaveEndsAsTheDspPlaysIt,
    TestRecordingEngineEndsTheOneShotAtTheSameFrame,
    TestOneShotLeadFollowsTheRate,
    TestRecordingEngineDropsTheLowestPriorityVoice,
    TestNoteTakesTheVoiceOfTheLowestPriority,
    TestRandomArgumentDrawsAfterTheStepsBeforeTheStart,
    TestEffectSendsReachNothing,
    TestSfzRegionFollowsTheDspsInterpolation,
};

} // namespace

int main()
{
    RunOn(Ctr(), kTests);

    return xsflib::testing::Summary("sequence_export_ctr");
}

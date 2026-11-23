// SPDX-License-Identifier: MIT

// Tests of the DS games' profiles as a converter's options and arrangements (sequence_export/ntr/game_profiles.h). A
// profile built by hand checks ApplyProfile and ArrangementsOf: the settings reach the options, the profile's commands
// follow those that the options contain, a variable that the options set keeps its value, and each state becomes an
// arrangement in the profile's order. The default profile changes nothing. UseGameProfiles then gives a converter's
// hooks the profile of a sequence of an archive, by the game code, the file, the archive's place and the label. Spirit
// Tracks' SEQ_39_INTRO then fades on the frames and ticks of a run of the game's code. A lock of a track's channel ends
// its note on the lock's frame, and a lock from the start keeps the note from starting.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/ntr/archive_input.h"
#include "ntdlib/sequence_export/ntr/game_profiles.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace ntr = ntdlib::ntr;
namespace snd = ntdlib::ntr::snd;
namespace sdat_files = ntdlib::testing::sdat_files;
using ntdlib::sequence_export::ntr::ApplyProfile;
using ntdlib::sequence_export::ntr::ArchiveConverter;
using ntdlib::sequence_export::ntr::ArchiveInput;
using ntdlib::sequence_export::ntr::Arrangement;
using ntdlib::sequence_export::ntr::ArrangementsOf;
using ntdlib::sequence_export::ntr::Perform;
using ntdlib::sequence_export::ntr::Performance;
using ntdlib::sequence_export::ntr::PerformOptions;
using ntdlib::sequence_export::ntr::ProfileOf;
using ntdlib::sequence_export::ntr::UseGameProfiles;
using xsflib::testing::Check;

snd::Command TrackVolume(uint32_t tracks, uint32_t volume)
{
    return {.id = snd::CommandId::kTrackParameter, .args = {0, tracks, volume, 0}};
}

bool SameArrangements(const std::vector<Arrangement>& a, const std::vector<Arrangement>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++)
    {
        if (a[i].suffix != b[i].suffix || a[i].variables != b[i].variables || a[i].commands != b[i].commands ||
            a[i].tick_commands != b[i].tick_commands)
        {
            return false;
        }
    }

    return true;
}

void TestApplyProfile()
{
    ntr::GameProfile profile;
    profile.settings.variables.local[2] = 5;
    profile.settings.variables.local[3] = -1;
    profile.settings.variables.global[0] = 7;
    profile.settings.channels[1] = 0x00f0;
    profile.settings.track_channels.push_back({.tracks = 0x0003, .channels = 0x0300});
    profile.settings.reverb = ntr::Reverb{.size = 0x2000, .pcm8 = true, .sample_rate = 8000, .volume = 64};
    profile.commands.push_back(TrackVolume(0x0001, 90));
    profile.channel_locks.push_back({.frame = 300, .channels = 0x00C0});
    profile.arrangements.push_back({.suffix = " (night)",
                                    .variables = {{2, 9}},
                                    .commands = {TrackVolume(2, 0)},
                                    .tick_commands = {{.tick = 96, .frames = {{TrackVolume(2, 1)}}}}});
    profile.arrangements.push_back({.suffix = " (indoors)", .variables = {}, .commands = {}});

    PerformOptions perform;
    perform.variables[3] = 4; // as --var 3=4 gives it
    perform.player_channels[0] = 0xffff;
    perform.commands.push_back(TrackVolume(0x0004, 10));
    perform.channel_locks.push_back({.frame = 10, .channels = 0x0001});
    ApplyProfile(profile, perform);

    Check(perform.variables.size() == 3 && perform.variables.at(2) == 5 && perform.variables.at(3) == 4 &&
              perform.variables.at(16) == 7,
          "the profile's variables reach the options, the global ones from 16, and a value set already stays");
    Check(perform.player_channels.size() == 2 && perform.player_channels.at(0) == 0xffff &&
              perform.player_channels.at(1) == 0x00f0,
          "the profile's channels of a player replace that player's, and the other players keep theirs");
    Check(perform.track_channels.size() == 1 && perform.track_channels[0].tracks == 0x0003 &&
              perform.track_channels[0].channels == 0x0300,
          "the profile's channels of some tracks reach the options");
    Check(perform.reverb && perform.reverb->size == 0x2000 && perform.reverb->pcm8 &&
              perform.reverb->sample_rate == 8000 && perform.reverb->volume == 64,
          "the profile's reverb reaches the options");
    Check(perform.commands.size() == 2 && perform.commands[0] == TrackVolume(0x0004, 10) &&
              perform.commands[1] == TrackVolume(0x0001, 90),
          "the profile's commands follow those that the options contain");
    Check(perform.channel_locks.size() == 2 && perform.channel_locks[0].frame == 10 &&
              perform.channel_locks[1] == profile.channel_locks[0],
          "the profile's locks of channels follow those that the options contain");

    const std::vector<Arrangement> arrangements = ArrangementsOf(profile);
    Check(arrangements.size() == 2 && arrangements[0].suffix == " (night)" && arrangements[0].variables.size() == 1 &&
              arrangements[0].variables.at(2) == 9 && arrangements[0].commands.size() == 1 &&
              arrangements[0].commands[0] == TrackVolume(2, 0) &&
              arrangements[0].tick_commands == profile.arrangements[0].tick_commands &&
              arrangements[1].suffix == " (indoors)" && arrangements[1].variables.empty() &&
              arrangements[1].commands.empty() && arrangements[1].tick_commands.empty(),
          "each state of the profile becomes an arrangement, in the profile's order");

    PerformOptions plain;
    plain.reverb = ntr::Reverb{.volume = 3};
    ApplyProfile(ntr::GameProfile{}, plain);
    Check(plain.variables.empty() && plain.player_channels.empty() && plain.track_channels.empty() &&
              plain.commands.empty() && plain.channel_locks.empty() && plain.reverb && plain.reverb->volume == 3 &&
              ArrangementsOf(ntr::GameProfile{}).empty(),
          "the default profile changes nothing and gives no arrangement");
}

// An input of one archive with one sequence, `label`, at the archive's place 0 in `file_path` of the image of
// `game_code`.
ArchiveInput OneSequence(const std::string& game_code, const std::string& file_path, const std::string& label)
{
    sdat_files::Archive spec;
    spec.items[0] = {sdat_files::SequenceItem(0, 0, label)};
    spec.files = {sdat_files::Sequence({0xFF})};

    ArchiveInput input;
    input.bytes = std::make_shared<const std::vector<uint8_t>>(sdat_files::SoundArchive(spec));
    input.archive = ntdlib::sdat::Archive::Parse(*input.bytes);
    input.sequences = ntr::Sequences(input.archive);
    input.game_code = game_code;
    input.file_path = file_path;

    return input;
}

// The profile of Victory Road in Pokémon Black sends track commands and gives arrangements. ntr_game_profiles_test.cpp
// checks its values.
void TestUseGameProfiles()
{
    ArchiveConverter converter;
    UseGameProfiles(converter);
    Check(converter.game_settings != nullptr && converter.arrangements != nullptr, "UseGameProfiles sets both hooks");

    const ArchiveInput road = OneSequence("IRBO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD");
    Check(road.sequences.size() == 1, "the archive has one sequence");
    const ntr::SequenceRef& sequence = road.sequences.front();
    const ntr::GameProfile profile = ntr::GameProfileOf("IRBO", "wb_sound_data.sdat", 0, "SEQ_BGM_D_CHAMPROAD");
    const ntr::GameProfile found = ProfileOf(road, sequence);
    Check(found.commands == profile.commands && found.arrangements.size() == profile.arrangements.size() &&
              found.settings.channels == profile.settings.channels,
          "ProfileOf finds the profile by the game code, the file, the archive's place and the label");
    Check(!profile.commands.empty(), "the profile sends commands");
    Check(!profile.arrangements.empty(), "the profile gives arrangements");

    // The same file and label in the second archive of the file: no profile names that archive.
    ArchiveInput second = OneSequence("IRBO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD");
    second.index_in_file = 1;
    const ntr::GameProfile other = ProfileOf(second, second.sequences.front());
    const ntr::GameProfile expected = ntr::GameProfileOf("IRBO", "wb_sound_data.sdat", 1, "SEQ_BGM_D_CHAMPROAD");
    Check(other.commands.empty() && other.arrangements.empty() && other.commands == expected.commands &&
              other.arrangements.size() == expected.arrangements.size(),
          "ProfileOf passes the archive's place in its file");

    PerformOptions hooked;
    converter.game_settings(road, sequence, hooked);
    PerformOptions direct;
    ApplyProfile(profile, direct);
    Check(hooked.variables == direct.variables && hooked.player_channels == direct.player_channels &&
              hooked.track_channels.size() == direct.track_channels.size() && hooked.commands == direct.commands &&
              hooked.reverb.has_value() == direct.reverb.has_value(),
          "the converter's game settings apply the sequence's profile");
    Check(SameArrangements(converter.arrangements(road, sequence), ArrangementsOf(profile)),
          "the converter's arrangements are the profile's states");

    const ArchiveInput unknown = OneSequence("ZZZZ", "sound_data.sdat", "SEQ_BGM");
    PerformOptions untouched;
    converter.game_settings(unknown, unknown.sequences.front(), untouched);
    Check(untouched.variables.empty() && untouched.player_channels.empty() && untouched.commands.empty() &&
              !untouched.reverb && converter.arrangements(unknown, unknown.sequences.front()).empty(),
          "a game that no profile lists plays as the archive gives it");
}

// SEQ_39_INTRO of Spirit Tracks (NTR-BKIE-USA) on a sequence with SEQ_39's tempos: 101 from tick 0, 96 from tick 49,
// 94 from 385, 92 from 409 and 100 from 529. Track 0 holds a note through the fade. Each step of the fade changes track
// 0's fader, and so its CC11.
//
// The game's code sent the steps on a run, frame by frame, after SEQ_37 and then SEQ_39 were requested through the BGM
// manager (0x0206C988) at the title. Before each list, the player's tick counter in the driver's shared work read 508,
// 510, 513, 515, 518, 520, 523, 525, 528, 530, 533, 535, 538, 541 and 543: a list in every second frame of the screen.
// A list's events go at the last tick run, one below the count. The profile's frames count from the start. The run's
// came 25.6 ms after it, because the request loaded the sequence for 125 ms and sent the start late in a frame. A game
// sends its requests at any point of a frame. A step may therefore land up to a frame of the game (about 2.5 ticks)
// away from the run's. The span from the first step to the last doesn't depend on it. The tolerance can't tell whether
// a count equal to the tick starts the lists. sequence_export_ntr_test pins that rule.
void TestSpiritTracksIntroTiming()
{
    const sdat_files::Bytes bytecode = {
        0xC7, 0x00,                         // note-wait mode off
        0x3C, 0x7F, 0x85, 0x3C,             // note 60 for 700 ticks
        0xE1, 0x65, 0x00, 0x80, 0x31,       // tempo 101, a wait of 49 ticks
        0xE1, 0x60, 0x00, 0x80, 0x82, 0x50, // tempo 96, a wait of 336
        0xE1, 0x5E, 0x00, 0x80, 0x18,       // tempo 94, a wait of 24
        0xE1, 0x5C, 0x00, 0x80, 0x78,       // tempo 92, a wait of 120
        0xE1, 0x64, 0x00, 0x80, 0x81, 0x2B, // tempo 100, a wait of 171
        0xFF,                               // the end
    };
    sdat_files::Archive spec;
    spec.items[0] = {sdat_files::SequenceItem(0, 0, "SEQ_39")};
    spec.items[2] = {sdat_files::BankItem(1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK_39")};
    spec.items[3] = {sdat_files::FileItem(2, "WAVE_39")};
    spec.items[4] = {sdat_files::PlayerItem(1, 0, 0, "PLAYER_39")};
    const sdat_files::Wave square{
        .loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    spec.files = {sdat_files::Sequence(bytecode), sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}),
                  sdat_files::WaveArchive({square})};
    const std::vector<uint8_t> bytes = sdat_files::SoundArchive(spec);
    const ntdlib::sdat::Archive archive = ntdlib::sdat::Archive::Parse(bytes);
    const ntr::GameProfile profile = ntr::GameProfileOf("BKIE", "SoundData/final_sound_data.sdat", 0, "SEQ_39");
    Check(profile.arrangements.size() == 1, "SEQ_39 has one arrangement, _INTRO");
    if (profile.arrangements.size() != 1)
    {
        return;
    }

    PerformOptions options;
    ApplyProfile(profile, options);
    const ntr::ProfileArrangement& intro = profile.arrangements[0];
    options.commands.insert(options.commands.end(), intro.commands.begin(), intro.commands.end());
    options.tick_commands = intro.tick_commands;

    const Performance performance = Perform(archive, ntr::Sequences(archive).front(), options);

    constexpr std::array<uint32_t, 15> kRunCounts = {508, 510, 513, 515, 518, 520, 523, 525,
                                                     528, 530, 533, 535, 538, 541, 543};
    std::vector<uint32_t> ticks;
    for (const seqlib::midi::TrackEvent& e : performance.tracks[0])
    {
        if (e.kind == seqlib::midi::EventKind::kExpression && e.tick >= 500)
        {
            ticks.push_back(e.tick);
        }
    }
    bool on_time = ticks.size() == kRunCounts.size();
    for (std::size_t i = 0; on_time && i < ticks.size(); i++)
    {
        on_time = ticks[i] + 1 + 3 >= kRunCounts[i] && ticks[i] + 1 <= kRunCounts[i] + 3;
    }
    Check(on_time, "SEQ_39_INTRO's 15 steps land within a frame of the game of the run's");
    const uint32_t run_span = kRunCounts.back() - kRunCounts.front();
    Check(ticks.size() == kRunCounts.size() && ticks.back() - ticks.front() + 1 >= run_span &&
              ticks.back() - ticks.front() <= run_span + 1,
          "SEQ_39_INTRO's fade spans the run's 35 ticks, give or take one");
}

// The tick at which key 60's first note ends on track 0, or none.
std::optional<uint32_t> NoteEnd(const Performance& performance)
{
    for (const seqlib::midi::TrackEvent& e : performance.tracks[0])
    {
        if (e.kind == seqlib::midi::EventKind::kNoteOff && e.key == 60)
        {
            return e.tick;
        }
    }

    return std::nullopt;
}

// Performs the archive whose one sequence plays `bytecode` with a looped square wave released at `release`. The
// options give track 0 channel 6 alone.
Performance PerformOnChannel6(const sdat_files::Bytes& bytecode, uint8_t release,
                              const std::vector<ntdlib::ntr::ChannelLock>& locks)
{
    sdat_files::Archive spec;
    spec.items[0] = {sdat_files::SequenceItem(0, 0, "SEQ_HELD")};
    spec.items[2] = {sdat_files::BankItem(1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK_HELD")};
    spec.items[3] = {sdat_files::FileItem(2, "WAVE_HELD")};
    spec.items[4] = {sdat_files::PlayerItem(1, 0, 0, "PLAYER_HELD")};
    const sdat_files::Wave square{
        .loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    spec.files = {sdat_files::Sequence(bytecode),
                  sdat_files::Bank({{.type = 1, .region = {.wave = 0, .release = release}}}),
                  sdat_files::WaveArchive({square})};
    const std::vector<uint8_t> bytes = sdat_files::SoundArchive(spec);
    const ntdlib::sdat::Archive archive = ntdlib::sdat::Archive::Parse(bytes);
    PerformOptions options;
    options.track_channels.push_back({.tracks = 0x0001, .channels = 0x0040});
    options.channel_locks = locks;
    return Perform(archive, ntr::Sequences(archive).front(), options);
}

// The ticks of the sound-offs on track 0.
std::vector<uint32_t> SoundOffs(const Performance& performance)
{
    std::vector<uint32_t> ticks;
    for (const seqlib::midi::TrackEvent& e : performance.tracks[0])
    {
        if (e.kind == seqlib::midi::EventKind::kSoundOff)
        {
            ticks.push_back(e.tick);
        }
    }
    return ticks;
}

// A note of 700 ticks at the default tempo (half a tick in each of the driver's frames) on track 0, on channel 6. A
// lock of channels 6 and 7 before frame 100 ends the note at tick 50. A lock from the start keeps the note from
// starting, and an unlock before frame 100 changes nothing for it: a note that gets no channel never starts later. The
// console stops a locked channel at once. The cut note therefore also gets a sound-off.
void TestChannelLock()
{
    const sdat_files::Bytes bytecode = {
        0xC7, 0x00,             // note-wait mode off
        0x3C, 0x7F, 0x85, 0x3C, // note 60 for 700 ticks
        0x80, 0x85, 0x3C,       // a wait of 700 ticks
        0xFF,                   // the end
    };
    const Performance whole = PerformOnChannel6(bytecode, 127, {});
    const Performance cut = PerformOnChannel6(bytecode, 127, {{.frame = 100, .channels = 0x00C0}});
    const Performance unstarted = PerformOnChannel6(
        bytecode, 127, {{.frame = 0, .channels = 0x00C0}, {.frame = 100, .channels = 0x00C0, .unlock = true}});

    const std::optional<uint32_t> whole_end = NoteEnd(whole);
    const std::optional<uint32_t> cut_end = NoteEnd(cut);
    Check(whole_end && *whole_end >= 699, "without a lock, the note plays its 700 ticks");
    Check(cut_end && *cut_end >= 49 && *cut_end <= 50, "a lock before frame 100 ends the note at tick 50");
    Check(!NoteEnd(unstarted) && unstarted.unstarted.size() == 1 && unstarted.unstarted[0].key == 60,
          "a lock from the start keeps the note from starting");
    const std::vector<uint32_t> cut_offs = SoundOffs(cut);
    Check(SoundOffs(whole).empty() && cut_offs.size() == 1 && cut_offs[0] >= 49 && cut_offs[0] <= 50,
          "the lock stops the cut note at once");
}

// A note of 10 ticks with a slow release on channel 6. A lock before frame 100 (tick 50) stops the release with a
// sound-off. Without the lock the release rings on.
void TestChannelLockCutsRelease()
{
    const sdat_files::Bytes bytecode = {
        0xC7, 0x00,       // note-wait mode off
        0x3C, 0x7F, 0x0A, // note 60 for 10 ticks
        0x80, 0x85, 0x3C, // a wait of 700 ticks
        0xFF,             // the end
    };
    const Performance rings = PerformOnChannel6(bytecode, 40, {});
    const Performance cut = PerformOnChannel6(bytecode, 40, {{.frame = 100, .channels = 0x00C0}});

    const std::vector<uint32_t> cut_offs = SoundOffs(cut);
    Check(NoteEnd(rings) == NoteEnd(cut) && NoteEnd(cut) && *NoteEnd(cut) <= 10,
          "the note ends at tick 10 with the lock and without it");
    Check(SoundOffs(rings).empty() && cut_offs.size() == 1 && cut_offs[0] >= 49 && cut_offs[0] <= 50,
          "a lock stops the release at tick 50");
}

void Checks()
{
    TestApplyProfile();
    TestUseGameProfiles();
    TestSpiritTracksIntroTiming();
    TestChannelLock();
    TestChannelLockCutsRelease();
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("DS games' profiles in a conversion", Checks);
}

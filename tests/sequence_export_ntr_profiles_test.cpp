// SPDX-License-Identifier: MIT

// Tests of the DS games' profiles as a converter's options and arrangements (sequence_export/ntr/game_profiles.h). A
// profile built by hand checks ApplyProfile and ArrangementsOf: the settings reach the options, the profile's commands
// follow those that the options contain, a variable that the options set keeps its value, and each state becomes an
// arrangement in the profile's order. The default profile changes nothing. UseGameProfiles then gives a converter's
// hooks the profile of a sequence of an archive, by the game code, the file, the archive's place and the label.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
        if (a[i].suffix != b[i].suffix || a[i].variables != b[i].variables || a[i].commands != b[i].commands)
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
    profile.arrangements.push_back({.suffix = " (night)", .variables = {{2, 9}}, .commands = {TrackVolume(2, 0)}});
    profile.arrangements.push_back({.suffix = " (indoors)", .variables = {}, .commands = {}});

    PerformOptions perform;
    perform.variables[3] = 4; // as --var 3=4 gives it
    perform.player_channels[0] = 0xffff;
    perform.commands.push_back(TrackVolume(0x0004, 10));
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

    const std::vector<Arrangement> arrangements = ArrangementsOf(profile);
    Check(arrangements.size() == 2 && arrangements[0].suffix == " (night)" && arrangements[0].variables.size() == 1 &&
              arrangements[0].variables.at(2) == 9 && arrangements[0].commands.size() == 1 &&
              arrangements[0].commands[0] == TrackVolume(2, 0) && arrangements[1].suffix == " (indoors)" &&
              arrangements[1].variables.empty() && arrangements[1].commands.empty(),
          "each state of the profile becomes an arrangement, in the profile's order");

    PerformOptions plain;
    plain.reverb = ntr::Reverb{.volume = 3};
    ApplyProfile(ntr::GameProfile{}, plain);
    Check(plain.variables.empty() && plain.player_channels.empty() && plain.track_channels.empty() &&
              plain.commands.empty() && plain.reverb && plain.reverb->volume == 3 &&
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

void Checks()
{
    TestApplyProfile();
    TestUseGameProfiles();
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("DS games' profiles in a conversion", Checks);
}

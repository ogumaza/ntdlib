// SPDX-License-Identifier: MIT

// Tests of the host's side of the Z2 start-up's parameters (flipper/z2_params.h): the offsets, the profile's words, a
// piece's state and the wave list that the parameters hold (flipper/wave_list.h).

#include "ntdlib/flipper/z2_params.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/flipper/wave_list.h"
#include "ntdlib/jaudio/z2_states.h"

namespace z2 = ntdlib::flipper::z2;
using ntdlib::flipper::WaveList;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

int main()
{
    // The profile follows the sound, its handle, the wave count and 64 wave groups.
    Check(z2::kProfileOffset == 0x10c, "the profile's offset");

    // The profile's 56 words in Z2Params' order: the frame's retraces, the 10 sizes, the game's 30 addresses, the two
    // erase functions, the count and the 4 first-scene groups, the count and the 4 groups for room, then the music
    // manager, its setChildTrackVolume and the sound starter's setPortData.
    z2::Profile profile;
    profile.frame_retraces = 1;
    profile.audio_heap_size = 0x180000;
    profile.vi_init = 0x80001000;
    profile.archive_heap_parent = 0x8052112c;
    profile.start_sound = 0x8000abcd;
    profile.erase_bgm_wave = 0x8029ca40;
    profile.first_scene_wave_count = 2;
    profile.first_scene_waves = {0x00010000, 0x00000058, 0, 0};
    profile.room_wave_count = 1;
    profile.room_waves = {0x00000002, 0, 0, 0};
    profile.music_manager = 0x80450b84;
    profile.set_child_volume = 0x802b3fec;
    profile.set_port_data = 0x802aaedc;
    profile.get_port_data = 0x802aaf74;
    const std::vector<uint32_t> words = profile.Words();
    Check(words.size() == 57, "the profile has 57 words");
    Check(words.size() == 57 && words[0] == 1 && words[10] == 0x180000 && words[11] == 0x80001000 &&
              words[23] == 0x8052112c && words[40] == 0x8000abcd && words[41] == 0x8029ca40 && words[42] == 0 &&
              words[43] == 2 && words[44] == 0x00010000 && words[45] == 0x58 && words[47] == 0 && words[48] == 1 &&
              words[49] == 2 && words[52] == 0 && words[53] == 0x80450b84 && words[54] == 0x802b3fec &&
              words[55] == 0x802aaedc && words[56] == 0x802aaf74,
          "the profile's words in Z2Params' order");
    // A rip writes the words where the driver's struct has the fields. A console's driver may add fields after them.
    Check(sizeof(z2::Profile) == 4 * words.size(), "the profile's size is that of its words");
    Check(z2::Profile{}.Words() == std::vector<uint32_t>(57, 0), "a profile's fields are 0 by default");
    // The piece's state follows the profile: the children to mute, the partial children, their 16 volumes, the status
    // and the statuses by section. It ends Z2Params.
    Check(z2::kMutedChildrenOffset == 0x10c + 4 * 57 && z2::kPartialChildrenOffset == z2::kMutedChildrenOffset + 4 &&
              z2::kPartialVolumesOffset == z2::kMutedChildrenOffset + 8 &&
              z2::kMusicStatusOffset == z2::kMutedChildrenOffset + 72 &&
              z2::kSectionStatusesOffset == z2::kMutedChildrenOffset + 76 &&
              z2::kParamsSize == z2::kMutedChildrenOffset + 80,
          "the offsets of the piece's state and the size of Z2Params");

    // A state's words go up to the last that isn't 0. A state of muted children alone has one word. A state without
    // any has none.
    Check(z2::PieceState{}.Bytes().empty(), "a piece without a state writes nothing");
    Check(z2::PieceState{.muted_children = 0x00000600}.Bytes() == std::vector<uint8_t>{0, 0, 6, 0},
          "a state of muted children writes one word");

    // Ordon Village's status 1: children 0 to 7 at 0.65, child 8 at 0, and 1 on port 9. Every word goes in.
    const auto ordon =
        std::find_if(ntdlib::jaudio::kTwilightPrincessStates.begin(), ntdlib::jaudio::kTwilightPrincessStates.end(),
                     [](const ntdlib::jaudio::MusicState& state)
                     { return state.sound == 0x01000005 && state.suffix == "_status1"; });
    Check(ordon != ntdlib::jaudio::kTwilightPrincessStates.end(), "the table lists Ordon Village's status 1");
    const z2::PieceState village = z2::PieceStateOf(*ordon);
    const std::vector<uint8_t> bytes = village.Bytes();
    Check(village.muted_children == 0x0100 && village.partial_children == 0x00ff &&
              village.partial_volumes[0] == 0.65f && village.partial_volumes[7] == 0.65f &&
              village.partial_volumes[8] == 0.0f && village.music_status == (z2::kMusicStatusSet | 1),
          "Ordon Village's status 1 as a piece's state");
    Check(bytes.size() == 76 && bytes[3] == 0x00 && bytes[2] == 0x01 && bytes[7] == 0xff && bytes[8] == 0x3f &&
              bytes[9] == 0x26 && bytes[10] == 0x66 && bytes[11] == 0x66 && bytes[40] == 0 && bytes[72] == 0 &&
              bytes[73] == 1 && bytes[74] == 0 && bytes[75] == 1,
          "Ordon Village's status 1 in words");

    // A state without a status ends with its last partial volume. The battle music's start mutes children only.
    const z2::PieceState partial = {.partial_children = 0x0002, .partial_volumes = {0.0f, 0.5f}};
    Check(partial.Bytes().size() == 16, "a state without a status ends with its last volume");
    const z2::PieceState battle = z2::PieceStateOf(ntdlib::jaudio::State(0x0100000f, "", {{0x500e, 0.0f}}));
    Check(battle == z2::PieceState{.muted_children = 0x500e} && battle.Bytes().size() == 4,
          "a state of muted children alone");

    // Hyrule Field's day music: status 11 writes 12 in its first section (1) and 11 in the D sections' first (17), and
    // status 9 writes 9 before the first tick (section 0). Each half of the last word holds one.
    const auto in_main = [](std::string_view suffix)
    {
        const auto& states = ntdlib::jaudio::kTwilightPrincessStates;
        return std::find_if(states.begin(), states.end(), [&](const ntdlib::jaudio::MusicState& state)
                            { return state.sound == 0x01000000 && state.suffix == suffix; });
    };
    const z2::PieceState evening = z2::PieceStateOf(*in_main("_riding_status11"));
    const std::vector<uint8_t> evening_bytes = evening.Bytes();
    Check(evening.muted_children == 0xf000 && evening.music_status == 0 && evening.section_statuses == 0x110b010c,
          "the riding arrangement's status 11 as a piece's state");
    Check(evening_bytes.size() == 80 && evening_bytes[76] == 0x11 && evening_bytes[77] == 0x0b &&
              evening_bytes[78] == 0x01 && evening_bytes[79] == 0x0c,
          "statuses by section are the state's last word");
    Check(z2::PieceStateOf(*in_main("_status9")).section_statuses == 0x00000009 &&
              z2::PieceStateOf(*in_main("_riding_status3")).section_statuses == 0x00000303 &&
              z2::PieceStateOf(*in_main("_status12")).music_status == (z2::kMusicStatusSet | 12) &&
              in_main("_status3") == ntdlib::jaudio::kTwilightPrincessStates.end(),
          "the other statuses of Hyrule Field's day music");

    // A status that an actor writes once the piece plays: the driver writes it 30 frames later. Zant's create
    // writes its status right after the start.
    const auto in_table = [](uint32_t sound, std::string_view suffix)
    {
        const auto& states = ntdlib::jaudio::kTwilightPrincessStates;
        return std::find_if(states.begin(), states.end(), [&](const ntdlib::jaudio::MusicState& state)
                            { return state.sound == sound && state.suffix == suffix; });
    };
    Check(z2::PieceStateOf(*in_table(0x01000094, "_status2")).music_status ==
                  (z2::kMusicStatusSet | z2::kMusicStatusLater | 2) &&
              z2::PieceStateOf(*in_table(0x01000062, "_status11")).music_status == (z2::kMusicStatusSet | 11) &&
              z2::PieceStateOf(*in_table(0x01000062, "_status10")).music_status ==
                  (z2::kMusicStatusSet | z2::kMusicStatusLater | 10),
          "a status that the game writes once the piece plays");

    // The horse battle's status 2 and the variant music's status 3 move their sequences to another section. Both
    // sequences clear port 9 in their first tick. The variant music's status 2 leaves its sequence in the start's
    // section, and the rodeo music's sequence reads no port. Both statuses stay with the start.
    Check(z2::PieceStateOf(*in_table(0x01000004, "_status2")).music_status ==
                  (z2::kMusicStatusSet | z2::kMusicStatusLater | 2) &&
              z2::PieceStateOf(*in_table(0x01000039, "_status3")).music_status ==
                  (z2::kMusicStatusSet | z2::kMusicStatusLater | 3) &&
              z2::PieceStateOf(*in_table(0x01000039, "_status2")).music_status == (z2::kMusicStatusSet | 2) &&
              z2::PieceStateOf(*in_table(0x01000029, "_status2")).music_status == (z2::kMusicStatusSet | 2),
          "the later statuses of the horse battle and the variant music");

    // The Wii's table: Hyrule Castle's room 12 plays its volumes as status 3150, from Link's height. Room 8 stays.
    const auto& wii = ntdlib::jaudio::kTwilightPrincessWiiStates;
    const auto in_wii = [](std::string_view suffix)
    {
        const auto& states = ntdlib::jaudio::kTwilightPrincessWiiStates;
        return std::find_if(states.begin(), states.end(), [&](const ntdlib::jaudio::MusicState& state)
                            { return state.sound == 0x0100003f && state.suffix == suffix; });
    };
    const auto foot = in_wii("_status3150");
    Check(in_wii("_status12") == wii.end() && foot != wii.end() && z2::PieceStateOf(*foot).muted_children == 0x0278 &&
              z2::PieceStateOf(*foot).partial_children == 0x0180 &&
              z2::PieceStateOf(*foot).partial_volumes[7] == 0.5f && foot->status_port == 3150 &&
              in_wii("_status8") != wii.end() && in_wii("_status6150") != wii.end() &&
              in_table(0x0100003f, "_status12") != ntdlib::jaudio::kTwilightPrincessStates.end() &&
              wii.size() == ntdlib::jaudio::kTwilightPrincessStates.size(),
          "Twilight Princess's states on the Wii");

    // HD's table: Hyrule Castle's piece starts with children 3 to 6 at 0.3, without 0 to 2 and 7 to 9. Its room 12 is
    // gone. The twilight battle music writes its status to port 9.
    const auto& hd = ntdlib::jaudio::kTwilightPrincessHdStates;
    const auto in_hd = [](uint32_t sound, std::string_view suffix)
    {
        const auto& states = ntdlib::jaudio::kTwilightPrincessHdStates;
        return std::find_if(states.begin(), states.end(), [&](const ntdlib::jaudio::MusicState& state)
                            { return state.sound == sound && state.suffix == suffix; });
    };
    const auto castle = in_hd(0x0100003f, "");
    const auto twilight = in_hd(0x0100001b, "_status3");
    Check(castle != hd.end() && z2::PieceStateOf(*castle).muted_children == 0x0387 &&
              z2::PieceStateOf(*castle).partial_children == 0x0078 &&
              z2::PieceStateOf(*castle).partial_volumes[3] == 0.3f && in_hd(0x0100003f, "_status12") == hd.end() &&
              in_hd(0x0100003f, "_status3150") != hd.end() && twilight != hd.end() && twilight->status_port == 3 &&
              hd.size() == ntdlib::jaudio::kTwilightPrincessStates.size() + 1,
          "Twilight Princess HD's states");

    // The wave list: the count, then each group, big-endian.
    const std::vector<uint32_t> kWaves = {0x00010003, 0x00000007};
    Check(WaveList(kWaves) == std::vector<uint8_t>{0, 0, 0, 2, 0, 1, 0, 3, 0, 0, 0, 7}, "a wave list of two groups");
    Check(WaveList({}) == std::vector<uint8_t>{0, 0, 0, 0}, "an empty wave list");
    const std::vector<uint32_t> kTooMany(65, 0);
    Check(ErrorOf([&] { WaveList(kTooMany); }) == "a sound names 65 waves, more than 64", "too many waves");

    return xsflib::testing::Summary("z2 params");
}

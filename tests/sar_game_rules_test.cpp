// SPDX-License-Identifier: MIT

// Checks RulesOf (ntdlib/sar/game_rules.h): an archive that no rule names gets none, and the rules of WarioWare: Get It
// Together!'s Sound_Common.bfsar hold its 9 layered sounds, its two reverbs for aux bus B, its microgames' sounds and
// the other sounds that send to bus B. WarioWare: Move It!'s Sound.bfsar keeps one reverb on bus B, under a delay on
// bus A. Clubhouse Games: 51 Worldwide Classics' main archive puts the renderer's reverb on bus B for the piano's
// notes, and the bowling game's sub-archive keeps one there for every sound. The values come from the effect list in
// the game's sound_settingpreset.bspp. StartsOf gives the values that Super Mario 3D World + Bowser's Fury and Super
// Mario Party Jamboree write as they start some sounds.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/sar/game_rules.h"

using xsflib::testing::Check;

int main()
{
    Check(ntdlib::sar::RulesOf(std::vector<uint8_t>(64, 0)).Empty(), "an archive that no rule names has no rules");
    Check(ntdlib::sar::RulesOf(169'540'000, std::string(40, '0')).Empty(),
          "an archive of a ruled size but another SHA-1");

    const ntdlib::sar::ArchiveRules& r = ntdlib::sar::RulesOf(169'540'000, "de37204c45671bbb532b238669b459ab57c70a95");
    Check(r.layered.size() == 9 && r.layered[0].label == "BGM_P_Guitar" && r.layered[0].states.size() == 4 &&
              r.layered[0].states[0].name.empty() && r.layered[0].states[0].muted == 0xfe,
          "the layered sounds, with the guitar's start state first");
    const ntdlib::sar::ReverbPreset* hall = r.FindBusBPreset("I3DL2_Hall");
    const ntdlib::sar::ReverbPreset* standard = r.FindBusBPreset("I3DL2_Default");
    Check(
        hall && standard && hall->DryGain() == 0.0f && standard->DryGain() == 0.25f && !r.FindBusBPreset("I3DL2_Room"),
        "the two reverbs that the microgames put on bus B, with their dry gains");
    std::size_t halls = 0;
    for (const auto& [label, preset] : r.bus_b)
    {
        halls += preset == "I3DL2_Hall" ? 1 : 0;
    }
    Check(r.bus_b.size() == 261 && halls == 35 && r.bus_b.at("BGM_P_Maze") == "I3DL2_Hall" &&
              r.bus_b.at("BGM_P_Dance") == "I3DL2_Default" && !r.bus_b.contains("BGM_P_Guitar") &&
              !r.bus_b.contains("BGM_T_Sport_SpeedUp"),
          "the microgames' sounds that send to bus B, 35 of them with the Hall reverb, and none of the stages' music");
    Check(r.bus_b_later.size() == 183 && r.bus_b_always.empty() &&
              std::ranges::find(r.bus_b_later, "BGM_T_Sport_SpeedUp") != r.bus_b_later.end() &&
              std::ranges::find(r.bus_b_later, "BGM_T_Wario_SpeedUp") == r.bus_b_later.end(),
          "the other sounds that send to bus B, and not a stage's music that sends only to bus A");

    const std::vector<ntdlib::sar::SoundState> guitar = ntdlib::sar::StatesOf(r, "BGM_P_Guitar");
    const std::vector<ntdlib::sar::SoundState> speed_up = ntdlib::sar::StatesOf(r, "BGM_T_Sport_SpeedUp");
    const std::vector<ntdlib::sar::SoundState> maze = ntdlib::sar::StatesOf(r, "BGM_P_Maze");
    Check(guitar.size() == 4 && guitar[1].name == "20 percent" && guitar[1].muted == 0xf9 && guitar[1].bus_b.empty() &&
              ntdlib::sar::StatesOf(r, "BGM_P_Dance")[1].bus_b == "I3DL2_Default",
          "a layered microgame sound's states, each with its microgame's reverb where the sound sends to bus B");
    Check(speed_up.size() == 3 && speed_up[0].name.empty() && speed_up[0].bus_b.empty() &&
              speed_up[1].name == "Default reverb" && speed_up[1].bus_b == "I3DL2_Default" &&
              speed_up[2].name == "Hall reverb" && speed_up[2].bus_b == "I3DL2_Hall",
          "a stage's sound starts with bus B empty, then plays with each reverb");
    Check(maze.size() == 1 && maze[0].name.empty() && maze[0].muted == 0 && maze[0].bus_b == "I3DL2_Hall",
          "a microgame's sound has one state, with its reverb");
    Check(ntdlib::sar::StatesOf(r, "SE_Unknown").size() == 1 && ntdlib::sar::StatesOf(r, "SE_Unknown")[0].bus_b.empty(),
          "any other sound has one state with bus B empty");

    const ntdlib::sar::ArchiveRules& m = ntdlib::sar::RulesOf(195'310'032, "971e60148f9ab6793a091803a2e649fea6b91fbb");
    const ntdlib::sar::ReverbPreset* waltz = m.FindBusBPreset(m.bus_b_always);
    Check(m.layered.empty() && m.bus_b.empty() && waltz && waltz->name == "I3DL2_Waltz" && waltz->DryGain() == 0.0f,
          "a game that keeps one reverb on bus B for every sound");
    Check(m.bus_a_delay && !r.bus_a_delay, "Move It! keeps a delay on bus A, and Get It Together! a reverb");
    Check(ntdlib::sar::StatesOf(m, "BGM_Any").size() == 1 &&
              ntdlib::sar::StatesOf(m, "BGM_Any")[0].bus_b == "I3DL2_Waltz",
          "such a game's sounds have one state, with that reverb");

    const ntdlib::sar::ArchiveRules& c = ntdlib::sar::RulesOf(56'574'072, "cd337a1d0041f9fed8eb6f2c9c74c419e87905bb");
    const ntdlib::sar::ReverbPreset* piano = c.FindBusBPreset("EFFECT_GAM022");
    Check(piano && piano->type == ntdlib::sar::ReverbType::kReverb && piano->parameters[0] == 2.0f &&
              piano->parameters[2] == 15'000.0f && piano->parameters[3] == 1.0f &&
              piano->parameters[5] == 2'500'000.0f && piano->parameters[8] == 0.4f && piano->parameters[11] == 2.0f &&
              piano->DryGain() == 0.0f,
          "the piano game's reverb: modes 2 and 1, a pre-delay of 15 ms, a decay of 2.5 s and no dry part");
    Check(c.bus_b.size() == 49 && c.bus_b.at("SQ_SE_GAM022_PIANO_CN1") == "EFFECT_GAM022" &&
              c.bus_b.at("SQ_SE_GAM022_PIANO_BN4") == "EFFECT_GAM022" &&
              c.bus_b.at("SQ_SE_GAM022_PIANO_CN5") == "EFFECT_GAM022" && !c.bus_b.contains("SQ_SE_GAM022_PIANO_CS5") &&
              c.bus_b_always.empty() && c.bus_b_later.empty(),
          "the piano's 49 notes play with the reverb on bus B, and the other sounds with the bus empty");

    const ntdlib::sar::ArchiveRules& b = ntdlib::sar::RulesOf(3'600'192, "2f6b6c44031995054eaf70aa903d68d4faf4317b");
    const ntdlib::sar::ReverbPreset* room = b.FindBusBPreset(b.bus_b_always);
    Check(room && room->name == "EFFECT_COMMON_ROOM_00" && room->type == ntdlib::sar::ReverbType::kReverb &&
              room->parameters[2] == 40'000.0f && room->parameters[3] == 4.0f && room->parameters[7] == 0.1f &&
              room->DryGain() == 0.0f && ntdlib::sar::StatesOf(b, "SQ_SE_GAM005_ANY")[0].bus_b == room->name,
          "the bowling game's sub-archive keeps its reverb on bus B for every sound");

    const ntdlib::sar::ArchiveRules& w = ntdlib::sar::RulesOf(929'216, "c628822613bf210ed412463c1f47de952be1a907");
    const auto spin = ntdlib::sar::StartsOf(w, "SePvSpinAttack");
    const auto lantern = ntdlib::sar::StartsOf(w, "SeOjLanternSwing");
    Check(spin.size() == 1 && spin[0].size() == 1 && spin[0].at(0) == 4,
          "Rosalina's spin attack voice starts with the character variable at 4");
    Check(ntdlib::sar::StartsOf(w, "SePvKinotanJumpFail").size() == 1 &&
              ntdlib::sar::StartsOf(w, "SePvKinotanJumpFail")[0].at(0) == 6,
          "Captain Toad's voice starts with the character variable at 6");
    Check(lantern.size() == 2 && lantern[0].at(0) == 120 && lantern[1].at(0) == 80,
          "a lantern's swing starts with variable 0 at 120 or 80");
    Check(ntdlib::sar::StartsOf(w, "SePvSpinAttackAirX").empty() && ntdlib::sar::StartsOf(m, "SePvSpinAttack").empty(),
          "a label must match in full, and another archive's sounds get no starts");

    const ntdlib::sar::ArchiveRules& j = ntdlib::sar::RulesOf(64'338'704, "a4ede9f67dab3a17300a7a22d97b58f95552bfde");
    const auto step = ntdlib::sar::StartsOf(j, "SQ_SE_FS_KB00_NPC103");
    Check(step.size() == 39 && step[0].size() == 2 && step[0].at(6) == 127 && step[0].at(7) == 127 &&
              step[21].at(6) == (64 << 7 | 127) && step[38].at(7) == (81 << 7 | 127),
          "a footstep starts on one of the 39 surfaces that the footsteps compare, under the whole foot");
    Check(ntdlib::sar::StartsOf(j, "SQ_SE_FT_ANY").empty(), "a label outside the footsteps' prefix gets no starts");
    const ntdlib::sar::ArchiveRules& bd03 = ntdlib::sar::RulesOf(1'842'144, "e6d62e59da1c2e15ed63d9dad14f17404b78901c");
    const auto train = ntdlib::sar::StartsOf(bd03, "SQ_SE_BD03_ITEM00_TRN_LOP");
    Check(train.size() == 1 && train[0].at(15) == 127, "the roulette's train loop starts with variable 15 at 127");
    const ntdlib::sar::ArchiveRules& rides = ntdlib::sar::RulesOf(650'360, "1fee777a145e7f00084e37c676adbcb59490c872");
    const auto boat = ntdlib::sar::StartsOf(rides, "SQ_SE_MG1703_BOAT_MOV_LOP");
    Check(boat.size() == 5 && boat[0].at(15) == 4 && boat[1].at(15) == 0 &&
              ntdlib::sar::StartsOf(rides, "SQ_SE_MG0703_WTRBIKE_MOV_LOP")[0].at(15) == 5 &&
              ntdlib::sar::StartsOf(rides, "SQ_AMB_MG1703_BOAT_MOV_LOP").empty(),
          "the boat's and the water bike's move loops start moving, and their ambient copies get no starts");
    const ntdlib::sar::ArchiveRules& rock = ntdlib::sar::RulesOf(433'024, "ad47ccf12af80a9d1052c4b5c2a87191fd7c9a04");
    const ntdlib::sar::ArchiveRules& steam =
        ntdlib::sar::RulesOf(1'731'768, "320813bb7292721bcae21d544fcde3288ac8feb9");
    Check(ntdlib::sar::StartsOf(rock, "SQ_SE_MG0912_OBJ_ROCK00_ROL_LOP")[0].at(15) == 127 &&
              ntdlib::sar::StartsOf(steam, "SQ_SE_MG0503_STEAM00_R_LOP")[0].at(15) == 127,
          "the rolling rock and the steam start at full volume");

    return xsflib::testing::Summary("sar game rules");
}

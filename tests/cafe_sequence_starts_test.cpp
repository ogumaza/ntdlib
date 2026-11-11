// SPDX-License-Identifier: MIT

// Tests of how known Wii U games start some sequences (ntdlib/cafe/snd/sequence_starts.h): the variables that they set,
// the later states and their names, a piece whose tracks a game turns down, and the archives and sequences that the
// lists leave out. Each game's code gives the values, at the addresses that src/cafe/snd/sequence_starts.cpp cites.

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/cafe/snd/sequence_starts.h"

namespace
{

using ntdlib::cafe::snd::ListedSequences;
using ntdlib::cafe::snd::SequenceArrangement;
using ntdlib::cafe::snd::SequenceArrangements;
using ntdlib::cafe::snd::SequenceStart;
using ntdlib::cafe::snd::SequenceVariable;
using ntdlib::cafe::snd::SequenceVariables;
using ntdlib::cafe::snd::StartOfSequence;
using xsflib::testing::Check;

// The SHA-1s of the sound archives that the lists hold.
constexpr std::string_view kAmiiboFestival = "5784acb45299369f8388246f0ba4311610c709bc";
constexpr std::string_view kAmiiboFestivalArc1 = "724d9942fa0ea684839a3de269878e11b206a135";
constexpr std::string_view kMarioParty10 = "00bdc6c4610830576a44b7198d25894fe87f06b9";
constexpr std::string_view kNintendoLand = "1379ab0c17a80e0c38e0c9e90aaf92e971f2c1a4";
constexpr std::string_view kSuperMario3dWorld = "96b005771143ce5508eb0136143bde832e059ef8";
constexpr std::string_view kNewSuperMarioBrosU = "e0da277f9eed3c9fbd6dcbce09230811932d14dc";
constexpr std::string_view kWiiPartyU = "528580bb03f3fbdea2815d7fa7b355233d90d28d";
constexpr std::string_view kRainbowCurse = "813a15124d13b1978e7e3cb49f6648b9cb50d705";

// The variables that `variables` sets, each followed by a space: "index=value" for a variable of the sequence or a
// global variable, and "track:index=value" for a variable of a track.
std::string Set(const SequenceVariables& variables)
{
    std::string set;
    for (const SequenceVariable& variable : variables)
    {
        if (variable.index == SequenceVariable::kNone)
        {
            continue;
        }

        if (variable.track != SequenceVariable::kNoTrack)
        {
            set += std::to_string(variable.track) + ":";
        }
        set += std::to_string(variable.index) + "=" + std::to_string(variable.value) + " ";
    }

    return set;
}

// The name that an arrangement's variables give it: "_" before each variable's number and value, and "_" between two
// variables.
std::string NameOf(const SequenceVariables& variables)
{
    std::string name;
    for (const SequenceVariable& variable : variables)
    {
        if (variable.index == SequenceVariable::kNone)
        {
            continue;
        }

        name += (name.empty() ? "_" : "__") + std::to_string(variable.index) + "_" + std::to_string(variable.value);
    }

    return name;
}

void TestStarts()
{
    const SequenceStart humming = StartOfSequence(kAmiiboFestivalArc1, "SQ_SE_ACT_HUMMING");
    Check(Set(humming.variables) == "0:15=1 " && humming.tracks == 0xffff,
          "amiibo Festival's humming starts with track 0's variable 15 at 1");
    Check(Set(StartOfSequence(kAmiiboFestival, "SQ_SE_HAB_ENV").variables) == "18=0 ",
          "the habitat card's ambience starts with global 2 at 0");
    Check(Set(StartOfSequence(kMarioParty10, "SQ_SE_BD003_VCL_LOP").variables) == "29=0 ",
          "a board's vehicle starts with global 13 at 0");
    Check(Set(StartOfSequence(kMarioParty10, "SQ_SE_MG0307_TRP").variables) == "15=0 ",
          "minigame MG0307's trap starts with variable 15 at 0");
    Check(Set(StartOfSequence(kNintendoLand, "SE_LIG_OBAKE_LIFE_DOWN").variables) == "4=0 ",
          "the ghost's falling life starts with variable 4 at 0");
    Check(Set(StartOfSequence(kNintendoLand, "SE_TTL_TRAIN_MOVE_STEAM").variables) == "0=-9 ",
          "the plaza's train starts at level -9");
    Check(Set(StartOfSequence(kWiiPartyU, "SEQ_SE_PLY_FUTTOBI_DAMAGE").variables) == "15=0 ",
          "a Mii's damage in Wii Party U starts with variable 15 at 0");
    Check(Set(StartOfSequence(kWiiPartyU, "SEQ_SE_PLY_FOOT_WALK_DRC").variables) == "11=0 ",
          "a Mii's footstep in Wii Party U starts with the footstep at 0");
    Check(Set(StartOfSequence(kWiiPartyU, "SEQ_BGM_NPG006_THEMA").variables).empty(),
          "Wii Party U's theme starts with its level unset");

    const std::string unlisted = Set(StartOfSequence(kAmiiboFestival, "SQ_BGM_LOUNGE").variables) +
                                 Set(StartOfSequence("", "SQ_SE_HAB_ENV").variables) +
                                 Set(StartOfSequence(kAmiiboFestivalArc1, "SQ_SE_HAB_ENV").variables);
    Check(unlisted.empty() && StartOfSequence("", "SQ_SE_HAB_ENV").tracks == 0xffff,
          "an unlisted sequence, an unknown archive and the other archive of the game set nothing");
}

void TestArrangements()
{
    const std::span<const SequenceArrangement> touch = SequenceArrangements(kMarioParty10, "SQ_SE_MG0K04_KOP_TCH_DRC");
    Check(touch.size() == 5 && touch[3].suffix == "_24_0__25_0" && Set(touch[3].variables) == "24=0 25=0 ",
          "Bowser's touch has five arrangements, one of them with two variables");
    Check(SequenceArrangements(kNewSuperMarioBrosU, "SE_OBJ_RC_LIFTLINE_MOVE").size() == 3 &&
              Set(SequenceArrangements(kNewSuperMarioBrosU, "SE_OBJ_RC_LIFTLINE_MOVE")[2].variables) == "0=200 ",
          "the line lift's fastest arrangement");
    Check(SequenceArrangements(kAmiiboFestival, "SQ_SE_BOARD_RLT_MAN_RLT").empty(),
          "a sequence that starts with its only value has no arrangement");
    Check(SequenceArrangements(kMarioParty10, "SQ_SE_MG0206_JUMP").size() == 2,
          "minigame MG0206's jump has an arrangement for each other value that the game sets");
    Check(SequenceArrangements(kNintendoLand, "SE_TTL_TRAIN_MOVE").size() == 2,
          "the plaza's train has an arrangement for each faster level");
    Check(SequenceArrangements(kSuperMario3dWorld, "SeEvRunnerSqueak").size() == 3,
          "the runner's squeak has three arrangements");
    Check(SequenceArrangements(kWiiPartyU, "SEQ_SE_PLY_LANDING").size() == 12,
          "a Mii's landing in Wii Party U has an arrangement for each of the 12 surfaces");
    Check(SequenceArrangements(kWiiPartyU, "SEQ_BGM_NPG006_THEMA").size() == 3,
          "Wii Party U's theme has an arrangement for each of its three higher levels");
    Check(SequenceArrangements(kWiiPartyU, "SEQ_SE_NMG425_TUBE_SLIDE_1P").size() == 3,
          "a slide in Wii Party U has arrangements at the bottom, the middle and the top of its range");
    Check(SequenceArrangements("", "SeEvRunnerSqueak").empty(), "an unknown archive has no arrangement");
}

// Every arrangement of the listed sequences is named after its variables, and no two of a sequence share a name.
void TestNames()
{
    struct Listed
    {
        std::string_view archive_sha1;
        std::string_view sound;
    };

    constexpr std::array<Listed, 16> kListed = {{
        {kAmiiboFestival, "SQ_SE_ESCAPE_MFA_FLY_WING_LP"},
        {kAmiiboFestival, "SQ_SE_BOARD_LND_ONE_R_01_5"},
        {kMarioParty10, "SQ_SE_MG0K04_KOP_TCH_DRC"},
        {kMarioParty10, "SQ_SE_MG0K06_BAL_ROL_DRC"},
        {kMarioParty10, "SQ_SE_MG0206_JUMP"},
        {kNintendoLand, "SE_PKM_PLAYER_FOOTSTEP"},
        {kNintendoLand, "SE_LIG_OBAKE_LIFE_DOWN"},
        {kNintendoLand, "SE_TTL_TRAIN_MOVE"},
        {kSuperMario3dWorld, "SePvSpinAttack"},
        {kNewSuperMarioBrosU, "SE_SYS_MAP_TOUCH_SCROLL_LV"},
        {kNewSuperMarioBrosU, "SE_OBJ_RC_FENCE_ROLL"},
        {kWiiPartyU, "SEQ_SE_PLY_DAMAGE_BOUND"},
        {kWiiPartyU, "SEQ_SE_NPG009_BALL_HIT_BALL"},
        {kWiiPartyU, "SEQ_SE_PLY_FOOT_RUN"},
        {kWiiPartyU, "SEQ_BGM_NPG006_THEMA"},
        {kWiiPartyU, "SEQ_SE_NMG431_TARU_HIT_KI_RM"},
    }};
    for (const Listed& listed : kListed)
    {
        const std::span<const SequenceArrangement> arrangements =
            SequenceArrangements(listed.archive_sha1, listed.sound);
        bool named = !arrangements.empty();
        for (std::size_t i = 0; i < arrangements.size(); i++)
        {
            named = named && arrangements[i].suffix == NameOf(arrangements[i].variables);
            for (std::size_t j = 0; j < i; j++)
            {
                named = named && arrangements[i].suffix != arrangements[j].suffix;
            }
        }
        Check(named, std::string(listed.sound) + "'s arrangements are named after their variables");
    }
}

// Kirby and the Rainbow Curse's stage select starts its piece with track 7 alone, the area view's track. The piece's 14
// other states follow, each named after its world, with the tracks of the game's table. Another piece plays every
// track.
void TestTrackStates()
{
    const SequenceStart start = StartOfSequence(kRainbowCurse, "BGM_SEQ_MENU_INTERACTIVE");
    Check(start.tracks == 0x0080 && Set(start.variables).empty(), "the stage select starts with track 7 alone");

    const std::span<const SequenceArrangement> states = SequenceArrangements(kRainbowCurse, "BGM_SEQ_MENU_INTERACTIVE");
    Check(states.size() == 14 && states[0].suffix == " world 1" && states[0].tracks == 0x0401 &&
              states[6].suffix == " world 7" && states[6].tracks == 0x0040 && states[7].suffix == " world 1 boss" &&
              states[7].tracks == 0x0501 && states[9].tracks == 0x1104 && states[13].suffix == " world 7 boss" &&
              states[13].tracks == 0x0140,
          "the stage select's states follow the game's table");
    Check(StartOfSequence(kRainbowCurse, "BGM_SEQ_TITLE").tracks == 0xffff, "the game's other pieces play every track");
}

// The labels that the lists hold for each archive, in order. Every listed label has an entry. amiibo Festival's second
// archive holds the last sequence of the game's list.
void TestListedSequences()
{
    const std::vector<std::string_view> rainbow = ListedSequences(kRainbowCurse);
    Check(rainbow == std::vector<std::string_view>{"BGM_SEQ_MENU_INTERACTIVE"}, "Rainbow Curse lists one piece");
    Check(ListedSequences(kWiiPartyU).size() == 101 && ListedSequences(kMarioParty10).size() == 48 &&
              ListedSequences(kNintendoLand).size() == 4 && ListedSequences(kSuperMario3dWorld).size() == 9 &&
              ListedSequences(kNewSuperMarioBrosU).size() == 6,
          "each archive lists its sequences");
    const std::vector<std::string_view> arc1 = ListedSequences(kAmiiboFestivalArc1);
    Check(ListedSequences(kAmiiboFestival).size() == 16 && arc1.size() == 1 &&
              StartOfSequence(kAmiiboFestival, arc1[0]).variables[0].index == SequenceVariable::kNone &&
              !Set(StartOfSequence(kAmiiboFestivalArc1, arc1[0]).variables).empty(),
          "amiibo Festival's second archive holds the last sequence");
    Check(ListedSequences("0000000000000000000000000000000000000000").empty() && ListedSequences("").empty(),
          "an unlisted archive lists nothing");

    bool all_found = true;
    for (const std::string_view archive : {kAmiiboFestival, kAmiiboFestivalArc1, kMarioParty10, kNintendoLand,
                                           kSuperMario3dWorld, kNewSuperMarioBrosU, kWiiPartyU, kRainbowCurse})
    {
        for (const std::string_view label : ListedSequences(archive))
        {
            const SequenceStart start = StartOfSequence(archive, label);
            all_found = all_found && (!Set(start.variables).empty() || start.tracks != 0xffff ||
                                      !SequenceArrangements(archive, label).empty());
        }
    }
    Check(all_found, "every listed sequence has a start or later states");
}

} // namespace

int main()
{
    TestStarts();
    TestArrangements();
    TestNames();
    TestTrackStates();
    TestListedSequences();

    return xsflib::testing::Summary("cafe_sequence_starts");
}

// SPDX-License-Identifier: MIT

// Tests of the tables of what some Wii games set up for their sounds (rvl/snd/game_setups.h): the arrangements that
// Animal Crossing: City Folk starts its music in and plays it in later, made from the masks of its pieces. The town
// music leaves out track 15 in every weather. The museum's music starts in its first arrangement and plays in
// arrangements 2 to 6 later. Wii Music's Pitch Perfect sends its phrases and the jingles of its pictures to bus A.

#include <cstddef>
#include <string_view>

#include "xsflib/testing/check.h"

#include "ntdlib/rvl/snd/game_setups.h"

namespace
{

using namespace ntdlib::rvl::snd;
using xsflib::testing::Check;

void CheckCityFolkStarts()
{
    Check(kCityFolkStarts.size() == 29, "a start for each of the 28 pieces and the museum");
    Check(kCityFolkStarts[0].sound == "BGM_OUTDOOR_00" && kCityFolkStarts[0].tracks == 0x00ff,
          "the first hourly piece starts with its tracks of fine weather");
    Check(kCityFolkStarts[24].sound == "BGM_TOWN_EARLY" && kCityFolkStarts[24].tracks == 0x19ff,
          "the town music starts without track 15");
    Check(kCityFolkStarts[28].sound == "BGM_MUSEUM" && kCityFolkStarts[28].tracks == 0x00e7,
          "the museum's music starts in its first arrangement");
}

void CheckCityFolkVariants()
{
    Check(kCityFolkVariants.size() == 60, "rain and snow for the 28 pieces, and 4 arrangements of the museum");
    Check(kCityFolkVariants[0].sound == "BGM_OUTDOOR_00" && kCityFolkVariants[0].state == "rain" &&
              kCityFolkVariants[0].tracks == 0x01a0,
          "the first hourly piece in rain");
    Check(kCityFolkVariants[49].sound == "BGM_TOWN_EARLY" && kCityFolkVariants[49].state == "snow" &&
              kCityFolkVariants[49].tracks == 0x6600,
          "the town music in snow leaves out track 15");
    Check(kCityFolkVariants[56].state == "arrangements 2 and 3" && kCityFolkVariants[56].tracks == 0x0063,
          "the museum's second arrangement");
    Check(kCityFolkVariants[59].state == "arrangement 6" && kCityFolkVariants[59].tracks == 0x0763,
          "the museum's sixth arrangement");
    for (std::size_t i = 0; i < kCityFolkVariants.size(); i++)
    {
        Check(kCityFolkVariants[i].tempo_ratio == 1.0f, "City Folk's variants keep the tempo");
    }
}

void CheckPitchPerfectSendSounds()
{
    Check(IsPitchPerfectSendSound("RP_KKM_SE_PHRASE_1_1") && IsPitchPerfectSendSound("RP_KKM_SE_PHRASE_1_6"),
          "Pitch Perfect's phrases get the send");
    Check(IsPitchPerfectSendSound("RP_KKM_SE_IMG_OMEDETO") && IsPitchPerfectSendSound("RP_KKM_SE_IMG_KEIJI"),
          "the jingles of Pitch Perfect's pictures get the send");
    Check(!IsPitchPerfectSendSound("RP_KKM_BEATS_LOOP") && !IsPitchPerfectSendSound("RP_MUSIC_MAINMENU_BACKTRACK"),
          "the beats and the main menu's music get no send");
}

} // namespace

int main()
{
    const auto tests = []
    {
        CheckCityFolkStarts();
        CheckCityFolkVariants();
        CheckPitchPerfectSendSounds();
    };

    return xsflib::testing::RunTest("rvl_game_setups", tests);
}

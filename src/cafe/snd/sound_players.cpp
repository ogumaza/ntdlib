// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/sound_players.h"

#include <array>
#include <cstdint>
#include <string_view>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/stream_sound_player_impl.h"
#include "ntdlib/nwsnd/wave_sound_player.h"
#include "ntdlib/nwsnd/wave_sound_player_impl.h"

template class ntdlib::nwsnd::WaveSoundPlayer<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::StreamSoundPlayer<ntdlib::cafe::snd::SequencePolicy>;

namespace ntdlib::cafe::snd
{
namespace
{

struct StartingTracks
{
    std::string_view archive_sha1;
    std::string_view sound;
    uint16_t tracks;
};

// The SHA-1s of the USA releases' sound archives.
constexpr std::string_view kNintendoLand = "1379ab0c17a80e0c38e0c9e90aaf92e971f2c1a4";
constexpr std::string_view kGameAndWario = "4d2c7f19370a0eea57de309b950c6af937e6e01f";
constexpr std::string_view kAmiiboFestival = "5784acb45299369f8388246f0ba4311610c709bc";

// Each game starts these stream sounds with one track at its volume and the others at 0, with no fade, through
// StreamSound::SetTrackVolume (Nintendo Land 0x02AE70AC, Game & Wario 0x0300DD34, amiibo Festival 0x022C2814). The
// list gives the start's tracks. Later states of the games change them.
//
// Nintendo Land plays track 0 of its plaza's BGMs in the plaza's first mode (0x028D64E4). It plays one track of Balloon
// Trip Breeze's four by two flags. Both flags start clear (0x028B6C64). It plays track 0 of Donkey Kong's Crash
// Course's opening and result (0x028BA6DC), of Metroid Blast's stages (0x028C5AF4), of Pikmin Adventure's fields
// (0x028CE3D4) and of Battle Quest's fields (0x028DECF4). Some of Battle Quest's stages start track 1 alone. Captain
// Falcon's Twister Race starts a section on track 1 alone, and its next state fades track 0 in (0x028BC5E0). The list
// leaves those streams at every track.
//
// Game & Wario plays track 0 as BGM_AR_01 (0x022A02A4), BGM_MM_01 (0x02146A00), BGM_SK_ENDLESS1 (0x021ABD10) and
// BGM_BO_CHALLENGE (0x025D8258) start. amiibo Festival's board plays track 0 of its two BGMs, or track 1 in weathers
// 2 and 3 (bd::BoardBgm::changeMainTurnBgmTrack, 0x0246A044).
constexpr std::array<StartingTracks, 31> kStartingTracks = {{
    {kNintendoLand, "BGM_TTL_MAIN_DAYTIME", 0x1},
    {kNintendoLand, "BGM_TTL_MAIN_EVENING", 0x1},
    {kNintendoLand, "BGM_TTL_MAIN_NIGHT", 0x1},
    {kNintendoLand, "BGM_BLF_MAIN_MORNING", 0x1},
    {kNintendoLand, "BGM_BLF_MAIN_NOON", 0x1},
    {kNintendoLand, "BGM_BLF_MAIN_EVENING", 0x1},
    {kNintendoLand, "BGM_BLF_MAIN_NIGHT", 0x1},
    {kNintendoLand, "BGM_DKB_DEMO_OPENING", 0x1},
    {kNintendoLand, "BGM_DKB_RESULT", 0x1},
    {kNintendoLand, "BGM_MTR_STAGE_01", 0x1},
    {kNintendoLand, "BGM_MTR_STAGE_02", 0x1},
    {kNintendoLand, "BGM_MTR_STAGE_03", 0x1},
    {kNintendoLand, "BGM_MTR_STAGE_05", 0x1},
    {kNintendoLand, "BGM_MTR_STAGE_06", 0x1},
    {kNintendoLand, "BGM_MTR_STAGE_07", 0x1},
    {kNintendoLand, "BGM_PKM_MAIN_SPRING", 0x1},
    {kNintendoLand, "BGM_PKM_MAIN_SUMMER", 0x1},
    {kNintendoLand, "BGM_PKM_MAIN_AUTUMN", 0x1},
    {kNintendoLand, "BGM_PKM_MAIN_WINTER", 0x1},
    {kNintendoLand, "BGM_PKM_MAIN_UNDER", 0x1},
    {kNintendoLand, "BGM_ZLD_YAMA", 0x1},
    {kNintendoLand, "BGM_ZLD_YAMA_YUGATA", 0x1},
    {kNintendoLand, "BGM_ZLD_YAMA_YUGATA_FIELD_6", 0x1},
    {kNintendoLand, "BGM_ZLD_TATEMONO", 0x1},
    {kNintendoLand, "BGM_ZLD_TATEMONO_PRE_BOSS", 0x1},
    {kGameAndWario, "BGM_AR_01", 0x1},
    {kGameAndWario, "BGM_MM_01", 0x1},
    {kGameAndWario, "BGM_SK_ENDLESS1", 0x1},
    {kGameAndWario, "BGM_BO_CHALLENGE", 0x1},
    {kAmiiboFestival, "SM_BGM_BD_TH_01_SUNNY", 0x1},
    {kAmiiboFestival, "SM_BGM_BD_TH_02_SUNNY", 0x1},
}};

} // namespace

uint16_t StartingStreamTracks(std::string_view archive_sha1, std::string_view sound)
{
    for (const StartingTracks& entry : kStartingTracks)
    {
        if (entry.archive_sha1 == archive_sha1 && entry.sound == sound)
        {
            return entry.tracks;
        }
    }

    return ntdlib::nwsnd::kAllStreamTracks;
}

} // namespace ntdlib::cafe::snd

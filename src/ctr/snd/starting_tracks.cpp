// SPDX-License-Identifier: MIT

#include "ntdlib/ctr/snd/starting_tracks.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ntdlib/nwsnd/sound_players.h"

namespace ntdlib::ctr::snd
{

namespace
{

struct StartingTracks
{
    std::string_view archive_sha1;
    std::string_view sound;
    uint16_t tracks;
};

// The SHA-1s of the USA releases' sound archives. Happy Home Designer's first is revision 1's. Its second is the
// archive of another release of the game. That archive holds the sound at the same index and ID.
constexpr std::string_view kMarioKart7 = "cfd8fb5750069922dc631708d883fcc7374f8fa5";
constexpr std::string_view kSuperMario3DLand = "bfb2742fc5e8f2869c9ce781710ba393f675fbc1";
constexpr std::string_view kNewLeaf = "54f0fbc801c85ea2035219923aa4cd955e6f4cc7";
constexpr std::string_view kHappyHomeDesigner = "8c07814daf28befb015cc97dd2c66699254035be";
constexpr std::string_view kHappyHomeDesignerOld = "30c98d66881296eda5e676964918e7003b403c75";
constexpr std::string_view kTomodachiLife = "200b87d7d1152f5035b29a092f2e20e726695264";

// Each game sets the tracks of these stream sounds through StreamSound::SetTrackVolume (Mario Kart 7 0x271470, New Leaf
// 0x52CF80, Super Mario 3D Land 0x2BE57C, Happy Home Designer 0x3257B4, Tomodachi Life 0x711690). The list gives the
// tracks of the start, or of the first state in which the sound plays. Later states of the games change the tracks.
//
// Mario Kart 7 chooses a course's BGM from a table (0x3C7BF0). Four of the courses mark it as a BGM of two tracks.
// The start of that BGM (0x3C8004) and the start of the final lap's BGM, the next sound (_F, 0x3C7DA4 through
// 0x3C80E4), then play track 0 alone, with no fade (0x3C86E0). A later state crossfades to track 1 (0x3C7CA0), or
// switches to it (0x3C7AAC). A menu's scene chooses its BGM and one of its tracks (0x3D0E6C, through 0x3D0830). The
// first scenes of STRM_MENU_SINGLE_MULTI (2 and 3) and of STRM_MENU_WIFI (25 to 27) play track 0. STRM_MENU_CHANNEL
// plays track 1 in scenes 51 to 54 and track 0 in scenes 55 to 60. Scene 51 is MenuChannel_Top, the page through
// which the player enters the channel. Each page's constructor stores the number of its scene (0x48CC20).
//
// Super Mario 3D Land plays its BGMs through a controller of 4 tracks (0x1D844C). After each start (0x23F7A4), it
// sets the tracks with no fade (0x1E9130 and 0x1D8638) from the BGM's entry in a table of the game's data and two
// states. With the console's speakers and the player out of the water, each BGM plays track 0 alone. A BGM whose entry
// sets IsUseHeadPhoneSwap plays track 1 alone with headphones. STM_BGM_BEACH sets IsUseWaterSwap too, and it plays
// track 2 in the water with the speakers, or track 1 with headphones.
//
// New Leaf's BGM manager gives each BGM a state (0x1E1734). Right after the start, a routine sets the tracks of the
// state with no fade (0x1E12E0). Later calls follow the player's scene. STRM_BGM_IND_KODANUKI_HOMECENTER plays track 0
// in the HomeCenter scene and track 1 in Florists_HomeImprovementCenter (0x1E2F98). STRM_BGM_IND_ABLESISTERS plays
// track 0 in the Tailor scene and track 1 in AccessoryShop (0x1E20E0). The list gives the HomeCenter's and the Tailor's
// tracks. STRM_BGM_IND_MUSEUM plays with SEQ_BGM_IND_MUSEUM. A table of masks sets the tracks of both (0x1E2590).
// The stream is silent in the museum's entrance and its shop. It plays track 1 in the fish rooms and track 0 in the
// insect rooms. The list gives track 1, from the lowest scene in which the stream sounds. STRM_BGM_EVT_CARNIVAL and
// STRM_BGM_EVT_EASTER start with track 1 at 0 (0x1E2BC8). Later, the volume of track 1 follows a fader and the
// distance to a point of the event. A routine of the carnival's events starts STRM_BGM_EVT_CARNIVAL on a second
// handle with track 0 at 0 (0x1E100C). The game plays the _SUR sounds only when a byte of nw::snd's state is 2
// (0x1E1734). The byte is likely the output mode. In nn::snd, mode 2 is surround. The game sets none of their tracks.
//
// Happy Home Designer starts STRM_BGM_AMIIBOPHONE and then sets track 1 to 0 (0x53DB78). A later state sets it to 1
// (0x53FC64). Tomodachi Life starts BGM_MANSION_INFOMATION and then fades track 1 to 0 over 10 frames (0x409F90 and
// 0x43E020), or sets it to 0 with no fade (0x43E0B4). A later state fades it back to 1 (0x4C641C).
constexpr std::array<StartingTracks, 49> kStartingTracks = {{
    {kMarioKart7, "STRM_MENU_SINGLE_MULTI", 0x1},
    {kMarioKart7, "STRM_MENU_WIFI", 0x1},
    {kMarioKart7, "STRM_MENU_CHANNEL", 0x2},
    {kMarioKart7, "STRM_CN_UNDERGROUND_N", 0x1},
    {kMarioKart7, "STRM_CN_RAINBOW_ROAD_N", 0x1},
    {kMarioKart7, "STRM_CN_WARIO_SHIP_N", 0x1},
    {kMarioKart7, "STRM_CR_WII_NOKONOKO_CAPE_N", 0x1},
    {kMarioKart7, "STRM_CN_UNDERGROUND_F", 0x1},
    {kMarioKart7, "STRM_CN_RAINBOW_ROAD_F", 0x1},
    {kMarioKart7, "STRM_CN_WARIO_SHIP_F", 0x1},
    {kMarioKart7, "STRM_CR_WII_NOKONOKO_CAPE_F", 0x1},
    {kNewLeaf, "STRM_BGM_EVT_CARNIVAL", 0x1},
    {kNewLeaf, "STRM_BGM_IND_KODANUKI_HOMECENTER", 0x1},
    {kNewLeaf, "STRM_BGM_IND_ABLESISTERS", 0x1},
    {kNewLeaf, "STRM_BGM_IND_MUSEUM", 0x2},
    {kNewLeaf, "STRM_BGM_EVT_EASTER", 0x1},
    {kSuperMario3DLand, "STM_BGM_TRUCK_B", 0x1},
    {kSuperMario3DLand, "STM_BGM_TRUCK_B_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_SENKAN", 0x1},
    {kSuperMario3DLand, "STM_BGM_SENKAN_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_MUTEKISTAR", 0x1},
    {kSuperMario3DLand, "STM_BGM_GOAL_01", 0x1},
    {kSuperMario3DLand, "STM_BGM_FIELD_WALK", 0x1},
    {kSuperMario3DLand, "STM_BGM_FIELD_WALK_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_JG_MISS", 0x1},
    {kSuperMario3DLand, "STM_BGM_JG_GAME_OVER", 0x1},
    {kSuperMario3DLand, "STM_BGM_BEACH", 0x1},
    {kSuperMario3DLand, "STM_BGM_BEACH_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_STAFF_ROLL", 0x1},
    {kSuperMario3DLand, "STM_BGM_GHOST", 0x1},
    {kSuperMario3DLand, "STM_BGM_GHOST_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_CLOCK_TOWER", 0x1},
    {kSuperMario3DLand, "STM_BGM_CLOCK_TOWER_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_ATHLETIC", 0x1},
    {kSuperMario3DLand, "STM_BGM_ATHLETIC_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_DESERT", 0x1},
    {kSuperMario3DLand, "STM_BGM_DESERT_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_DOTCOIN", 0x1},
    {kSuperMario3DLand, "STM_BGM_DOTCOIN_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_KOOPA_01", 0x1},
    {kSuperMario3DLand, "STM_BGM_KOOPA_01_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_KOOPA_02", 0x1},
    {kSuperMario3DLand, "STM_BGM_KOOPA_02_FAST", 0x1},
    {kSuperMario3DLand, "STM_BGM_WORLD3_A", 0x1},
    {kSuperMario3DLand, "STM_BGM_KINOHOUSE", 0x1},
    {kSuperMario3DLand, "STM_BGM_DEMO04_B", 0x1},
    {kHappyHomeDesigner, "STRM_BGM_AMIIBOPHONE", 0x1},
    {kHappyHomeDesignerOld, "STRM_BGM_AMIIBOPHONE", 0x1},
    {kTomodachiLife, "BGM_MANSION_INFOMATION", 0x1},
}};

struct StartingSequence
{
    std::string_view archive_sha1;

    // The sound's name. With `prefix`, the start of the names of the sounds that the entry covers.
    std::string_view sound;

    SequenceStart start;
    bool prefix = false;
};

// The SHA-1s of more sound archives: those of the USA releases of Ocarina of Time 3D and Island Tour, WarioWare Gold's,
// and those of four add-on games of StreetPass Mii Plaza. The four come from the Plaza's add-on content, version
// 0.2.0 (USA). The European add-on content holds the same files. The Plaza's update v14.6.0 holds the add-ons' code.
constexpr std::string_view kOcarinaOfTime3D = "c3fdb98e65e6143d76e6d971cfaaac058455300d";
constexpr std::string_view kIslandTour = "e9d5d40aa7b034360905ecb171a5825b37688c6c";
constexpr std::string_view kWarioWareGold = "6b471abb246fcaa2cf13ffbf16345e5d0c02e7e1";
constexpr std::string_view kUltimateAngler = "a98dd3c22da8e4bed8327e4fc98196e102ce53ea";
constexpr std::string_view kFlowerTown = "331b572b09819830436fe8a09673b22fc6f7bdae";
constexpr std::string_view kNinjaLauncher = "ddb93158672258c7d8ea2b5edb8662bda34234e6";
constexpr std::string_view kMiiForce = "e566c28135170a936f1f6e4538a15d745f3b2a9e";

// The SHA-1 of the Plaza's sound archive of its update v14.6.0. Older builds of the Plaza hold another archive.
constexpr std::string_view kMiiPlaza = "33b514ee80f6a6df3034bc8774235e40dfa52d1c";

// The SHA-1 of the sound archive of the Plaza's system title. The system title plays without the update. The European
// system title of system update 11.17.0-50E holds the archive. The archive's seven From_New_Mii sequences match those
// of v14.6.0, command for command. The system title's code plays them by ID (0x2D5180).
constexpr std::string_view kMiiPlazaSystemTitle = "c2550ce04d4c8c44e92c2d72682038616fbec658";

// The SHA-1s of the sound archives of four European system titles of the 3DS: System Settings (0004001000022000), a
// second System Settings title (0004001000022F00), the Activity Log (0004001000022200) and Mii Maker
// (0004001000022700).
constexpr std::string_view kSystemSettings = "ff5231c919ad81331321e6e29e24d84bbf2127cc";
constexpr std::string_view kSystemSettings2 = "fd187b38953c162695779f26c66140859a63e74b";
constexpr std::string_view kActivityLog = "ea55099f2b92479898b4eda7956d645021dac2d3";
constexpr std::string_view kMiiMaker = "0cc99a6732a7dbc0d6e8887449233ab25396672c";

// The SHA-1 of the sound archive of Mario Party: Star Rush (USA).
constexpr std::string_view kMarioPartyStarRush = "90fa0978d0f4e3af259a48b48141d3e58ab5e66e";

// Each game quiets these sequences' tracks through the sound's handle as they start, or sets their variables. The list
// gives the start, or the state in which the game plays the sound most. Later states of the games change the tracks
// or the variables. kArrangedSequences gives some of those states.
//
// Ocarina of Time 3D's actors of Malon (0x1D9D54, 0x1DA004 and 0x1DA290) set the volumes of NA_BGM_RONRON's tracks by
// her distance from Link (0x33D364). Tracks 0 and 1 hold her voice. Their volume goes from full at 200 units to 0 at
// 2,000. Track 13 plays the same melody on an instrument. It takes 1 minus that volume (0x33D408 and 0x33D418, through
// the setter of a track's volume at 0x3560D8). The list gives the state near Malon, with track 13 at a volume of 0.
//
// Island Tour plays SEQ_BGM_MENU in four layers from a table of the game's data: tracks 0 to 3, 0 to 6, 0 to 9 or all
// 16. The menu's flow starts the piece with the first layer's tracks (0x158D60, through 0x39BB04) and mutes the other
// tracks (0x18DD80). Each menu screen then fades to its layer over a second (0x272D00 and 0x2FFCCC). The main menu
// plays tracks 0 to 3 (0x16336C). kArrangedSequences gives the other three layers.
//
// WarioWare Gold's code acts on the starts of three Tricky jingles (vtable 0x83B724, hook 0x361D58). A flag silences
// one arrangement with SetTrackSilence and lets the other play, with fades of 10 frames (0x361CC4). Flag 0 plays
// tracks 12 to 15. Flag 1 plays tracks 0 to 13. The flag starts at 0 (0x36AC68). A trick's request sets it to 1
// (0x362664). The end of the cut-in that shows the trick sets it to 0 (0x36A9C0). The trick's microgames and their
// jingles play at flag 0. The game fades tracks 0 to 11 out over a jingle's first 10 frames. The list silences them
// from the start.
//
// New Leaf plays SEQ_BGM_IND_MUSEUM beside STRM_BGM_IND_MUSEUM (BGM mode 3, 0x1E1E64). A table gives each state the
// sequence's tracks that play (0x8BA11C). The music's start gives the museum's entrance and its shop state 2
// (0x1E1330). A routine mutes the other tracks before they sound, with no fade (0x1E2590). State 2 plays tracks 0 to 3
// and 10 to 12.
//
// New Leaf's DJ K.K. plays each song in one of two styles, SEQ_BGM_DJnn_HS_* and SEQ_BGM_DJnn_FC_*. Between songs he
// plays an interlude: SEQ_BGM_DJ_HS_HANIWA, SEQ_BGM_DJ_HS_MURAMELO and their FC twins. Each song waits at its start
// until player variable 0 is 0 or 1. At 0 it plays an intro first. At -1 it plays nothing. DJ K.K.'s player (0x28B8E8)
// takes the song's sound from the table at 0x8BA1D0 by the style and the low 7 bits of the song's number, with 0x5E
// read as 0x5B (0x28BA38 to 0x28BA5C). Once the sound is prepared, the player writes variable 0 (0x28BABC to 0x28BAF4).
// It writes 1, or 0 for a number with bit 7 set. In two of the playlist's modes, an entry with bit 7 set becomes the
// birthday song (0xDA) or the event song (0xDE), with bit 7 kept (0x28AE04 to 0x28AE1C). The list starts those two
// songs at 0 and the others at 1. The interludes wait for 0 the same way. The player writes 0 to them (0x28B9B8 to
// 0x28B9C8).
//
// DJ K.K. plays only in the club's scene CLUB444_DJKK (0x46). Only DJ K.K.'s player reads the songs' table, and the
// club's code at 0x344144 calls it. New Leaf's effects manager (0x1B29D4) builds the game's effects. Effect 0 is an
// nn::snd FxReverb (0x1B2AE4). The scene change (0x1B3070) starts effect 0 for scene 0x46. The level setter (0x1B2E28)
// then gives PLAYER_BGM, the songs' player, a send of 0.4 on bus A from the table at 0x8BB488. The songs therefore
// always play with effect 0 at 0.4.
//
// New Leaf's BGM manager gives SEQ_BGM_IND_CAFEARBEIT, the café job's piece, mode 10 (0x1E1810). The piece's start in
// that mode sets track 7's volume to 0 and mutes the track (0x1E151C to 0x1E1560). At an event of the job, the café
// unmutes track 7 and fades it in over 40 frames (0x1E2E50, from 0x420404). The list gives the start. kCafeTrack7 gives
// the state after the fade.
//
// SEQ_BGM_DJ_HS_MURAMELO and SEQ_BGM_DJ_FC_MURAMELO also play the town's tune four times on track 15. Before each step,
// the track copies the next of its variables 0 to 15 into player variable 4. A value of 10000 mutes the step. A value
// below -1000 picks a random note. Any other value transposes note 60. No other track reads a track variable. DJ K.K.'s
// player (0x28B08C) writes the 16 values to track 15 (SetTrackVariable, 0x52EACC) from the tune that the save holds. A
// note gets its transpose in the major scale (0x411284) plus the key of the song in the player's slot. The key comes
// from the song's entry in the table at 0x8BB51C (0x7ADE6C and 0x7ADDA0). A rest or a hold gets 10000. A random note
// gets the key less 10000. A new town takes one of four default tunes at random (0x7B7F40, from the table at 0x8BBCE2).
// The list gives the first of them in key 0. Key 0 is the commonest key of DJ K.K.'s 94 songs: 24 of them take it. A
// save can hold any tune, and the songs' keys run from -5 to 6. At -1, every step plays B, a semitone under note 60.
//
// New Leaf's tune player (0x411330) plays a tune of 16 steps one note at a time. Most players take the town's tune
// (0x410F64). Each note starts the player's SE_MELODY_ sound again (0x411534). The player writes variable 15 = 0,
// variable 14 = the tune's note, variable 13 = a delay in ticks, and variable 12 = the note's transpose. The note is 0
// to 12, or 13 to 18 for a random one. The transpose comes from a table of ten scales of 19 notes (0x8BBD4C), plus an
// offset. Each sound gives its scale and its offset. Started once with variable 15 = 1, the sound writes them to its
// variables 4 and 5. The player reads them there (0x410F90 to 0x411148). At -1 a sound plays no note. TuneStart gives
// each sound the tune's note 3, the C, and the transpose of its scale and offset. Variables 15 and 13 stay at -1. The
// sequences compare variable 15 only with 1. They set variable 13 to 0 when it's -1. SE_MELODY_TRAIN_ANNOUNCE plays a
// fixed tune (0x8BBD22, which 0x217BB8 loads). TuneStart gives it the tune's first note, 9. Each sound holds its note
// for the length that the sequence gives it. The game stops each note after variable 1 of the sound's block times 1.5
// frames (0x410CF4, 0x411120). A held step lets the note sound on.
//
// Tomodachi Life plays BGM_MANSION_AND_ROOM in a Mii's apartment, or BGM_AKICHI_NIGHT when a flag of the room is set
// (0x7F84FC). Each piece holds several arrangements. A chooser gives the tracks that play by the Mii's state
// (0x7A7260). The player mutes the other tracks with release as the piece starts (0x7A4ECC, 0x7A5024 and 0x7A5038).
// The list gives the chooser's fallback: tracks 0 to 2 of BGM_MANSION_AND_ROOM (0x7A7678) and tracks 4 to 7 of
// BGM_AKICHI_NIGHT (0x7A7314).
//
// The chooser gives BGM_MANSION_AND_ROOM five other sets of tracks. It checks them in this order. Tracks 3, 4 and 10
// play while the byte at +0x1A1 of the object at 0xACB624 is set (0x7A74B4). Tracks 0, 6 and 12 play in a state of kind
// 2 of the Mii (0x7A7518). Tracks 5, 9 and 13 play in a state of kind 6 with the Mii's cold or stomach ache, tl_cold or
// tl_stomach (0x7A7558 to 0x7A75E8). Track 15 plays alone while 0x7E29C4 reports a state of the Mii (0x7A7614). Tracks
// 0, 8 and 14 play while the byte at +0x2BC of an object of the Mii is 1 (0x7A766C). BGM_AKICHI_NIGHT plays tracks 8 to
// 11 while 0x7DB8AC reports a state of the room (0x7A72E8). The player mutes the other tracks, as at the start.
// BGM_AKICHI_NIGHT has tracks 0 to 11 only. kApartmentStates and kNightApartmentStates give these sets.
//
// Tomodachi Life's SE_MII_PLAY_GUITAR_S, _M and _L play a Mii's guitar in the chord of the apartment's music. They
// pick their notes by global variable 10. No code writes it. BGM_MANSION_AND_ROOM writes the chord to it as it plays,
// 0 to 4, and 0 first (the sequence's offset 0x5F). At -1 the guitar plays nothing. The list gives the music's first
// chord. This rests on a reading of the music's bytecode.
//
// Channel 12's show on the TV of New Leaf and Happy Home Designer runs for 483 frames in three parts. The show's
// routine (New Leaf 0x2D4690, Happy Home Designer 0x1E3E74) starts each part's effect. It rewrites the effect's
// variable 0 every frame with the ticks left in the part, 3.2 a frame. SE_TV_12_RUN loops while the count is 80 or
// more. The list gives the count at the run's start, 928. The jump's and the crash's effects play only as the count
// falls through their tracks' windows. No start plays them.
//
// Ultimate Angler, an add-on game of StreetPass Mii Plaza, holds a speaker mix and a headphone mix of one part on
// tracks 14 and 15 of its pieces. As each piece starts on the main BGM player (0xD52678), the game reads whether
// headphones are plugged in (0xD3C284). It mutes track 15 on the speakers and track 14 on headphones (0xD52C74 and
// 0xD52D88). The mute goes through 0x33E834, which builds the mute command (0x16) in the mode that keeps the
// channels sounding (SEQ_MUTE_NO_STOP). The program builds no silence command. The game starts SEQ_BGM_SPOT_00_A on a
// second player (0xD52718). SEQ_BGM_CROSS holds two arrangements: tracks 0 to 9 with tracks 14 and 15, and tracks 8
// to 13 (0xD50C70). It starts in the first, with no fade (0xD52B3C). The other tracks get a volume of 0 (0xD3BEB0,
// through 0xD3F5D4 and 0xD3D0B0 to the builder of the volume command at 0x33E7DC). The list gives the speakers'
// state.
//
// Each of Ultimate Angler's four reeling pieces holds a layer that plays only at tension level 2 and up (0xD508D8):
// track 5 of SEQ_BGM_REELING_NORMAL, track 8 of _BIG, track 6 of _KAIGYO and track 12 of _LASTBOSS. Below level 2,
// the layer plays at a volume of 0. Each piece starts so (0xD5274C): _NORMAL through the event
// EVENT_TRACK_REELING_BATTLE_NORMAL_LV0 of the archive's user parameters, the others through 0xD3BEB0. _KAIGYO and
// _LASTBOSS also mute the layer as they start (0xD52884 and 0xD52ACC). The first update of the tension unmutes it,
// at a volume of 0 (0xD50A44). The list gives the start, with the layer silenced.
//
// The ending's cutscene starts SEQ_BGM_ED2_DEMO with tracks 0 and 1 (0xD52B64). It mutes tracks 2 to 13 and gives
// them a volume of 0. The list gives that start.
//
// The home scene starts its ambience, SE_ENV_HOME_CROSS, with tracks 0 and 1, and mutes the other tracks (0xD51CAC,
// group 7 of the table at 0xEE4DF8, through 0x750804 and 0x75081C).
//
// StreetPass Mii Plaza starts SEQ_USAGI and sets its state "NORMAL" at once (0x2650A8 and 0x2650C4). A table gives
// each of five states a mask of tracks, bit N for track N (0x10A26A8). Each frame, a fader moves each track's volume
// toward its state, and mutes a track whose volume is 0 (0x2A9820, through 0x33E7DC and 0x33E6E4). NORMAL plays tracks
// 0, 2 to 7 and 9 to 11. The list gives that state.
//
// The Plaza's seven pieces that follow its new Mii jingle, BGM_MAIN_Mii_*_From_New_Mii, copy global variable 16 as
// they start. Each waits until the variable changes. The jingle, BGM_NEW_Mii or BGM_NEW_Mii_Long, counts its beats in
// that variable. The Plaza's tables of music list the pieces with the jingles (0xED9928 and 0xEFD01C). In the game each
// piece starts on the jingle's next beat. The list changes the variable at frame 1. The piece then starts at once.
// The system title's archive holds the same seven pieces. Its BGM_NEW_Mii_Long counts the beats in the same way.
//
// The Plaza's music manager writes one global variable, 17. No sound reads it. The manager sets it to 0 as a
// From_New_Mii piece starts (0x3A237C). The piece sets it to 1 two beats in. The manager then stops the sound of its
// handle at +0x10 with a fade of 5 frames (0x3A18EC). That handle isn't the piece's: the manager adds the piece's
// handle to a list (0x3A23A4).
//
// Some sounds write a global variable that another sound reads. Ultimate Angler's motor sounds set global variable 31
// to 0 (SE_ANIM_CRUISER_MOTOR, for one). SE_ANIM_CRUISER_JUMP_IN_MY_MII adds 1 to it as each Mii jumps into the
// cruiser. The two jump-in sounds take a pitch and a volume by its value, 0 to 9. The list
// gives the first jump-in, at 0. At the default of -1, the sounds would play a pitch that the game never plays.
//
// Ultimate Angler's SE_FEVER_PULL_2 bends one track by player variable 0, with a bend range of 50 semitones: 39 cents
// for each step. The game starts the sound with the fever's pull count in the variable, then adds 1 to the count, up
// to 20 (0xD51420). The count goes back to 0 as a fever starts (0xD51804). The list gives the first pull, at 0. At the
// default of -1, the sound would play 39 cents flat. Each later count is an arrangement, for the second pull to the
// 21st and later ones.
//
// Ultimate Angler's SE_MIIS_VOICE, _HARD, _START and _START_FEVER take the crowd's state in player variable 0
// (0xD51F1C, 0xD521AC, 0xD517C0 and 0xD51470). 0xD50E00 builds the state from the counts of three kinds of Mii
// (0x75BE18). Bits 0 and 1 give kind A's level: 1 Mii, 2 to 6, or 7 to 10. Bits 2 and 3 give kind B's level. With no
// Mii of kind B, bit 4 is set, and bits 2 and 3 give kind C's level. Tracks 0 and 2 play kind A's voices, and tracks 1
// and 3 kind B's or kind C's. The list gives the fullest crowd, 0x1F, which plays as the default of -1 does. Each other
// state that 0xD50E00 can give is an arrangement, 26 in all. The state with no Mii of any kind is left out. It plays
// nothing.
//
// Flower Town, another add-on game of the Plaza, plays BGM_FLOWER_ATELIER in one group of tracks for each state of the
// atelier. A table gives each state its tracks (0xEDE978), and a fader fades the other tracks out (0x7167B4). The
// atelier's code sets the state "Normal", tracks 0 to 5 at full volume, in seven places (0x6C3A38 to 0x6C9140). No
// other state is set in as many. The list gives that state.
//
// Flower Town's SE_COLOR_WHEEL_LIGHT sets global variable 19 to 0. SE_COLOR_WHEEL_LIGHT_2 then plays its first note
// at 0 and its second at 1, and adds 1 each time. At any other value it plays nothing. The list gives the first note.
//
// Ultimate Angler's SE_CATCHED_092 and SE_CATCHED_120 draw a random number as they start. They play their sound on
// one draw in five and one in three, a quieter one on some of the other draws, and nothing on the rest. The list gives
// each a state of the generator that plays the sound: the states 28 and 19 steps after nw::snd's first state. The
// player's state at a sequence's first tick is the one 17 steps after it.
//
// The system titles link a sound library that sets the level of each of a piece's 16 tracks. System Settings, the
// second System Settings title and the Activity Log take a mask from the high halfword of a sound's user parameter
// (System Settings 0x13F62C, through 0x141728). Bit 15 - N keeps track N (0x113490 gives track group N the mask
// 1 << N). As a piece starts, the player mutes each other track in the mode that stops its channels (SEQ_MUTE_STOP,
// 0x131564, through 0x1384E4 and the builder of the mute command at 0x137E30). No parameter, or a mask of 0, keeps
// every track. A later piece of the same sequence fades the tracks to its mask instead of starting again (0x13138C,
// through 0x138658). In System Settings, BGM_CTR_MSET keeps tracks 0 to 6, BGM_CTR_MSET_NET tracks 7 to 15 and
// BGM_CTR_MSET_FIRST track 3, all of one sequence. The second System Settings title's archive holds the same sounds
// and parameters (0x222FD4). In the Activity Log (0x1CCCB0), BGM_CTR_PLOG_MAIN keeps tracks 0 to 9 of its 14,
// BGM_CTR_PLOG_MAIN_CLOCK tracks 0 to 11 and BGM_CTR_PLOG_MAIN_STEP tracks 0 to 9, 12 and 13.
//
// Mii Maker sets the levels of MEDIT_BGM_FACE's tracks from its code (0x19E97C, through SetTrackVolume at 0x1EFF90).
// Its first screen starts the piece and sets tracks 8 to 15 to a volume of 0 at once (0x27D0FC). The list gives that
// start. Two other routines give the same tracks (0x27D038, and 0x1C4718 with the sound's volume at 0.7). Two more
// give the later mixes. 0x27D470 (state 6 of a screen's update at 0x171344) sets the sound's volume to 0.7 (0x19EAD0),
// tracks 8 and 9 to 0.8 and tracks 3, 4 and 7 to 0. 0x27D9E4 (state 8, at 0x171470) also sets track 10 to 0.75. While
// the piece plays, each fades over 30 frames, and the tracks that go out fade over 60. Tracks 10 to 15 keep the first
// screen's volume of 0 through 0x27D470. kMiiMakerMixes gives the two mixes, named by the tracks that they add. A
// Unicorn run of each routine from a hand-set state gives the values. Which screens run the two states wasn't mapped.
//
// Seven of Mario Party: Island Tour's minigame and boss pieces set their tempo from player variable 0. A track moves a
// copy of the variable toward it every 2 ticks, clamps the copy and scales it (TEMPO_CONTROL_START). The game writes
// the variable as the piece plays. Each minigame's code is in a separate module (rom:/dll/mod_mg*.cro.p), and the
// addresses below are offsets in its file. Azahar runs that patched the game into each minigame give the values.
// mg3801 starts SEQ_BGM_MNG_MG3801 (0xF4C0) with row 10 of rom:/ts_sound_ctrl_track.bin (0xF4D4) and writes the
// player's time left out of 100 seconds (0xBE18). The piece plays at 145 BPM down to 50 seconds, 165 at 20 and 215 at
// 0. At 20 seconds the game moves to row 11, and back to row 10 at 40 (0xBE9C and 0xBE60).
// mg3802 starts SEQ_BGM_MNG_MG3802 (0x9C40) and writes the time left, 99 as the piece starts (0x614C). The piece plays
// at 123 BPM down to 30 seconds, 160 at 20 and 230 at 0.
// mg3704 starts SEQ_BGM_BOSS_04_A (0x13DC) and writes 20 at once (0x13EC). Each round then writes its timer (0xFD4 and
// 0x2F5C). The timer starts at 20 seconds. The piece plays at 122 BPM at 20 seconds and 170 at 0. A round that starts
// with the value at 0xEA4 at 50 or less starts _B, _B2 or _B3 (0xF00, 0xF30 and 0xF48). Those play at 148 BPM at 20
// seconds and 170 at 0. Runs patched that value to 50 and the choice at 0xED0 to 0, 1 and 2. They started each piece,
// and each wrote 20.
// mg3419 starts SEQ_BGM_MNG_012 (0x1C44) with row 7 (0x1C54) and writes the ball's speed times 96 (0x1E6C, 0xE4D0).
// The piece plays at 60 BPM with the ball at rest. With the tilt patched to its maximum, the speed stays at 1 and the
// variable at 96, or 165 BPM. Rows 8 and 9 come with the byte at 0x1DEC at 2 and 3 (0x1E10 and 0x1E2C). A run patched
// that byte reached both.
// The list starts each piece at the value of its first seconds, or of full speed. kArrangedSequences gives the rest.
//
// Mario Party: Star Rush plays the music of minigame 708 from one sequence. The minigame's code is in a separate
// module, romfs/dll/mod_mgn708.cro.p. The offsets below are offsets in that file. The module writes the music's
// variables through the sound's handle (nd::SysAudio::SoundHandle::writeVariable). Variable 10 picks the pattern that
// each pass of the loop plays: 0 MGN708_A_OTEHON, 1 _A_RHYTHM_LOOP, 2 _A_KAITOU and 3 _A_INTERVAL_LOOP, and the same
// four for the B set. Variable 9 at 1 moves the A set on to the B set. At 2 it ends the music. SQ_BGM_MGN708_A_FIRST,
// _A_INTERVAL_START, _B_FIRST and _B_INTERVAL_START set both variables as they start. SQ_BGM_MGN708 sets neither,
// and it ends at once unless variable 9 is 0. The module can start SQ_BGM_MGN708 or _A_FIRST as the minigame starts
// (0x6314). An Azahar run of the USA game, with the module at 0x924000, shows the start: the minigame plays
// SQ_BGM_MGN708 and writes 0 to variable 9 right after it (0x73CC). The list gives that start.
//
// Super Mario 3D Land plays a melody as the player collects the notes of a trail. The melody's sequence waits at each
// beat, on every track, until track variable 0 (32 in the commands) changes, unless track variable 2 (34) is 1. The
// melody's object (vtable 0x3D6090) starts the sound with variable 0 at 0 (0x1BEEA0). Each note that the player
// collects adds one to a count of beats to play (0x1EA67C). An update sets variable 0 to 1 for each beat that the count
// holds (0x1BEF24). A note collected while the melody plays between beats starts the sound again at that note's beat,
// with variable 2 at 1 (0x1EA6F8). Variable 2 goes back to 0 once the melody reaches the beat (0x1BEF80). Each write
// goes to the player and to the variable of all 16 tracks (0x1E8E88). The list gives variable 2 at 1. Each melody then
// plays as written. The game plays it that way when the player collects each note in time. With each note, the trail
// also sets the melody's tempo ratio to 16 divided by a value of the trail. The ratio stays between 0.4 and 2
// (0x1710E8 to 0x171114, through 0x1EA6E8 and driver command 0x0F). The list leaves the ratio at 1.
//
// Super Mario 3D Land's BGM player gives each piece of its table (romfs/SoundData/BgmTable.szs, read at 0x1D7EC4) the
// volume of its entry times the line's volume. The player's constructor sets the line's volume to 1 (0x250A98). After
// each start, 0x1E9130 passes the product to the sound's SetVolume (0x1E91A4). The table's sequences are the seven
// note melodies and two gift fanfares. The game's code names none of them, and plays them only through the table. The
// list gives each its entry's volume.
//
// Super Mario 3D Land also starts some effects with a player variable, through 0x278B90 or 0x26CE48. Most of them
// transpose their notes by it. The list gives each the value of its first play:
// - SeSyRedCoin: variable 0 is the count of red coins collected before this one (0x1497D4). The red coin starter
//   writes 0 to it (0x149894). The first coin therefore plays at 0.
// - SeSyRedCoinStarterComplete: the last coin plays with the same count instead (0x149834). Each of the game's 36 sets
//   holds 5 coins. The count is therefore 4.
// - SeSyGetCollectCoin: variable 0 is the number of the stage's three star medals already collected (0x187B04 to
//   0x187B88). The first medal plays at 0.
// - SeSyDmWldInt05MotionCounter: variable 10 is the intro's count divided by 5 below 25 (0x11FCC4 to 0x11FD58). The
//   count starts at 0 (0x11FF30). The sound first plays at a count of 3, with 0.
// - SeSyBeatEnemySequentially: variable 0 is the count of enemies beaten in a row less 2, held between 0 and 6
//   (0x31CA68 to 0x31CAC0). Its first play gives 0. Four other routines start it at 1 (0x1550E0, 0x1610C4, 0x1A710C,
//   0x31CBFC). The list gives 0.
// - SeBmBunbunFireDamage: variable 0 is Boom Boom's count of fire hits (0x1357E0, 0x15806C, 0x168E50). Two of the
//   three helpers' constructors write 0 to it (0x135D10, 0x1585AC). The first hit plays at 0.
// - SeBmPunpunFireDamage: variable 0 is Pom Pom's count of fire hits in each of her two battles (0x163E38, 0x1649CC).
//   The battle states' base constructor writes 0 to it (0x25D514). The first hit plays at 0.
// - SeSyNoteBlockJumpNormalNote and SeSyNoteBlockJumpHighNote: variable 0 is the key of the BGM that plays (0x265EFC,
//   0x336CC4). Four of the game's BGMs have a key. The others get 0 (0x1D8180). The note blocks of world 1-1 and of the
//   title screen play with music that has no key. The first jump therefore plays at 0.
// SeOjRouletteShuffle keeps -1. The roulette gives variable 0 its step, 0 to 2, from 0 (0x1873B0, 0x1874BC). The
// sequence doubles the variable and never reads it. No start changes how it sounds.
// SeSyBeatBlockTrgSub keeps -1. The beat blocks of world 6's course and special 7's course run in the mode that sets
// no variable (0x156330). Only the mode of special 3's course sets it (0x155FD8 to 0x156008). Its first play there
// gives 0.
// SeOjTrickTeresaWarpStepMove keeps -1. The Boos' warp lift starts it through a reaction, without a variable
// (0x34DEF4, 0x27A588).
//
// Mario Kart 7 plays a beat sequence beside the BGM of each course: SEQ_SYNC_BEAT_MUSIC_PARK, _KILLER_SHIP and
// _WALUIGI_PINBALL with their courses, and SEQ_SYNC_BEAT_120 with the others (0x3C8254, and 0x3C80E4 for the final
// lap). The BGM's start reads each sound's BPM from byte 9 of its 3D information in the archive (0x3C9B6C). The beat
// starts once the stream plays (0x3CA0D8). Its tempo ratio is the stream's BPM over the beat's (0x3C9F88). Each beat
// sequence is at 120 BPM. Music Park's stream plays at 139 BPM, and at 166 on the final lap. Killer Ship's plays at
// 148 and 178, and Waluigi Pinball's at 135 and 166. SEQ_SYNC_BEAT_120 takes the BPM of each of the other 29 courses,
// from 112 to 173, and from 136 to 207 on the final lap. The list starts it at Toad Circuit's 140 BPM, the first
// course's. kSyncBeat120States gives Toad Circuit's final lap and both of each other course's tempos, named by the
// stream's label without STRM_CN_ or STRM_CR_ and _N.
//
// Track 15 of each beat plays a drum loop for the player who leads the race. As the race's BGM starts, and again on
// the final lap, it fades the track's level to 0 at once (0x3C7E5C and 0x3C8090, through 0x3C86E0 and 0x3C9D2C). The
// BGM's update sets track 15's volume to the BGM's volume times that level, and mutes the track at 0 (0x3CA2CC,
// through 0x3C9DB4). A race controller sets the level in each frame (0x3D4D38). It's 0 unless the player is in first
// place. Then it rises from 0 at a lead of 150 over second place to 1 at a lead of 370, or from 200 to 1000 in one mode
// (0x3D4F40 to 0x3D5040, the ramp at 0x3D75B4, the parameters at 0x661AF8 from 0x5430F8). The list plays track 15 at
// full level: the leader's beat.
//
// SEQ_SYNC_BEAT_MUSIC_PARK holds an instrument's part on each of tracks 0 to 3. The race's BGM mutes all four as the
// beat starts (0x3C8418). Track 15 plays on. On one of the course's instrument roads, the kart unmutes track 0, 1 or 2
// by the road's instrument (0x3CE1E8 to 0x3CE330). Off the roads, the three tracks are muted again. A controller of
// the course fades track 3 in over 60 frames as the kart enters an area of the course (0x3C6E08, through 0x3D5A8C and
// 0x3D5B94). The controller fades the track out again as the kart leaves the area (0x3C78A8). On the final lap, the
// BGM also sets track variable 0 to 1 on tracks 0 to 2 (0x3C8470 to 0x3C84A0). Tracks 0 and 2 then play a semitone
// higher. No other track reads the variable. The list sets it on every track. The final lap's start mutes track 3 and
// turns the controller off (0x3C817C, through 0x3D5A50). The list gives the start. kMusicParkStates gives the other
// states.
//
// Mario Kart 7 also starts some effects with player variable 0 (0x3D9728 or 0x2728E4). The list gives each a state in
// which the game plays it:
// - SE_KT_FALL_WATER and SE_KT_FALL_LAVA: the player's kart starts them with variable 0 at 1 (0x3CEBBC to 0x3CEC08).
//   At 1, a second track opens. The two tracks play panned left and right. Other karts start them with no variable.
//   One track then plays in the middle.
// - SE_KT_GLIDER_AIR_CURRENT: the player's kart starts it with variable 0 at 1 too (0x3CF060 to 0x3CF07C). At 1,
//   tracks 1 and 2 play panned.
// - SE_KT_GLIDER_FLUTTER: the routine writes 1 to variable 0 in each frame of the sound (0x3CF204). At 1, the sequence
//   plays another part. The routine also sets a tempo ratio by the glider's speed (0x3CF134). The list leaves the
//   ratio at 1.
// - SE_RC_RANK_UP: variable 0 is 7 less the new rank, 0 to 5 (0x3C920C). The list gives 0, the rise from last place to
//   7th. SE_RC_RANK_DOWN keeps -1, the drop to 8th place.
// - The horns (SE_KT_HORN_*): variable 0 is the kart's body, 0 to 16 (0x3CC2A4 to 0x3CC2DC). Each horn takes its
//   program from the variable. The bank holds one horn for each body. At -1, the program command selects program
//   65535. The bank has no such program. The horn then plays nothing. The list gives body 0.
// Mario Kart 7's other effects that read variable 0 keep -1. The game plays each of them at -1, or at a value that
// plays as -1 does. SE_SYS_BTN_OK starts with no variable (0x3C8F60). The coins' effects take the coin count (0x3CDC6C,
// 0x3CDE90, 0x3CDEE8 and 0x3CE5E8). The first coin or the fewest coins play as -1 does. The wooden boards' runs take 0
// and 1 in turn, from 0 (0x3CC484, 0x3CF90C). The menu's slot and list sounds take 1 only below a speed (0x3D0C68,
// 0x3D07BC). Music Park's road runs take the stream's BPM (0x3CE5D0). Until the final lap, that's 139, the tempo of
// their sequences. The Wiggler's shake waits 5 ticks for each segment from the first (0x36FEE0). Nobody has read the
// value of SE_SYS_WIFI_MEMBER_LIST_IN (0x3C991C), the count of an online menu's members.
constexpr SequenceVariable kNewMiiBeat = {.index = 16, .value = 1, .frame = 1};
constexpr SequenceVariable kEveryNoteInTime = {.index = 34, .value = 1};
constexpr SequenceVariable kFirstPlay = {.index = 0, .value = 0};
constexpr SequenceVariable kLastRedCoin = {.index = 0, .value = 4};
constexpr SequenceVariable kIntroFirstCount = {.index = 10, .value = 0};
constexpr SequenceVariable kPlayersKart = {.index = 0, .value = 1};
constexpr SequenceVariable kFinalLapTranspose = {.index = 32, .value = 1};
constexpr SequenceVariable kFirstBody = {.index = 0, .value = 0};
constexpr SequenceVariable kDjSong = {.index = 0, .value = 1};
constexpr SequenceVariable kDjIntro = {.index = 0, .value = 0};
constexpr SequenceVariable kDjInterlude = {.index = 0, .value = 0};
constexpr std::array<float, 2> kClubSend = {0.4f, 0.0f};
constexpr uint8_t kClubEffect = 0;

// A start of one of DJ K.K.'s sequences, with player variable 0 at `variable`.
constexpr SequenceStart DjStart(SequenceVariable variable)
{
    return {.variables = {{variable}}, .fx_send = kClubSend, .game_effect = kClubEffect};
}

// What DJ K.K.'s player writes to track 15's variables 0 to 15 for New Leaf's first default tune (0x8BBCE2) in key 0. A
// Unicorn run of 0x28B08C gives the values.
constexpr std::array<int16_t, 16> kFirstDefaultTune = {12, 10000, 7, 4,      5, 5,     5,     10000,
                                                       7,  7,     5, -10000, 0, 10000, 10000, 10000};

// A start of one of DJ K.K.'s interludes that play the town's tune: player variable 0 at 0, and the first default tune
// in track variables 0 to 15 (indices 32 to 47).
constexpr SequenceStart DjTuneStart()
{
    SequenceStart start = DjStart(kDjInterlude);
    for (std::size_t step = 0; step < kFirstDefaultTune.size(); ++step)
    {
        start.variables[step + 1] = {.index = static_cast<uint8_t>(32 + step), .value = kFirstDefaultTune[step]};
    }

    return start;
}

constexpr SequenceVariable kTuneNoteC = {.index = 14, .value = 3};
constexpr SequenceVariable kTrainTuneFirstNote = {.index = 14, .value = 9};
constexpr SequenceVariable kFirstChord = {.index = 26, .value = 0};
constexpr SequenceVariable kRunCountdown = {.index = 0, .value = 928};

// Mario Kart 7's beat sequences' tempo ratios: each stream's BPM over the beat's 120.
constexpr float kMusicParkTempo = 139.0f / 120.0f;
constexpr float kMusicParkFinalLapTempo = 166.0f / 120.0f;
constexpr float kKillerShipTempo = 148.0f / 120.0f;
constexpr float kKillerShipFinalLapTempo = 178.0f / 120.0f;
constexpr float kWaluigiPinballTempo = 135.0f / 120.0f;
constexpr float kWaluigiPinballFinalLapTempo = 166.0f / 120.0f;

constexpr std::array<StartingSequence, 121> kStartingSequences = {{
    {kOcarinaOfTime3D, "NA_BGM_RONRON", {.silenced = 0x2000}},
    {kIslandTour, "SEQ_BGM_MENU", {.muted = 0xfff0}},
    {kWarioWareGold, "SEQ_M_TSUNAGI_TRICKY_NEXT1", {.silenced = 0x0fff}},
    {kWarioWareGold, "SEQ_M_TSUNAGI_TRICKY_NG1", {.silenced = 0x0fff}},
    {kWarioWareGold, "SEQ_M_TSUNAGI_TRICKY_OK1", {.silenced = 0x0fff}},
    {kNewLeaf, "SEQ_BGM_IND_MUSEUM", {.muted = 0xe3f0}},
    {kNewLeaf, "SEQ_BGM_IND_CAFEARBEIT", {.muted = 0x0080}},
    {kNewLeaf, "SEQ_BGM_DJ", DjStart(kDjSong), true},
    {kNewLeaf, "SEQ_BGM_DJ91_HS_BIRTHDAYSONG", DjStart(kDjIntro)},
    {kNewLeaf, "SEQ_BGM_DJ91_FC_BIRTHDAYSONG", DjStart(kDjIntro)},
    {kNewLeaf, "SEQ_BGM_DJ95_HS_EVENT", DjStart(kDjIntro)},
    {kNewLeaf, "SEQ_BGM_DJ95_FC_EVENT", DjStart(kDjIntro)},
    {kNewLeaf, "SEQ_BGM_DJ_HS_HANIWA", DjStart(kDjInterlude)},
    {kNewLeaf, "SEQ_BGM_DJ_FC_HANIWA", DjStart(kDjInterlude)},
    {kNewLeaf, "SEQ_BGM_DJ_HS_MURAMELO", DjTuneStart()},
    {kNewLeaf, "SEQ_BGM_DJ_FC_MURAMELO", DjTuneStart()},
    {kTomodachiLife, "BGM_MANSION_AND_ROOM", {.muted = 0xfff8}},
    {kTomodachiLife, "BGM_AKICHI_NIGHT", {.muted = 0xff0f}},
    {kTomodachiLife, "SE_MII_PLAY_GUITAR_S", {.variables = {{kFirstChord}}}},
    {kTomodachiLife, "SE_MII_PLAY_GUITAR_M", {.variables = {{kFirstChord}}}},
    {kTomodachiLife, "SE_MII_PLAY_GUITAR_L", {.variables = {{kFirstChord}}}},
    {kNewLeaf, "SE_TV_12_RUN", {.variables = {{kRunCountdown}}}},
    {kHappyHomeDesigner, "SE_TV_12_RUN", {.variables = {{kRunCountdown}}}},
    {kHappyHomeDesignerOld, "SE_TV_12_RUN", {.variables = {{kRunCountdown}}}},
    {kUltimateAngler, "SEQ_BGM_SPOT_00_A", {}},
    {kUltimateAngler, "SEQ_BGM_CROSS", {.muted = 0x8000, .silenced = 0x3c00}},
    {kUltimateAngler, "SEQ_BGM_REELING_NORMAL", {.muted = 0x8000, .silenced = 0x0020}},
    {kUltimateAngler, "SEQ_BGM_REELING_BIG", {.muted = 0x8000, .silenced = 0x0100}},
    {kUltimateAngler, "SEQ_BGM_REELING_KAIGYO", {.muted = 0x8000, .silenced = 0x0040}},
    {kUltimateAngler, "SEQ_BGM_REELING_LASTBOSS", {.muted = 0x8000, .silenced = 0x1000}},
    {kUltimateAngler, "SEQ_BGM_ED2_DEMO", {.muted = 0xbffc}},
    {kUltimateAngler, "SE_ENV_HOME_CROSS", {.muted = 0xfffc}},
    {kUltimateAngler, "SEQ_BGM_", {.muted = 0x8000}, true},
    {kUltimateAngler, "SE_ANIM_CRUISER_JUMP_IN", {.variables = {{{.index = 31, .value = 0}}}}},
    {kUltimateAngler, "SE_ANIM_CRUISER_JUMP_IN_MY_MII", {.variables = {{{.index = 31, .value = 0}}}}},
    {kUltimateAngler, "SE_CATCHED_092", {.seed = 0x8d9333a4}},
    {kUltimateAngler, "SE_CATCHED_120", {.seed = 0x995e6351}},
    {kUltimateAngler, "SE_FEVER_PULL_2", {.variables = {{{.index = 0, .value = 0}}}}},
    {kUltimateAngler, "SE_MIIS_VOICE", {.variables = {{{.index = 0, .value = 0x1f}}}}},
    {kUltimateAngler, "SE_MIIS_VOICE_HARD", {.variables = {{{.index = 0, .value = 0x1f}}}}},
    {kUltimateAngler, "SE_MIIS_VOICE_START", {.variables = {{{.index = 0, .value = 0x1f}}}}},
    {kUltimateAngler, "SE_MIIS_VOICE_START_FEVER", {.variables = {{{.index = 0, .value = 0x1f}}}}},
    {kFlowerTown, "BGM_FLOWER_ATELIER", {.silenced = 0xffc0}},
    {kFlowerTown, "SE_COLOR_WHEEL_LIGHT_2", {.variables = {{{.index = 19, .value = 0}}}}},
    {kMiiPlaza, "SEQ_USAGI", {.muted = 0xf102}},
    {kMiiPlaza, "BGM_MAIN_Mii_Only_One_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlaza, "BGM_MAIN_Mii_MIN_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlaza, "BGM_MAIN_Mii_MID_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlaza, "BGM_MAIN_Mii_MAX_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlaza, "BGM_MAIN_Mii_No5_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlaza, "BGM_MAIN_Mii_No6_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlaza, "BGM_MAIN_Mii_No7_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_Only_One_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_MIN_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_MID_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_MAX_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_No5_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_No6_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kMiiPlazaSystemTitle, "BGM_MAIN_Mii_No7_From_New_Mii", {.variables = {{kNewMiiBeat}}}},
    {kSystemSettings, "BGM_CTR_MSET", {.muted = 0xff80}},
    {kSystemSettings, "BGM_CTR_MSET_NET", {.muted = 0x007f}},
    {kSystemSettings, "BGM_CTR_MSET_FIRST", {.muted = 0xfff7}},
    {kSystemSettings2, "BGM_CTR_MSET", {.muted = 0xff80}},
    {kSystemSettings2, "BGM_CTR_MSET_NET", {.muted = 0x007f}},
    {kSystemSettings2, "BGM_CTR_MSET_FIRST", {.muted = 0xfff7}},
    {kActivityLog, "BGM_CTR_PLOG_MAIN", {.muted = 0xfc00}},
    {kActivityLog, "BGM_CTR_PLOG_MAIN_CLOCK", {.muted = 0xf000}},
    {kActivityLog, "BGM_CTR_PLOG_MAIN_STEP", {.muted = 0xcc00}},
    {kMiiMaker, "MEDIT_BGM_FACE", {.silenced = 0xff00}},
    {kMarioPartyStarRush, "SQ_BGM_MGN708", {.variables = {{{.index = 9, .value = 0}}}}},
    {kIslandTour, "SEQ_BGM_MNG_012", {.muted = 0xbff0, .variables = {{{.index = 0, .value = 96}}}}},
    {kIslandTour, "SEQ_BGM_MNG_MG3801", {.muted = 0x98b4, .variables = {{{.index = 0, .value = 100}}}}},
    {kIslandTour, "SEQ_BGM_MNG_MG3802", {.variables = {{{.index = 0, .value = 99}}}}},
    {kIslandTour, "SEQ_BGM_BOSS_04_A", {.variables = {{{.index = 0, .value = 20}}}}},
    {kIslandTour, "SEQ_BGM_BOSS_04_B", {.variables = {{{.index = 0, .value = 20}}}}},
    {kIslandTour, "SEQ_BGM_BOSS_04_B2", {.variables = {{{.index = 0, .value = 20}}}}},
    {kIslandTour, "SEQ_BGM_BOSS_04_B3", {.variables = {{{.index = 0, .value = 20}}}}},
    {kSuperMario3DLand, "SS_NOTE_OBJ_CHIKA", {.variables = {{kEveryNoteInTime}}, .volume = 0.55f}},
    {kSuperMario3DLand, "SS_NOTE_OBJ_SEA", {.variables = {{kEveryNoteInTime}}, .volume = 0.6f}},
    {kSuperMario3DLand, "SS_NOTE_OBJ_MWORLD", {.variables = {{kEveryNoteInTime}}, .volume = 0.55f}},
    {kSuperMario3DLand, "SS_NOTE_OBJ_M3INTRO", {.variables = {{kEveryNoteInTime}}, .volume = 0.55f}},
    {kSuperMario3DLand, "SS_NOTE_OBJ_MWORLD2", {.variables = {{kEveryNoteInTime}}, .volume = 0.55f}},
    {kSuperMario3DLand, "SsNoteObjTest", {.variables = {{kEveryNoteInTime}}, .volume = 0.55f}},
    {kSuperMario3DLand, "SsNoteObjTestLong", {.variables = {{kEveryNoteInTime}}, .volume = 0.7f}},
    {kSuperMario3DLand, "SS_KINO_GIFT_SILVER", {.volume = 0.5f}},
    {kSuperMario3DLand, "SS_KINO_GIFT_GOLD", {.volume = 0.4f}},
    {kSuperMario3DLand, "SeSyRedCoin", {.variables = {{kFirstPlay}}}},
    {kSuperMario3DLand, "SeSyRedCoinStarterComplete", {.variables = {{kLastRedCoin}}}},
    {kSuperMario3DLand, "SeSyGetCollectCoin", {.variables = {{kFirstPlay}}}},
    {kSuperMario3DLand, "SeSyDmWldInt05MotionCounter", {.variables = {{kIntroFirstCount}}}},
    {kSuperMario3DLand, "SeSyBeatEnemySequentially", {.variables = {{kFirstPlay}}}},
    {kSuperMario3DLand, "SeBmBunbunFireDamage", {.variables = {{kFirstPlay}}}},
    {kSuperMario3DLand, "SeBmPunpunFireDamage", {.variables = {{kFirstPlay}}}},
    {kSuperMario3DLand, "SeSyNoteBlockJumpNormalNote", {.variables = {{kFirstPlay}}}},
    {kSuperMario3DLand, "SeSyNoteBlockJumpHighNote", {.variables = {{kFirstPlay}}}},
    {kMarioKart7, "SEQ_SYNC_BEAT_MUSIC_PARK", {.muted = 0x000f, .tempo_ratio = kMusicParkTempo}},
    {kMarioKart7, "SEQ_SYNC_BEAT_KILLER_SHIP", {.tempo_ratio = kKillerShipTempo}},
    {kMarioKart7, "SEQ_SYNC_BEAT_WALUIGI_PINBALL", {.tempo_ratio = kWaluigiPinballTempo}},
    {kMarioKart7, "SEQ_SYNC_BEAT_120", {.tempo_ratio = 140.0f / 120.0f}},
    {kMarioKart7, "SE_KT_FALL_WATER", {.variables = {{kPlayersKart}}}},
    {kMarioKart7, "SE_KT_FALL_LAVA", {.variables = {{kPlayersKart}}}},
    {kMarioKart7, "SE_KT_GLIDER_AIR_CURRENT", {.variables = {{kPlayersKart}}}},
    {kMarioKart7, "SE_KT_GLIDER_FLUTTER", {.variables = {{{.index = 0, .value = 1}}}}},
    {kMarioKart7, "SE_RC_RANK_UP", {.variables = {{{.index = 0, .value = 0}}}}},
    {kMarioKart7, "SE_KT_HORN_KOOPA", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_DAISY", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_DONKEY", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_HONEY_QUEEN", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_NOKONOKO", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_JUGEM", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_LUIGI", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_MARIO", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_METALMARIO", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_MII", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_PEACH", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_ROSETTA", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_HEIHO", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_KINOPIO", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_WARIO", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_HANACHAN", {.variables = {{kFirstBody}}}},
    {kMarioKart7, "SE_KT_HORN_YOSHI", {.variables = {{kFirstBody}}}},
}};

struct ArrangedSequence
{
    std::string_view archive_sha1;
    std::string_view sound;
    std::span<const SequenceArrangement> arrangements;
};

// The later states of some listed sequences, in the order in which each game plays them.
//
// Each of Ultimate Angler's reeling pieces plays its layer at tension level 2 and up (0xD508D8).
constexpr std::array<SequenceArrangement, 1> kReelingAtTension2 = {{
    {"_tension_2", {.muted = 0x8000}},
}};

// The ending's cutscene moves SEQ_BGM_ED2_DEMO through four more sets of tracks (0xD5195C, through 0xD3BEB0). Each
// change unmutes every track (0xD51A28), then fades the set's tracks in and the other tracks out over 150 frames. The
// cutscene's states run in the order 4, 5, 2, 6, 3, 7, 8, 9, 10, 11 and 12. The steps come at the entry of state 8
// (tracks 0 to 3, 0x7DC218), in state 9 once the dialog reaches its second line (tracks 0 to 3, 5 and 6, 0x7DE3D8), at
// frame 98 of state 10's camera animation (tracks 0 to 8, 0x7DCCF0) and at frame 262 of state 12's (tracks 7 to 10,
// 0x7DCED4).
constexpr std::array<SequenceArrangement, 4> kEndingSteps = {{
    {"_step_1", {.muted = 0x8000, .silenced = 0x7ff0}},
    {"_step_2", {.muted = 0x8000, .silenced = 0x7f90}},
    {"_step_3", {.muted = 0x8000, .silenced = 0x7e00}},
    {"_step_4", {.muted = 0x8000, .silenced = 0x787f}},
}};

// SE_ENV_HOME_CROSS plays one of eight groups of tracks for each of 10 rows and 7 columns of a table (0xEE4DF8).
// 0xD51CAC mutes the other tracks with release, with no fade. Groups 0 and 7 play tracks 0 and 1. Groups 1 to 5 play
// tracks 2 to 4, 5 to 7, 8 to 10, 11 and 12, and 13 to 15. Group 6 plays track 0. The list gives each other group,
// named by its tracks. Group 1's mute leaves tracks 3 to 5 as they are before it unmutes tracks 2 to 4 (0xD51D84).
// After the start, which mutes tracks 2 to 15, group 1 plays tracks 2 to 4.
constexpr std::array<SequenceArrangement, 6> kHomeAmbienceGroups = {{
    {"_tracks_2_to_4", {.muted = 0xffe3}},
    {"_tracks_5_to_7", {.muted = 0xff1f}},
    {"_tracks_8_to_10", {.muted = 0xf8ff}},
    {"_tracks_11_and_12", {.muted = 0xe7ff}},
    {"_tracks_13_to_15", {.muted = 0x1fff}},
    {"_track_0", {.muted = 0xfffe}},
}};

// SEQ_USAGI's other four states, in the order of the Plaza's table (0x10A26A8). The Plaza sets "SILENCE" at 0x2649E0.
// INTRO plays tracks 0, 2 to 7, 9 and 11. SILENCE plays tracks 9 to 11. TOHOHO plays tracks 0 and 8 to 11. HAPPY plays
// every track but 8.
constexpr std::array<SequenceArrangement, 4> kUsagiStates = {{
    {"_intro", {.muted = 0xf502}},
    {"_silence", {.muted = 0xf1ff}},
    {"_tohoho", {.muted = 0xf0fe}},
    {"_happy", {.muted = 0x0100}},
}};

// The start of a jump-in into Ultimate Angler's cruiser, with global variable 31 at `count`.
constexpr SequenceStart JumpIn(int16_t count)
{
    return {.variables = {{{.index = 31, .value = count}}}};
}

// The other nine values of the count, for the second jump-in to the tenth.
constexpr std::array<SequenceArrangement, 9> kLaterJumpIns = {{
    {"_jump_2", JumpIn(1)},
    {"_jump_3", JumpIn(2)},
    {"_jump_4", JumpIn(3)},
    {"_jump_5", JumpIn(4)},
    {"_jump_6", JumpIn(5)},
    {"_jump_7", JumpIn(6)},
    {"_jump_8", JumpIn(7)},
    {"_jump_9", JumpIn(8)},
    {"_jump_10", JumpIn(9)},
}};

// The start of Ultimate Angler's SE_FEVER_PULL_2 with the pull count `count` in variable 0.
constexpr SequenceStart Pull(int16_t count)
{
    return {.variables = {{{.index = 0, .value = count}}}};
}

// The other 20 values of the pull count, for the second pull of a fever to the 21st and later ones.
// clang-format off
constexpr std::array<SequenceArrangement, 20> kLaterPulls = {{
    {"_pull_2", Pull(1)},
    {"_pull_3", Pull(2)},
    {"_pull_4", Pull(3)},
    {"_pull_5", Pull(4)},
    {"_pull_6", Pull(5)},
    {"_pull_7", Pull(6)},
    {"_pull_8", Pull(7)},
    {"_pull_9", Pull(8)},
    {"_pull_10", Pull(9)},
    {"_pull_11", Pull(10)},
    {"_pull_12", Pull(11)},
    {"_pull_13", Pull(12)},
    {"_pull_14", Pull(13)},
    {"_pull_15", Pull(14)},
    {"_pull_16", Pull(15)},
    {"_pull_17", Pull(16)},
    {"_pull_18", Pull(17)},
    {"_pull_19", Pull(18)},
    {"_pull_20", Pull(19)},
    {"_pull_21", Pull(20)},
}};
// clang-format on

// The start of one of Ultimate Angler's four SE_MIIS_VOICE effects with the crowd's state `state` in variable 0.
constexpr SequenceStart Crowd(int16_t state)
{
    return {.variables = {{{.index = 0, .value = state}}}};
}

// The other 26 states of the crowd that 0xD50E00 can give, by kind A's level and kind B's or kind C's.
// clang-format off
constexpr std::array<SequenceArrangement, 26> kOtherCrowds = {{
    {"_crowd_a0_b1", Crowd(0x04)},
    {"_crowd_a0_b2", Crowd(0x08)},
    {"_crowd_a0_b3", Crowd(0x0c)},
    {"_crowd_a0_c1", Crowd(0x14)},
    {"_crowd_a0_c2", Crowd(0x18)},
    {"_crowd_a0_c3", Crowd(0x1c)},
    {"_crowd_a1", Crowd(0x01)},
    {"_crowd_a1_b1", Crowd(0x05)},
    {"_crowd_a1_b2", Crowd(0x09)},
    {"_crowd_a1_b3", Crowd(0x0d)},
    {"_crowd_a1_c1", Crowd(0x15)},
    {"_crowd_a1_c2", Crowd(0x19)},
    {"_crowd_a1_c3", Crowd(0x1d)},
    {"_crowd_a2", Crowd(0x02)},
    {"_crowd_a2_b1", Crowd(0x06)},
    {"_crowd_a2_b2", Crowd(0x0a)},
    {"_crowd_a2_b3", Crowd(0x0e)},
    {"_crowd_a2_c1", Crowd(0x16)},
    {"_crowd_a2_c2", Crowd(0x1a)},
    {"_crowd_a2_c3", Crowd(0x1e)},
    {"_crowd_a3", Crowd(0x03)},
    {"_crowd_a3_b1", Crowd(0x07)},
    {"_crowd_a3_b2", Crowd(0x0b)},
    {"_crowd_a3_b3", Crowd(0x0f)},
    {"_crowd_a3_c1", Crowd(0x17)},
    {"_crowd_a3_c2", Crowd(0x1b)},
}};
// clang-format on

// SE_COLOR_WHEEL_LIGHT_2's second note, at variable 19's value 1.
constexpr std::array<SequenceArrangement, 1> kSecondLight = {{
    {"_second", {.variables = {{{.index = 19, .value = 1}}}}},
}};

// Ninja Launcher's battle plays its music at a tempo ratio of 1.3 once the battle's timer falls below 10 seconds, and
// at 1 before (0x857810, on the BGM of 0x850F3C). The timer counts down to the battle's end (0x856F38).
constexpr std::array<SequenceArrangement, 1> kHurryUp = {{
    {"_hurry", {.tempo_ratio = 1.3f}},
}};

// The Plaza's quick mode, a setting in bit 0x400 of the Plaza's save (0x4517C0). In it, the gate plays
// BGM_NEW_Mii_Long at a speed of 1.25 (0x1C3770 and 0x1B719C), and the BGM manager plays the Plaza's music at 1.25
// (0x3A16E4, called at 0x3B6AA4 once 0x4517C0 holds, and in seven more scenes). A speed v gives the sound a pitch of v
// and a tempo ratio of 1 + 0.65 (v - 1), 1.1625 at 1.25 (0x1B6F2C and 0x3A1DEC). The BGM manager's pieces are those of
// its table at 0xEFD01C. The quick button's speed of 1.667 lasts only while the player holds the button to skip ahead
// (0x47CBD8), and has no arrangement.
constexpr std::array<SequenceArrangement, 1> kQuickMode = {{
    {"_quick", {.tempo_ratio = 1.1625f, .pitch = 1.25f}},
}};

// A From_New_Mii piece in quick mode keeps its change of global variable 16.
constexpr std::array<SequenceArrangement, 1> kQuickModeAfterJingle = {{
    {"_quick", {.variables = {{kNewMiiBeat}}, .tempo_ratio = 1.1625f, .pitch = 1.25f}},
}};

// The other layers of Island Tour's SEQ_BGM_MENU: rows 26 to 28 of the game's table rom:/ts_sound_ctrl_track.bin, which
// 0x272D00 picks from the list at 0x3D5274. Game select and party select play tracks 0 to 6 (0x165A90 and 0x168C58),
// character select tracks 0 to 9 (0x178884), and the rule entrance and minigame select all 16 tracks (0x16F3A4 and
// 0x174658). The game's fader takes the other tracks to a volume of 0 and then mutes them (0x18EE04).
constexpr std::array<SequenceArrangement, 3> kIslandTourMenuLayers = {{
    {"_game_select", {.muted = 0xff80}},
    {"_character_select", {.muted = 0xfc00}},
    {"_minigame_select", {}},
}};

// Island Tour's tempo pieces with 10 seconds left. The game's value falls to 0 as the time runs out, and a rip holds
// one value. MG3801 also plays row 11 of the track table from 20 seconds left.
constexpr std::array<SequenceArrangement, 1> kTenSecondsLeft = {{
    {"_hurry", {.variables = {{{.index = 0, .value = 10}}}}},
}};
constexpr std::array<SequenceArrangement, 1> kMg3801TenSecondsLeft = {{
    {"_hurry", {.muted = 0x654a, .variables = {{{.index = 0, .value = 10}}}}},
}};

// SEQ_BGM_MNG_012 at the ball's full speed with rows 8 and 9 of the track table.
constexpr std::array<SequenceArrangement, 2> kMng012Layers = {{
    {"_second", {.muted = 0xbe0f, .variables = {{{.index = 0, .value = 96}}}}},
    {"_third", {.muted = 0x01ff, .variables = {{{.index = 0, .value = 96}}}}},
}};

// Minigame 708's music with variable 10 at `pattern` in the A set. The module writes variable 10 as the minigame goes
// on: 0 at 0x1FCC, 1 at 0x343C, 2 at 0x15D8, 0x1654 and 0x1D84, and 3 at 0x6CC4 and 0x7224. In the Azahar run, the
// minigame plays the A set's four patterns on SQ_BGM_MGN708. It leaves variable 10 unwritten for the first 12
// seconds. It then writes 3 (0x6CC4), 0 (0x1FCC, from 15 s), 1 (0x343C, 42 s), 2 (0x1D84, 50 s), 1, 3 (0x7224, 66 s)
// and 0 again. SQ_BGM_MGN708 sets neither variable. Its patterns take variable 10 from the first tick, with variable 9
// at 0.
constexpr SequenceStart Mgn708APattern(int16_t pattern)
{
    return {.variables = {{{.index = 9, .value = 0}, {.index = 10, .value = pattern}}}};
}

// Minigame 708's music with variable 10 at `pattern` in the B set, from frame 1. SQ_BGM_MGN708_B_FIRST sets the
// variable to 3 at the first tick. The loop's first pass comes after frame 1 and plays the pattern. The module moves
// the music on to the B set with variable 9 at 1 (0x135C). A patched Azahar run set r2 to 1 at the module's check at
// 0x6BD8 in the second round. The module then wrote variable 9 = 1 for 7 seconds. Each later round wrote variable 10 =
// 3, 0, 1, 2 and 1, as in the A set, and the B set's four patterns played at those times.
constexpr SequenceStart Mgn708BPattern(int16_t pattern)
{
    return {.variables = {{{.index = 10, .value = pattern, .frame = 1}}}};
}

// The patterns in the order in which the run plays them: 3, 0, 1 and 2. SQ_BGM_MGN708_A_FIRST starts with pattern 0,
// and _B_FIRST with pattern 3.
constexpr std::array<SequenceArrangement, 4> kMgn708APatterns = {{
    {"_interval_loop", Mgn708APattern(3)},
    {"_otehon", Mgn708APattern(0)},
    {"_rhythm_loop", Mgn708APattern(1)},
    {"_kaitou", Mgn708APattern(2)},
}};

constexpr std::array<SequenceArrangement, 3> kMgn708BPatterns = {{
    {"_otehon", Mgn708BPattern(0)},
    {"_rhythm_loop", Mgn708BPattern(1)},
    {"_kaitou", Mgn708BPattern(2)},
}};

// Mii Force's SD_BGM_D01 tests player variable 0 every two beats, and its loop never ends by itself. At a value of 1,
// each track jumps to the piece's ending. The ending takes the key of the part that the piece has reached: one key from
// the 2nd test to the 9th, the other from the 10th to the first test of the next pass. Four of the game's demo scripts
// play the piece, and one changes the variable. The game-over demo sets it to 1 once the boss's last line has gone
// (demo1001.txt, command SetSndSeqGlobalVal, through 0x917C40, 0x91BB6C and 0x80ED88). The time depends on the player's
// buttons. _ending changes the variable just before the 17th test: one pass, then the ending. _ending_early changes it
// just before the 9th, for the other key.
constexpr std::array<SequenceArrangement, 2> kGameOverEndings = {{
    {"_ending", {.variables = {{{.index = 0, .value = 1, .frame = 4400}}}}},
    {"_ending_early", {.variables = {{{.index = 0, .value = 1, .frame = 2200}}}}},
}};

// SEQ_SYNC_BEAT_MUSIC_PARK's other states: each instrument road's track, the area's track 3, and the final lap's
// start and roads.
constexpr std::array<SequenceArrangement, 8> kMusicParkStates = {{
    {"_track_0", {.muted = 0x000e, .tempo_ratio = kMusicParkTempo}},
    {"_track_1", {.muted = 0x000d, .tempo_ratio = kMusicParkTempo}},
    {"_track_2", {.muted = 0x000b, .tempo_ratio = kMusicParkTempo}},
    {"_track_3", {.muted = 0x0007, .tempo_ratio = kMusicParkTempo}},
    {"_final_lap", {.muted = 0x000f, .variables = {{kFinalLapTranspose}}, .tempo_ratio = kMusicParkFinalLapTempo}},
    {"_final_lap_track_0",
     {.muted = 0x000e, .variables = {{kFinalLapTranspose}}, .tempo_ratio = kMusicParkFinalLapTempo}},
    {"_final_lap_track_1",
     {.muted = 0x000d, .variables = {{kFinalLapTranspose}}, .tempo_ratio = kMusicParkFinalLapTempo}},
    {"_final_lap_track_2",
     {.muted = 0x000b, .variables = {{kFinalLapTranspose}}, .tempo_ratio = kMusicParkFinalLapTempo}},
}};

// SEQ_BGM_IND_CAFEARBEIT after the café's fade: every track.
constexpr std::array<SequenceArrangement, 1> kCafeTrack7 = {{
    {"_track_7", {}},
}};

// Tomodachi Life's other sets of tracks of the apartment's music, named by the tracks that play.
constexpr std::array<SequenceArrangement, 5> kApartmentStates = {{
    {"_tracks_3_4_10", {.muted = 0xfbe7}},
    {"_tracks_0_6_12", {.muted = 0xefbe}},
    {"_tracks_5_9_13", {.muted = 0xdddf}},
    {"_track_15", {.muted = 0x7fff}},
    {"_tracks_0_8_14", {.muted = 0xbefe}},
}};
constexpr std::array<SequenceArrangement, 1> kNightApartmentStates = {{
    {"_tracks_8_to_11", {.muted = 0xf0ff}},
}};

// MEDIT_BGM_FACE's later mixes in Mii Maker: the sound at 0.7, tracks 3, 4 and 7 out, tracks 8 and 9 at 0.8, and in
// the second, track 10 at 0.75.
constexpr std::array<float, kSequenceTracks> kMiiMakerTracks8And9 = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                                     0.8f, 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr std::array<float, kSequenceTracks> kMiiMakerTracks8To10 = {1.0f, 1.0f, 1.0f,  1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                                     0.8f, 0.8f, 0.75f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr std::array<SequenceArrangement, 2> kMiiMakerMixes = {{
    {"_tracks_8_9", {.silenced = 0xfc98, .track_volumes = kMiiMakerTracks8And9, .volume = 0.7f}},
    {"_tracks_8_to_10", {.silenced = 0xf898, .track_volumes = kMiiMakerTracks8To10, .volume = 0.7f}},
}};

// The final laps of Killer Ship and Waluigi Pinball.
constexpr std::array<SequenceArrangement, 1> kKillerShipFinalLap = {{
    {"_final_lap", {.tempo_ratio = kKillerShipFinalLapTempo}},
}};
constexpr std::array<SequenceArrangement, 1> kWaluigiPinballFinalLap = {{
    {"_final_lap", {.tempo_ratio = kWaluigiPinballFinalLapTempo}},
}};

// SEQ_SYNC_BEAT_120's other states: Toad Circuit's final lap, then each other course and its final lap, at the stream's
// BPM over 120 (the BPMs in the archive's 3D information).
constexpr std::array<SequenceArrangement, 57> kSyncBeat120States = {{
    {"_toad_circuit_final_lap", {.tempo_ratio = 168.0f / 120.0f}},
    {"_glide_lake", {.tempo_ratio = 140.0f / 120.0f}},
    {"_glide_lake_final_lap", {.tempo_ratio = 155.0f / 120.0f}},
    {"_marine_road", {.tempo_ratio = 130.0f / 120.0f}},
    {"_marine_road_final_lap", {.tempo_ratio = 156.0f / 120.0f}},
    {"_sand_town", {.tempo_ratio = 112.0f / 120.0f}},
    {"_sand_town_final_lap", {.tempo_ratio = 136.0f / 120.0f}},
    {"_wufu_island1", {.tempo_ratio = 125.0f / 120.0f}},
    {"_wufu_island1_final_lap", {.tempo_ratio = 150.0f / 120.0f}},
    {"_mario_circuit", {.tempo_ratio = 140.0f / 120.0f}},
    {"_mario_circuit_final_lap", {.tempo_ratio = 168.0f / 120.0f}},
    {"_rally_cource", {.tempo_ratio = 173.0f / 120.0f}},
    {"_rally_cource_final_lap", {.tempo_ratio = 207.0f / 120.0f}},
    {"_underground", {.tempo_ratio = 139.0f / 120.0f}},
    {"_underground_final_lap", {.tempo_ratio = 153.0f / 120.0f}},
    {"_advanced_circuit", {.tempo_ratio = 142.0f / 120.0f}},
    {"_advanced_circuit_final_lap", {.tempo_ratio = 168.0f / 120.0f}},
    {"_wufu_island2", {.tempo_ratio = 125.0f / 120.0f}},
    {"_wufu_island2_final_lap", {.tempo_ratio = 150.0f / 120.0f}},
    {"_dk_jungle", {.tempo_ratio = 137.0f / 120.0f}},
    {"_dk_jungle_final_lap", {.tempo_ratio = 153.0f / 120.0f}},
    {"_ice_slider", {.tempo_ratio = 145.0f / 120.0f}},
    {"_ice_slider_final_lap", {.tempo_ratio = 166.0f / 120.0f}},
    {"_koopa_castle", {.tempo_ratio = 136.0f / 120.0f}},
    {"_koopa_castle_final_lap", {.tempo_ratio = 163.0f / 120.0f}},
    {"_rainbow_road", {.tempo_ratio = 136.0f / 120.0f}},
    {"_rainbow_road_final_lap", {.tempo_ratio = 168.0f / 120.0f}},
    {"_wario_ship", {.tempo_ratio = 152.0f / 120.0f}},
    {"_wario_ship_final_lap", {.tempo_ratio = 174.0f / 120.0f}},
    {"_n64_luigi_circuit", {.tempo_ratio = 140.0f / 120.0f}},
    {"_n64_luigi_circuit_final_lap", {.tempo_ratio = 168.0f / 120.0f}},
    {"_agb_koopa_castle", {.tempo_ratio = 170.0f / 120.0f}},
    {"_agb_koopa_castle_final_lap", {.tempo_ratio = 190.0f / 120.0f}},
    {"_wii_kinoko_canyon", {.tempo_ratio = 133.0f / 120.0f}},
    {"_wii_kinoko_canyon_final_lap", {.tempo_ratio = 150.0f / 120.0f}},
    {"_ds_luigi_mansion", {.tempo_ratio = 122.0f / 120.0f}},
    {"_ds_luigi_mansion_final_lap", {.tempo_ratio = 155.0f / 120.0f}},
    {"_n64_nokonoko_beach", {.tempo_ratio = 118.0f / 120.0f}},
    {"_n64_nokonoko_beach_final_lap", {.tempo_ratio = 141.0f / 120.0f}},
    {"_sfc_mario_circuit2", {.tempo_ratio = 133.0f / 120.0f}},
    {"_sfc_mario_circuit2_final_lap", {.tempo_ratio = 160.0f / 120.0f}},
    {"_wii_coconuts_mall", {.tempo_ratio = 132.0f / 120.0f}},
    {"_wii_coconuts_mall_final_lap", {.tempo_ratio = 158.0f / 120.0f}},
    {"_n64_karakara_desert", {.tempo_ratio = 128.0f / 120.0f}},
    {"_n64_karakara_desert_final_lap", {.tempo_ratio = 154.0f / 120.0f}},
    {"_ds_dk_snow_mountain", {.tempo_ratio = 119.0f / 120.0f}},
    {"_ds_dk_snow_mountain_final_lap", {.tempo_ratio = 153.0f / 120.0f}},
    {"_gc_daisy_cruiser", {.tempo_ratio = 125.0f / 120.0f}},
    {"_gc_daisy_cruiser_final_lap", {.tempo_ratio = 155.0f / 120.0f}},
    {"_wii_maple_tree_house", {.tempo_ratio = 148.0f / 120.0f}},
    {"_wii_maple_tree_house_final_lap", {.tempo_ratio = 200.0f / 120.0f}},
    {"_wii_nokonoko_cape", {.tempo_ratio = 160.0f / 120.0f}},
    {"_wii_nokonoko_cape_final_lap", {.tempo_ratio = 192.0f / 120.0f}},
    {"_gc_dinodino_jungle", {.tempo_ratio = 137.0f / 120.0f}},
    {"_gc_dinodino_jungle_final_lap", {.tempo_ratio = 157.0f / 120.0f}},
    {"_sfc_rainbow_road", {.tempo_ratio = 145.0f / 120.0f}},
    {"_sfc_rainbow_road_final_lap", {.tempo_ratio = 174.0f / 120.0f}},
}};

constexpr std::array<ArrangedSequence, 58> kArrangedSequences = {{
    {kTomodachiLife, "BGM_MANSION_AND_ROOM", kApartmentStates},
    {kTomodachiLife, "BGM_AKICHI_NIGHT", kNightApartmentStates},
    {kNewLeaf, "SEQ_BGM_IND_CAFEARBEIT", kCafeTrack7},
    {kUltimateAngler, "SEQ_BGM_REELING_NORMAL", kReelingAtTension2},
    {kUltimateAngler, "SEQ_BGM_REELING_BIG", kReelingAtTension2},
    {kUltimateAngler, "SEQ_BGM_REELING_KAIGYO", kReelingAtTension2},
    {kUltimateAngler, "SEQ_BGM_REELING_LASTBOSS", kReelingAtTension2},
    {kUltimateAngler, "SEQ_BGM_ED2_DEMO", kEndingSteps},
    {kUltimateAngler, "SE_ENV_HOME_CROSS", kHomeAmbienceGroups},
    {kUltimateAngler, "SE_ANIM_CRUISER_JUMP_IN", kLaterJumpIns},
    {kUltimateAngler, "SE_ANIM_CRUISER_JUMP_IN_MY_MII", kLaterJumpIns},
    {kUltimateAngler, "SE_FEVER_PULL_2", kLaterPulls},
    {kUltimateAngler, "SE_MIIS_VOICE", kOtherCrowds},
    {kUltimateAngler, "SE_MIIS_VOICE_HARD", kOtherCrowds},
    {kUltimateAngler, "SE_MIIS_VOICE_START", kOtherCrowds},
    {kUltimateAngler, "SE_MIIS_VOICE_START_FEVER", kOtherCrowds},
    {kFlowerTown, "SE_COLOR_WHEEL_LIGHT_2", kSecondLight},
    {kMiiPlaza, "SEQ_USAGI", kUsagiStates},
    {kMiiPlaza, "BGM_NEW_Mii_Long", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_Only_One", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_MIN", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_MID", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_MAX", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_No5", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_No6", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_No7", kQuickMode},
    {kMiiPlaza, "BGM_MAIN_Mii_Only_One_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "BGM_MAIN_Mii_MIN_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "BGM_MAIN_Mii_MID_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "BGM_MAIN_Mii_MAX_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "BGM_MAIN_Mii_No5_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "BGM_MAIN_Mii_No6_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "BGM_MAIN_Mii_No7_From_New_Mii", kQuickModeAfterJingle},
    {kMiiPlaza, "SEQ_VIPROOM", kQuickMode},
    {kNinjaLauncher, "SEQ_BGM_BATTLE1", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE2", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE3", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE4", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE5", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE6", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE_BOSS", kHurryUp},
    {kNinjaLauncher, "SEQ_BGM_BATTLE_LBOSS", kHurryUp},
    {kIslandTour, "SEQ_BGM_MENU", kIslandTourMenuLayers},
    {kMarioPartyStarRush, "SQ_BGM_MGN708", kMgn708APatterns},
    {kMarioPartyStarRush, "SQ_BGM_MGN708_B_FIRST", kMgn708BPatterns},
    {kIslandTour, "SEQ_BGM_MNG_012", kMng012Layers},
    {kIslandTour, "SEQ_BGM_MNG_MG3801", kMg3801TenSecondsLeft},
    {kIslandTour, "SEQ_BGM_MNG_MG3802", kTenSecondsLeft},
    {kIslandTour, "SEQ_BGM_BOSS_04_A", kTenSecondsLeft},
    {kIslandTour, "SEQ_BGM_BOSS_04_B", kTenSecondsLeft},
    {kIslandTour, "SEQ_BGM_BOSS_04_B2", kTenSecondsLeft},
    {kIslandTour, "SEQ_BGM_BOSS_04_B3", kTenSecondsLeft},
    {kMiiForce, "SD_BGM_D01", kGameOverEndings},
    {kMiiMaker, "MEDIT_BGM_FACE", kMiiMakerMixes},
    {kMarioKart7, "SEQ_SYNC_BEAT_MUSIC_PARK", kMusicParkStates},
    {kMarioKart7, "SEQ_SYNC_BEAT_KILLER_SHIP", kKillerShipFinalLap},
    {kMarioKart7, "SEQ_SYNC_BEAT_WALUIGI_PINBALL", kWaluigiPinballFinalLap},
    {kMarioKart7, "SEQ_SYNC_BEAT_120", kSyncBeat120States},
}};

// Happy Home Designer gives each room an ambient sound: an item of the category Environment, items 0x34EE to 0x34FC.
// The room's env is the item's index in the category (0x588800). A room's data starts with env 0, "silence"
// (0x17EB88). The player can pick another item for the room (0x36BB90). The room's load starts its env (0x540DB4).
// changeEnv (0x540AFC) starts the effects manager's effect 0 for env 1, "ringing", and effect 1 for env 2, "echoes"
// (0x15CA9C). Effect 0 is nn::snd's DSP reverb and effect 1 its DSP delay, both on bus A. The other envs start no
// effect. The effect's start sets a level of 0.4 (the table at 0x639E10). The level setter (0x15CEFC) gives the room's
// music players that level times the manager's scale of 0.95 (0x10ED90): a send of 0.38 on bus A for PLAYER_BGM_MU,
// PLAYER_BGM_MU_SESSION and PLAYER_BGM_RM. Any music item can play in any room. A rip therefore writes each of the
// three players' pieces twice more, after the piece itself: once in each state. No entry above starts those pieces.
// Their states keep the default start, with the send and the effect. The six control sequences of
// PLAYER_BGM_MU_SESSION are sound effects. The list leaves them out. The code read is revision 1's. The other
// release's archive holds the same three players' pieces and the same twelve ambient loops, at other IDs.
constexpr std::array<float, 2> kRoomSend = {0.38f, 0.0f}; // 0.4 × 0.95 in single precision
constexpr std::array<SequenceArrangement, 2> kRoomStates = {{
    {"_ringing", {.fx_send = kRoomSend, .game_effect = 0}},
    {"_echoes", {.fx_send = kRoomSend, .game_effect = 1}},
}};

// The states of every sequence whose name starts with `prefix`, for the sequences that kArrangedSequences leaves out.
struct ArrangedFamily
{
    std::string_view archive_sha1;
    std::string_view prefix;
    std::span<const SequenceArrangement> arrangements;
};

// SEQ_BGM_MU covers PLAYER_BGM_MU's SEQ_BGM_MU pieces and PLAYER_BGM_MU_SESSION's SEQ_BGM_MUSS pieces.
constexpr std::array<ArrangedFamily, 4> kArrangedFamilies = {{
    {kHappyHomeDesigner, "SEQ_BGM_MU", kRoomStates},
    {kHappyHomeDesigner, "SEQ_BGM_RM", kRoomStates},
    {kHappyHomeDesignerOld, "SEQ_BGM_MU", kRoomStates},
    {kHappyHomeDesignerOld, "SEQ_BGM_RM", kRoomStates},
}};

// The games whose code holds New Leaf's tune player: New Leaf and both revisions of Happy Home Designer. Happy Home
// Designer's note writer (0x2364A8) has the same table of scales (0x639F24) and the train's tune (0x639F12).
constexpr std::array<std::string_view, 3> kTunePlayerArchives = {kNewLeaf, kHappyHomeDesigner, kHappyHomeDesignerOld};

// The tune player's sounds: the families of names, and the sounds of no family.
constexpr std::array<std::string_view, 3> kTuneSoundFamilies = {"SE_MELODY_NPCNRM_", "SE_MELODY_NPCSP_",
                                                                "SE_MELODY_DOOR_"};
constexpr std::array<std::string_view, 5> kTuneSounds = {"SE_MELODY_BOARD_EDIT", "SE_MELODY_LETTER",
                                                         "SE_MELODY_PLAYER_GHOST", "SE_MELODY_HUMMING",
                                                         "SE_MELODY_TRAIN_ANNOUNCE"};

// A sound of the tune player whose note doesn't play at the sequence's note 60: the transpose that the sound's scale
// and offset give the note.
struct TuneTranspose
{
    std::string_view sound;
    int16_t transpose = 0;
};

constexpr std::array<TuneTranspose, 186> kTuneTransposes = {{
    {"SE_MELODY_HUMMING", 12},       {"SE_MELODY_TRAIN_ANNOUNCE", 11}, {"SE_MELODY_NPCNRM_BEA05", -1},
    {"SE_MELODY_NPCNRM_BEA06", -7},  {"SE_MELODY_NPCNRM_BEA07", -3},   {"SE_MELODY_NPCNRM_BEA10", -3},
    {"SE_MELODY_NPCNRM_BEA12", -3},  {"SE_MELODY_NPCNRM_BRD00", -12},  {"SE_MELODY_NPCNRM_BRD01", -3},
    {"SE_MELODY_NPCNRM_BRD02", -12}, {"SE_MELODY_NPCNRM_BRD04", -12},  {"SE_MELODY_NPCNRM_BRD15", -17},
    {"SE_MELODY_NPCNRM_BRD16", -9},  {"SE_MELODY_NPCNRM_BRD17", -12},  {"SE_MELODY_NPCNRM_BRD18", -10},
    {"SE_MELODY_NPCNRM_BUL00", 3},   {"SE_MELODY_NPCNRM_BUL08", 2},    {"SE_MELODY_NPCNRM_CAT00", -2},
    {"SE_MELODY_NPCNRM_CAT01", -3},  {"SE_MELODY_NPCNRM_CAT05", -5},   {"SE_MELODY_NPCNRM_CAT06", -2},
    {"SE_MELODY_NPCNRM_CAT08", -2},  {"SE_MELODY_NPCNRM_CAT09", -5},   {"SE_MELODY_NPCNRM_CAT10", -5},
    {"SE_MELODY_NPCNRM_CAT11", -5},  {"SE_MELODY_NPCNRM_CAT12", -3},   {"SE_MELODY_NPCNRM_CAT13", -5},
    {"SE_MELODY_NPCNRM_CAT14", -4},  {"SE_MELODY_NPCNRM_CAT17", -3},   {"SE_MELODY_NPCNRM_CAT18", -4},
    {"SE_MELODY_NPCNRM_CAT20", 4},   {"SE_MELODY_NPCNRM_CAT21", -5},   {"SE_MELODY_NPCNRM_CBR00", 2},
    {"SE_MELODY_NPCNRM_CBR02", 1},   {"SE_MELODY_NPCNRM_CBR06", -2},   {"SE_MELODY_NPCNRM_CBR10", 1},
    {"SE_MELODY_NPCNRM_CBR16", -5},  {"SE_MELODY_NPCNRM_CHN12", -5},   {"SE_MELODY_NPCNRM_CRD01", 1},
    {"SE_MELODY_NPCNRM_CRD04", -4},  {"SE_MELODY_NPCNRM_CRD07", 7},    {"SE_MELODY_NPCNRM_CRD08", -7},
    {"SE_MELODY_NPCNRM_DER01", -5},  {"SE_MELODY_NPCNRM_DER02", -10},  {"SE_MELODY_NPCNRM_DER03", -7},
    {"SE_MELODY_NPCNRM_DER05", -7},  {"SE_MELODY_NPCNRM_DER07", -5},   {"SE_MELODY_NPCNRM_DER08", 3},
    {"SE_MELODY_NPCNRM_DER09", -7},  {"SE_MELODY_NPCNRM_DOG01", -5},   {"SE_MELODY_NPCNRM_DOG06", -2},
    {"SE_MELODY_NPCNRM_DOG07", -2},  {"SE_MELODY_NPCNRM_DOG08", -2},   {"SE_MELODY_NPCNRM_DOG15", -3},
    {"SE_MELODY_NPCNRM_DOG16", -7},  {"SE_MELODY_NPCNRM_DOG18", -2},   {"SE_MELODY_NPCNRM_DUK01", -3},
    {"SE_MELODY_NPCNRM_DUK08", -2},  {"SE_MELODY_NPCNRM_DUK15", 2},    {"SE_MELODY_NPCNRM_DUK16", 4},
    {"SE_MELODY_NPCNRM_ELP10", 2},   {"SE_MELODY_NPCNRM_FLG01", -12},  {"SE_MELODY_NPCNRM_FLG02", -12},
    {"SE_MELODY_NPCNRM_FLG03", -12}, {"SE_MELODY_NPCNRM_FLG05", -12},  {"SE_MELODY_NPCNRM_FLG06", -2},
    {"SE_MELODY_NPCNRM_FLG07", -12}, {"SE_MELODY_NPCNRM_FLG10", -12},  {"SE_MELODY_NPCNRM_FLG12", -12},
    {"SE_MELODY_NPCNRM_FLG13", -1},  {"SE_MELODY_NPCNRM_FLG16", 7},    {"SE_MELODY_NPCNRM_FLG17", -7},
    {"SE_MELODY_NPCNRM_GOA04", -7},  {"SE_MELODY_NPCNRM_GOA06", -2},   {"SE_MELODY_NPCNRM_GOA07", -12},
    {"SE_MELODY_NPCNRM_GOR00", -7},  {"SE_MELODY_NPCNRM_GOR01", -12},  {"SE_MELODY_NPCNRM_GOR02", -7},
    {"SE_MELODY_NPCNRM_GOR08", -10}, {"SE_MELODY_NPCNRM_GOR09", -3},   {"SE_MELODY_NPCNRM_GOR10", -7},
    {"SE_MELODY_NPCNRM_HAM01", 5},   {"SE_MELODY_NPCNRM_HAM03", -5},   {"SE_MELODY_NPCNRM_HAM04", 5},
    {"SE_MELODY_NPCNRM_HAM07", -2},  {"SE_MELODY_NPCNRM_HIP09", 5},    {"SE_MELODY_NPCNRM_HRS05", -3},
    {"SE_MELODY_NPCNRM_HRS11", -3},  {"SE_MELODY_NPCNRM_HRS12", -3},   {"SE_MELODY_NPCNRM_HRS13", -5},
    {"SE_MELODY_NPCNRM_KAL02", -2},  {"SE_MELODY_NPCNRM_KAL05", -8},   {"SE_MELODY_NPCNRM_KAL09", -3},
    {"SE_MELODY_NPCNRM_KAL10", -2},  {"SE_MELODY_NPCNRM_KGR00", -5},   {"SE_MELODY_NPCNRM_KGR01", -6},
    {"SE_MELODY_NPCNRM_KGR06", -6},  {"SE_MELODY_NPCNRM_KGR08", -7},   {"SE_MELODY_NPCNRM_KGR09", -7},
    {"SE_MELODY_NPCNRM_LON01", 5},   {"SE_MELODY_NPCNRM_LON06", 3},    {"SE_MELODY_NPCNRM_LON07", -3},
    {"SE_MELODY_NPCNRM_LON08", -5},  {"SE_MELODY_NPCNRM_MNK07", 3},    {"SE_MELODY_NPCNRM_MNK08", 5},
    {"SE_MELODY_NPCNRM_MUS01", -2},  {"SE_MELODY_NPCNRM_MUS03", 5},    {"SE_MELODY_NPCNRM_MUS09", -1},
    {"SE_MELODY_NPCNRM_MUS15", -1},  {"SE_MELODY_NPCNRM_MUS16", 7},    {"SE_MELODY_NPCNRM_MUS17", 7},
    {"SE_MELODY_NPCNRM_OST01", -1},  {"SE_MELODY_NPCNRM_OST07", -8},   {"SE_MELODY_NPCNRM_OST08", 3},
    {"SE_MELODY_NPCNRM_PBR05", -2},  {"SE_MELODY_NPCNRM_PBR06", -2},   {"SE_MELODY_NPCNRM_PBR07", -4},
    {"SE_MELODY_NPCNRM_PBR08", -5},  {"SE_MELODY_NPCNRM_PBR09", 7},    {"SE_MELODY_NPCNRM_PGN02", -2},
    {"SE_MELODY_NPCNRM_PGN06", 5},   {"SE_MELODY_NPCNRM_PGN10", -2},   {"SE_MELODY_NPCNRM_PGN11", -10},
    {"SE_MELODY_NPCNRM_PGN12", 3},   {"SE_MELODY_NPCNRM_PGN13", 3},    {"SE_MELODY_NPCNRM_PGN14", 5},
    {"SE_MELODY_NPCNRM_PIG03", -2},  {"SE_MELODY_NPCNRM_PIG05", -2},   {"SE_MELODY_NPCNRM_PIG08", 3},
    {"SE_MELODY_NPCNRM_PIG13", 2},   {"SE_MELODY_NPCNRM_PIG14", 5},    {"SE_MELODY_NPCNRM_PIG15", 5},
    {"SE_MELODY_NPCNRM_RBT03", -2},  {"SE_MELODY_NPCNRM_RBT06", -7},   {"SE_MELODY_NPCNRM_RBT09", -7},
    {"SE_MELODY_NPCNRM_RBT13", -7},  {"SE_MELODY_NPCNRM_RBT15", 12},   {"SE_MELODY_NPCNRM_RBT16", -7},
    {"SE_MELODY_NPCNRM_RBT18", 5},   {"SE_MELODY_NPCNRM_RBT19", 5},    {"SE_MELODY_NPCNRM_RHN01", 2},
    {"SE_MELODY_NPCNRM_RHN08", 5},   {"SE_MELODY_NPCNRM_SHP01", -2},   {"SE_MELODY_NPCNRM_SHP09", 6},
    {"SE_MELODY_NPCNRM_SHP10", 5},   {"SE_MELODY_NPCNRM_SHP11", 2},    {"SE_MELODY_NPCNRM_SQU00", -1},
    {"SE_MELODY_NPCNRM_SQU04", -5},  {"SE_MELODY_NPCNRM_SQU05", -5},   {"SE_MELODY_NPCNRM_SQU06", -3},
    {"SE_MELODY_NPCNRM_SQU07", -3},  {"SE_MELODY_NPCNRM_SQU11", -2},   {"SE_MELODY_NPCNRM_SQU15", -3},
    {"SE_MELODY_NPCNRM_SQU16", -5},  {"SE_MELODY_NPCNRM_SQU17", -2},   {"SE_MELODY_NPCNRM_SQU18", -2},
    {"SE_MELODY_NPCNRM_TIG05", 7},   {"SE_MELODY_NPCNRM_TIG06", 5},    {"SE_MELODY_NPCNRM_WOL00", 3},
    {"SE_MELODY_NPCNRM_WOL01", 5},   {"SE_MELODY_NPCNRM_WOL02", 5},    {"SE_MELODY_NPCNRM_WOL06", 5},
    {"SE_MELODY_NPCNRM_WOL10", 5},   {"SE_MELODY_NPCSP_ALW", 5},       {"SE_MELODY_NPCSP_BLN", -5},
    {"SE_MELODY_NPCSP_HGH", 2},      {"SE_MELODY_NPCSP_HGS", 1},       {"SE_MELODY_NPCSP_MKA", -2},
    {"SE_MELODY_NPCSP_PKN", 2},      {"SE_MELODY_NPCSP_PLA", -2},      {"SE_MELODY_NPCSP_SEG", -5},
    {"SE_MELODY_NPCSP_SEO", -5},     {"SE_MELODY_NPCSP_SLO", 3},       {"SE_MELODY_NPCSP_SZA0", 12},
    {"SE_MELODY_NPCSP_SZO", -12},    {"SE_MELODY_NPCSP_TAP", 3},       {"SE_MELODY_NPCSP_TTL", -12},
    {"SE_MELODY_NPCSP_WRL", -2},     {"SE_MELODY_DOOR_RECYCLE", -3},   {"SE_MELODY_DOOR_CAFE", 5},
    {"SE_MELODY_DOOR_SHOP", -5},     {"SE_MELODY_DOOR_GARDEN", 24},    {"SE_MELODY_DOOR_SHOESHOP", -7},
    {"SE_MELODY_DOOR_CLUB", 5},      {"SE_MELODY_DOOR_CASE", -12},     {"SE_MELODY_DOOR_ACCESHOP", 5},
}};

// How the tune player starts `sound`, in a game that holds the player. Without a value, the sound isn't the player's.
std::optional<SequenceStart> TuneStart(std::string_view archive_sha1, std::string_view sound)
{
    const auto in_family = [sound](std::string_view family)
    {
        return sound.starts_with(family);
    };
    const bool tune_sound = std::ranges::find(kTuneSounds, sound) != kTuneSounds.end() ||
                            std::ranges::any_of(kTuneSoundFamilies, in_family);
    if (std::ranges::find(kTunePlayerArchives, archive_sha1) == kTunePlayerArchives.end() || !tune_sound)
    {
        return std::nullopt;
    }

    const auto found = std::ranges::find(kTuneTransposes, sound, &TuneTranspose::sound);
    const int16_t transpose = found == kTuneTransposes.end() ? 0 : found->transpose;
    const SequenceVariable note = sound == "SE_MELODY_TRAIN_ANNOUNCE" ? kTrainTuneFirstNote : kTuneNoteC;

    return SequenceStart{.variables = {{note, {.index = 12, .value = transpose}}}};
}

struct BankRule
{
    std::string_view archive_sha1;
    uint32_t first_bank;
    std::span<const SequenceArrangement> arrangements;
};

// Super Mario 3D Land gives Luigi his voice through Mario's voice sequences. With its argument set, 0x25DCC0 points the
// player's sound objects at a table of four StartInfos (0x41E270, built at 0x211B5C), and it loads
// BANK_PLAYER_VOICE_LU. The banks' names make that Luigi's case. The condition behind the argument (0x268E5C, 0x277CE4)
// wasn't read. The table's method (0x3448C8) looks the sequence's first bank up in a list (0x3A88CC). It then starts
// the sound with the StartInfo of that bank. Each StartInfo holds flag 0x10 and another bank for slot 0, and
// leaves slots 1 to 3 empty. BANK_PLAYER_VOICE_MR (0x03000000) becomes BANK_PLAYER_VOICE_LU (0x03000001), and
// BANK_PLAYER_VOICE_DEMO (0x0300000C) becomes BANK_PLAYER_VOICE_DEMO_LU (0x0300000D). No sequence names either of
// Luigi's banks. BANK_PLAYER_VOICE_DEMOKOOPA (0x03000016) keeps its bank. The list leaves out the change of
// BANK_PLAYER_VOICE_DEMOKOOPALAST_MR (0x03000017) to _LU (0x03000018): the archive holds Luigi's sequences of those
// lines (SePvDLu*), which name the bank.
constexpr std::array<SequenceArrangement, 1> kLuigiVoice = {{
    {"_Luigi", {.banks = {{0x03000001, kNoBank, kNoBank, kNoBank}}}},
}};
constexpr std::array<SequenceArrangement, 1> kLuigiCutsceneVoice = {{
    {"_Luigi", {.banks = {{0x0300000D, kNoBank, kNoBank, kNoBank}}}},
}};

constexpr std::array<BankRule, 2> kBankRules = {{
    {kSuperMario3DLand, 0x03000000, kLuigiVoice},
    {kSuperMario3DLand, 0x0300000C, kLuigiCutsceneVoice},
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

SequenceStart StartingSequenceTracks(std::string_view archive_sha1, std::string_view sound)
{
    if (const std::optional<SequenceStart> tune = TuneStart(archive_sha1, sound))
    {
        return *tune;
    }

    // An entry for the whole name comes before an entry for the start of names.
    for (const bool prefix : {false, true})
    {
        for (const StartingSequence& entry : kStartingSequences)
        {
            const bool matches = prefix ? sound.starts_with(entry.sound) : sound == entry.sound;
            if (entry.prefix == prefix && entry.archive_sha1 == archive_sha1 && matches)
            {
                return entry.start;
            }
        }
    }

    return {};
}

std::span<const SequenceArrangement> SequenceArrangements(std::string_view archive_sha1, std::string_view sound)
{
    for (const ArrangedSequence& entry : kArrangedSequences)
    {
        if (entry.archive_sha1 == archive_sha1 && entry.sound == sound)
        {
            return entry.arrangements;
        }
    }

    for (const ArrangedFamily& family : kArrangedFamilies)
    {
        if (family.archive_sha1 == archive_sha1 && sound.starts_with(family.prefix))
        {
            return family.arrangements;
        }
    }

    return {};
}

std::span<const SequenceArrangement> BankArrangements(std::string_view archive_sha1, uint32_t first_bank)
{
    for (const BankRule& rule : kBankRules)
    {
        if (rule.archive_sha1 == archive_sha1 && rule.first_bank == first_bank)
        {
            return rule.arrangements;
        }
    }

    return {};
}

} // namespace ntdlib::ctr::snd

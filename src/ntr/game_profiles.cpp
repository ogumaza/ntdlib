// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/game_profiles.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"

namespace ntdlib::ntr
{
namespace
{

// A piece that a game starts with other channels for its archive player, other channels for some of the piece's
// tracks, or the capture units' reverb.
struct Piece
{
    std::string_view game; // the game code, or its first three letters for every region of the game
    std::string_view file; // the path of the image's file that holds the archive
    std::string_view label;
    uint32_t player;                  // the archive's player
    uint16_t channels;                // its channels while the piece plays
    TrackChannels tracks;             // some tracks' channels: none when its tracks are 0
    std::optional<int> reverb_volume; // the reverb's volume, or no reverb
};

// Pokémon Diamond and Pearl: 0x0200516C gives the BGM's player (7) channels 0x7FFF and starts the reverb through
// 0x02004C80 (NNS_SndCaptureStartReverb at 0x020C1FF0, with a buffer of 0x1000 bytes, PCM16 and 16,000 Hz). The scene
// function 0x0200433C calls it for scene 1 at volume 30 and for scene 14 at volume 15. Overlay 63 starts SEQ_TITLE00
// (0x021D8890) and SEQ_TITLE01 (0x021D74E0) in scene 1, and overlay 84 starts SEQ_BLD_ENDING in scene 14. Scenes 2 and
// 4 give player 7 the archive's 0x7FF again and stop the reverb. Platinum does the same at 0x020053CC, through
// 0x02004EC8 (NNS_SndCaptureStartReverb at 0x020B8ED0), for the same pieces in pl_sound_data.sdat.
//
// Pokémon HeartGold and SoulSilver have no reverb. The scene function's table at 0x02004F1E calls 0x02005AF8 with 1 or
// 2 for scenes 1 and 14, and 0x02005AF8 then gives player 7 channels 0x3FFF in place of the archive's 0xA7FE
// (NNS_SndPlayerSetAllocatableChannel). The title screen starts SEQ_GS_POKEMON_THEME in scene 1, or
// SEQ_GS_P_POKEMON_THEME with the GB sounds (the table at 0x020F5730). The credits start SEQ_GS_ENDING in scene 14,
// then SEQ_GS_ENDING2. After each start, 0x02006C14 calls 0x02005CC0. That function gives tracks 0 to 3 of the
// field's, the BGM's and the ME's handles channels 0xA7FE (NNS_SndPlayerSetTrackAllocatableChannel). The radio plays
// SEQ_GS_P_POKEMON_THEME on player 9. That player keeps the archive's channels, 0xA7FE. The table takes the title's.
constexpr TrackChannels kGsTracks = {.tracks = 0x000F, .channels = 0xA7FE};
constexpr Piece kPieces[] = {
    {"ADA", "data/sound/sound_data.sdat", "SEQ_TITLE00", 7, 0x7FFF, {}, 30},
    {"ADA", "data/sound/sound_data.sdat", "SEQ_TITLE01", 7, 0x7FFF, {}, 30},
    {"ADA", "data/sound/sound_data.sdat", "SEQ_BLD_ENDING", 7, 0x7FFF, {}, 15},
    {"APA", "data/sound/sound_data.sdat", "SEQ_TITLE00", 7, 0x7FFF, {}, 30},
    {"APA", "data/sound/sound_data.sdat", "SEQ_TITLE01", 7, 0x7FFF, {}, 30},
    {"APA", "data/sound/sound_data.sdat", "SEQ_BLD_ENDING", 7, 0x7FFF, {}, 15},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_TITLE00", 7, 0x7FFF, {}, 30},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_TITLE01", 7, 0x7FFF, {}, 30},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_BLD_ENDING", 7, 0x7FFF, {}, 15},
    {"IPK", "data/sound/gs_sound_data.sdat", "SEQ_GS_POKEMON_THEME", 7, 0x3FFF, kGsTracks, {}},
    {"IPK", "data/sound/gs_sound_data.sdat", "SEQ_GS_P_POKEMON_THEME", 7, 0x3FFF, kGsTracks, {}},
    {"IPK", "data/sound/gs_sound_data.sdat", "SEQ_GS_ENDING", 7, 0x3FFF, kGsTracks, {}},
    {"IPK", "data/sound/gs_sound_data.sdat", "SEQ_GS_ENDING2", 7, 0x3FFF, kGsTracks, {}},
    {"IPG", "data/sound/gs_sound_data.sdat", "SEQ_GS_POKEMON_THEME", 7, 0x3FFF, kGsTracks, {}},
    {"IPG", "data/sound/gs_sound_data.sdat", "SEQ_GS_P_POKEMON_THEME", 7, 0x3FFF, kGsTracks, {}},
    {"IPG", "data/sound/gs_sound_data.sdat", "SEQ_GS_ENDING", 7, 0x3FFF, kGsTracks, {}},
    {"IPG", "data/sound/gs_sound_data.sdat", "SEQ_GS_ENDING2", 7, 0x3FFF, kGsTracks, {}},
};

// Pokémon Mystery Dungeon: Blue Rescue Team's sound start-up (0x0200B1F8) starts the capture units' reverb with a
// buffer of 0x900 bytes, PCM16, 16,000 Hz and volume 45 (NNS_SndCaptureStartReverb at 0x0206EFE8). That is the one call
// of the function. The one other stop of its timers is a callback that the SDK registers (0x0206D9F4). Every sequence
// of sound.sbin therefore plays with it. The BGM's function (0x0200A5F0) starts a sequence. Then it gives the
// sequence's tracks channels (NNS_SndPlayerSetTrackAllocatableChannel at 0x0206E410). The five sequences of the table
// at 0x02086040 get 0xFFFA on every track. Every other BGM gets 0x8FFA on tracks 0 to 7 and 0x7000 on tracks 8 to 15.
// The ME's function (0x0200A564) starts its sequence on player 1 and sets no channels. Players 0 and 1 both have the
// archive's 0xFFFA. The profile comes from the USA's image (APHE).
//
// The channels drop some of SND_BGM_M_HABITAT_HOME01's notes, all on track 10.
constexpr Reverb kBlueRescueReverb = {.size = 0x900, .pcm8 = false, .sample_rate = 16000, .volume = 45};
constexpr std::array<std::string_view, 5> kBlueRescueWholeTracks = {"SND_BGM_M_SYS_TITLE_01", "SND_BGM_M_SYS_TITLE_02",
                                                                    "SND_BGM_M_SYS_TITLE_03", "SND_BGM_M_SYS_ENDING_01",
                                                                    "SND_BGM_M_SYS_ENDING_02"};

// New Super Mario Bros. and Mario Kart DS start the SDK's output effect (NNS_SndCaptureStartEffect) in mode 1, the
// surround, at boot. New Super Mario Bros.' boot starts it at 0x0204F1DC, through the SDK's function at 0x02060BEC.
// Mario Kart DS's boot does the same, through the same code at 0x0212F690. Each game's buffer holds 4,096 bytes in two
// parts, in PCM16 at 32,000 Hz: OutputEffect's defaults. Boots of both games on ntdlib's DS machine (ntr::Machine) sent
// the effect's commands with those values (OutputEffectCommands). The effect changes the whole output. Every sequence
// of each game's archives therefore plays through it. New Super Mario Bros.' save setting picks the mode after the boot
// (0x02006444): 0 gives mode 1, 1 gives mode 2 and 2 gives mode 0. A new save holds 0. The profile takes a new save's
// mode. The games are the USA releases (the codes A2DE and AMCE). Nobody has read the other releases' code.
constexpr std::string_view kOutputEffectGames = "A2DE AMCE";

// A sequence that the game starts with values in some of its variables (0 to 15 the player's, 16 to 31 the global
// ones). The variable that the game sets for the whole of a state, such as a race, also gives arrangements: one for
// each value of the first variable after the base's, up to `last`. An arrangement's suffix is `suffix`, then the value
// plus `number_offset`.
struct VariablePiece
{
    std::string_view game; // the game code, or its first three letters for every region of the game
    std::string_view file; // the path of the image's file that holds the archive
    std::string_view label;
    std::array<std::pair<uint8_t, int16_t>, 2> values; // a pair of {0, 0} after the first ends the list
    int16_t last;
    std::string_view suffix;
    int16_t number_offset;
};

// Mario Kart DS: SEQARC_0_193 to SEQARC_0_205 start with a program change whose program is local variable 0 (a1 81 00).
// Bank 45 holds programs 0 to 12. The game has 13 drivers. Each program is therefore probably one driver's voice, and
// the driver stays set for a whole race. The game's write of the variable isn't read yet. Until a run of the game shows
// each driver's program, the base takes the first program, and the arrangements the other 12, named by their programs.
// Without a value, the program is -1, and the effects play nothing.
//
// Mario Kart DS: SEQARC_0 and SEQARC_1's 290, 291, 292 and 294 add local variable 1 to local variable 2 and pan by
// variable 2 as they play (a1 b1 02 01, then a1 c0 02): a sweep across from the start that variable 2 gives, by steps
// of variable 1. The game's write of the two isn't read yet. If they come from an object's position, the effects are
// positional. The table plays them at the centre without a sweep: 64 and 0.
//
// New Super Mario Bros.: the ARM9's function at 0x0204DE10 starts a sound effect of a sequence archive. For
// SE_OBJ_GET_RED_COIN (archive 29, sequence 19), it writes global variable 1 (17 here) with the value it's given less
// 1, at 0x0204E0C8 (NNS_SndPlayerWriteGlobalVariable at 0x0205DAF4). Values 0 to 7 play the eight red coins' rising
// notes, and 7 adds the last coin's fanfare. The base takes the first coin's, and the arrangements coins 2 to 8. For
// SE_OBJ_BLOCK_EXTEND (archive 29, sequence 28), it writes global variable 0 (16 here) with its value held to 0 to 2,
// at 0x0204E07C. The sequence plays -1 as 0. The table therefore leaves it out. These addresses are where the code
// runs. The boot copies that part of the ARM9 binary 0x87E0 lower (the autoload block at 0x02043380).
constexpr VariablePiece kVariablePieces[] = {
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_193", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_194", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_195", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_196", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_197", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_198", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_199", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_200", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_201", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_202", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_203", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_204", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_205", {{{0, 0}}}, 12, "_PROGRAM", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_290", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_291", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_292", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_0_294", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_1_290", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_1_291", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_1_292", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQARC_1_294", {{{1, 0}, {2, 64}}}, 0, "", 0},
    {"A2D", "sound_data.sdat", "SE_OBJ_GET_RED_COIN", {{{17, 0}}}, 7, "_COIN", 1},
};

// A state of a sequence's tracks that a game's code sets once the sequence has started, and that stays set while it
// plays: the tracks of `on` at full volume and those of `off` at volume 0 (NNS_SndPlayerSetTrackVolume at 127 and 0),
// the tempo's ratio (NNS_SndPlayerSetTempoRatio, 256 for the sequence's tempo), the pitch of the tracks of
// `pitch_tracks` in 64ths of a semitone (NNS_SndPlayerSetTrackPitch), the player's volume in place of the archive's
// volume of the sequence (NNS_SndPlayerSetInitialVolume), and the tracks of `muted` muted and those of `unmuted` not
// (NNS_SndPlayerSetTrackMute with TRUE and FALSE). A muted track starts no notes. A row without a suffix gives the
// base, and the sequence's other rows give arrangements, in their order. An arrangement's commands follow the base's.
// Each row therefore gives the volume and the mute of every track that another row of its sequence changes.
struct TrackState
{
    std::string_view games; // game codes, or their first three letters for every region, with a space between them
    std::string_view file;  // the path of the image's file that holds the archive
    std::string_view label;
    std::string_view suffix;
    uint16_t on;
    uint16_t off;
    uint16_t tempo_ratio;
    uint16_t pitch_tracks;
    int16_t pitch;
    int16_t volume = -1; // the player's volume from 0 to 127, or -1 for the archive's
    uint16_t muted = 0;
    uint16_t unmuted = 0;
};

// Pokémon Black, White, Black 2 and White 2: the field's sound units give some zones' BGM states of their tracks. A
// trace of the game's code on ntdlib's DS machine (ntr::Machine) gave each state's commands in the form of
// TrackCommands below. The zones are the zone table's (a/0/1/2) and their names its place names (a/0/0/2, file 89 in
// Black and 109 in White 2). White 2 keeps Black's zones and tables, and the switch and dungeon units at other
// addresses (0x0203235C and 0x020316D4). They send the same commands as Black's.
//
// White and Black 2 hold the same tables, scripts and sound archives as Black and White 2, byte for byte. Their calls
// into the sound code match site for site at shifted addresses, and two differences remain. The title's cry is the
// first. Opelucid City's music is the second. The zone table gives SEQ_BGM_C_08_B in all four games. White's zone BGM
// function (0x02013BEC) plays SEQ_BGM_C_08_W for it (at 0x02013C1C). Black's function (0x02013BDC) doesn't. White 2
// does the same as White at 0x020189B8, and Black 2's function (0x02018968) does the same as Black's. Each game's
// Opelucid City row therefore names the sequence that it plays. A run of each function on the DS machine, with a
// hand-set zone entry, gives those sequences.
//
// The switch unit (a/1/3/6: a zone, the tracks to play, the tracks to silence and a fade of 60 frames) sets a zone's
// state as the player enters it (Black's 0x0202BF54, then 0x0202BFF0 through 0x02005F24), and fades the tracks that
// differ to the next zone's (0x0202BF90). A zone that the table leaves out keeps the state. Victory Road's
// SEQ_BGM_D_CHAMPROAD plays in 25 zones of Black, each in the table, and its tracks 7 and 8 play in none. Black's base
// takes Victory Road's 15 zones from 215 to 229, and White 2's _INNER does. White 2 plays the sequence in 12 more
// zones, from 573 on. They start with every track. Zone 214 adds track 10 to those of zones 215 to 229. Route 10's
// zones 356 to 364 play a track more in each zone, from track 10 alone to the tracks of zone 214. Undella Town's four
// BGMs, one for each season, play every track outside (zone 412) and leave out track 10 in the town's zones inside
// (414 to 417 in Black, 415 to 417 in White 2).
//
// The dungeon unit (a/1/4/6: a zone, a pitch and a tempo's ratio) lowers the BGM's pitch and tempo in some zones
// (Black's 0x0202B2CC): the pitch of every track but 8 and 9, and the tempo. It also gives every track a pan of 0, the
// pan that the tracks start with. The Dreamyard's SEQ_BGM_D_01 plays 3 semitones lower at 235/256 of its tempo in zone
// 153. Chargestone Cave's SEQ_BGM_D_02 plays 3 semitones lower at 235/256 in zones 196, 325 and 353, and 6 lower at
// 215/256 in zones 197 and 354.
//
// The parts unit (a/1/4/7: a sequence, up to nine groups of tracks and its zones) plays the first group alone as the
// player enters a zone of the sequence (Black's 0x0202C1D4, then 0x0202C4A4 and 0x0202C518 through 0x0202C710, and
// White 2's 0x02032B18 and 0x02032BA4). The field's scripts fade the other groups in and out with the script commands
// 0xA3 and 0xA4 (0x021AF458 and 0x021AF484 in Black's overlay 21, 0x021A7400 and 0x021A742C in White 2's overlay 36).
// The base takes every group: the sequence as the archive gives it. _ENTRY takes the first group alone.
//
// In Accumula Town (script 810), SEQ_BGM_T_02 has groups 0xFC7F, 0x0300 and 0x0080. Two musicians each turn on a group,
// and nothing turns one off. At Village Bridge (script 510), SEQ_BGM_H_04 has groups 0xFC05, 0x010A, 0x0280, 0x0020 and
// 0x0050. Four musicians each toggle a group. Those towns' other states are the player's free choice and have no rows.
// The Battle Subway's SEQ_BGM_BATTLE_SUBWAY has groups 0xF948, 0x0200, 0x0080, 0x0400, 0x0006, 0x0001 and 0x0030. As a
// car starts, the script command 0x15A turns on groups 1 to R, with R the battles won so far in the set (0x021CB182 in
// Black's overlay 21, and 0x021E649C in White 2's overlay 50). _ROUND1 to _ROUND5 give those states, and the base gives
// the last car. This is a reading of the code. A run requires a save in the subway.
//
// White 2 adds two gyms. Nimbasa City's SEQ_BGM_ERECTRIC_GYM_01 has groups 0xFCC0, 0x0032 and 0x030D. A stage of 0
// plays the first group, 1 adds the second (_STAGE1), and 2 on plays them all (0x021EEF50 in overlay 92). Virbank
// City's SEQ_BGM_POISONE_GYM_01 has groups 0xF800 (no tracks), 0x0100, 0x000E, 0x0671 and 0x0080. The zone turns on
// each group whose trainer hasn't lost (0x021EEE8C in overlay 100), and script 1004 turns one off as its trainer loses.
// The base gives the first visit. The player chooses the order of the trainers. The other states have no rows.
//
// The distance unit (a/1/3/2 in Black at 0x0202AB8C, a/1/3/3 in White 2 at 0x02030F94: a zone, a point and levels by
// distance) sets tracks 8 and 9 of the zone's BGM to the product of its levels on the three axes (Black's 0x0202AF74,
// then 0x0202AEBC through 0x02005F24). The level is 127 within 3 tiles of the point and 0 from 15 tiles on. The zones
// are Nacrene City (SEQ_BGM_C_02), Icirrus City (SEQ_BGM_C_07), Opelucid City (SEQ_BGM_C_08_B or _W), Anville Town
// (SEQ_BGM_T_03) and, in Black and White alone, Driftveil City's gym (SEQ_BGM_GYM_C05). The base gives the sequence at
// the point, and _FAR gives it from 15 tiles on.
//
// The 3D unit (a/1/8/5 in Black at 0x0202BB48, a/1/8/2 in White 2 at 0x02031F50: a sequence and tracks) silences those
// tracks as the sequence starts. Up to 16 sources then set one track each, by the distance from the camera and with a
// pan by the direction (Black's 0x0202BBFC and 0x0202BCA0). Those are tracks 6 to 9 of Skyarrow Bridge's SEQ_BGM_H_01
// and 7 to 9 of SEQ_BGM_H_03. The base gives every track at the sequence's pans, and _FAR gives the start's state. The
// pans of a source aren't modelled.
//
// Pokémon Diamond, Pearl and Platinum: a new game starts with the TV's scene. It plays SEQ_TV_HOUSOU on the field's
// handle at the archive's volume of 96 (Diamond's overlay 59, through 0x0200414C and 0x0200433C). The player's room
// then loads with no new BGM, and the TV's music plays on. As the room loads, its script sets the sequence's volume to
// 50: script command 0x283 in Diamond's script 982 (0x0200488C), and Platinum's script through 0x02004AD4. The music
// keeps that volume until the program ends and the town's music starts. _ROOM gives the room's volume. A run of each
// game's start and call on ntdlib's DS machine sent the player's volume -24 (in 0.1 dB) at the start and -81 after the
// call. Diamond's run stands for Pearl. Nobody checked Pearl's scripts.
//
// Pokémon Platinum, HeartGold and SoulSilver: the Wi-Fi Plaza's games play SEQ_PL_WIFIGAME (Platinum's overlay 114) and
// SEQ_GS_WIFIGAME (HeartGold's overlay 90, at 0x02259588) in scene 19. One of them raises the tempo near the end of
// each round: Platinum's overlay 115 and HeartGold's overlay 91. Once the round's counter reaches 660 (a round lasts
// 1,200 frames), the ratio rises from 258 to 406 over 58 frames. It stays there until the round ends, and the end sets
// 256 again. A run of each overlay's ramp and reset on ntdlib's DS machine sent the 58 ratios, then 256 (Platinum's
// 0x022658F8 and 0x02265A08, HeartGold's 0x02261A00 and 0x02261B10). _FAST gives the last ratio.
//
// Mario Kart DS: each race course plays its BGM, one of SEQ_15 to SEQ_41 and SEQ_74 (the course table of the USA
// release). On the race's final lap, the game plays the jingle SEQ_62, then starts the course's BGM again (0x0210D5C0)
// and sets its tempo ratio to 300 (0x02106828, 0x02106CE8 and 0x02106E30). The ratio holds until the race ends.
// _FINAL_LAP gives that ratio. A run of the USA release on ntdlib's DS machine anchors the restart. No run reached a
// race. The run therefore patched the title. 0x02106188 branched to the race's sound frame (0x021061B8), and the
// race's state was set to the second of three laps. A call of the game's lap code (0x02106AB8) then made it the final
// lap. The code started the jingle on another player at once. The sound frame waits until the jingle's handle is free
// (0x02106740). It started the BGM again 3.24 s later. SEQ_62 lasts 3.7 s with its release tails. The same list of
// commands set the player's tempo ratio to 300 before the start. The run's BGM was the one that the title left in the
// race's sound state, not a course's.
//
// New Super Mario Bros.: in overlay 0, a level's time falls by 92/4096 of a unit each frame (0x020A2764). Once the
// time reaches 100, the code sets the BGM's tempo ratio to 358 and plays the sound 318 (0x020A27A4 to 0x020A27C4,
// through 0x02011D60). The ratio holds until the scene's sound reset (0x02012638). Each BGM that starts meanwhile
// takes the ratio too (0x02011E7C). A level's music is the music byte of its area's entry (0x0201E020). The courses'
// areas name the sequences of the rows below, apart from five. The code of the boss fights starts BGM_BOSS and
// BGM_FINAL_KUPPA, and the power-ups start BGM_MUTEKI, BGM_HUGE and BGM_SWITCH. In the Mario vs. Luigi courses, the
// ratio is 358 while one player leads by two stars and 256 otherwise (0x020A26E4 to 0x020A273C). _HURRY gives the ratio
// of 358.
//
// The jingles of a course start through 0x020210DC. It stops the BGM (0x0204E87C), starts the jingle through 0x02011E7C
// and then sets the ratio to 256 (0x0204E52C). The list of commands that starts a jingle therefore sets 358, then 256,
// before the start. The 256 also clears the flag that makes each sound frame set 358 again (0x0204E4D0 and 0x0204E924).
// Overlay 10 starts the jingles of the single-player courses this way: BGM_DOWN at each death (0x0210F8E8),
// BGM_GOAL_FANFARE2 and BGM_COURSE_CLEAR at the goal (0x0211C184 and 0x0211C194), BGM_FINAL_CLEAR and BGM_BOSS_CLEAR
// after a castle's boss (0x0211A58C and 0x0211A5B0), and BGM_BOSS_CLEAR after Bowser Jr. (0x0211B024). The jingles play
// at 256, and none takes _HURRY. The scene of the game over resets the sound state before it starts BGM_GAMEOVER
// (overlay 4's 0x020CC4E4 and 0x020CC8CC).
//
// Runs of the USA release on ntdlib's DS machine anchor the ratio and the jingles. Each run loaded a course of World 1
// (0x0200696C) and set the time to 101. The code then played the sound 318, and the next list set the BGM's ratio to
// 358. Each run then brought on one jingle. A time of 3 ran out and gave BGM_DOWN. A call of 0x02104474 and a patch of
// Mario's flags put Mario on the goal pole, once with a time of 99 and once with 97. The goal plays BGM_GOAL_FANFARE2
// when the time's last two digits match, and BGM_COURSE_CLEAR otherwise. Patches of Mario's flags brought on the clears
// of the castle's boss and of the last boss in World 1's castle, and of Bowser Jr. in its tower. Each jingle played at
// 256. A run of the star played BGM_MUTEKI at 358. The course's BGM then started again from its start at 358. In the
// Mario vs. Luigi courses, the code sets 256 after each jingle too (overlay 10's 0x020FB988, 0x020FB9D0, 0x020FBB54 and
// 0x020FBD70). That part and the game over are readings.
//
// New Super Mario Bros.: the World 8 map plays BGM_WORLD8. As the sequence starts, the code mutes tracks 1 and 8 while
// a flag of the save is clear (0x0204E7D8 to 0x0204E818, through NNS_SndPlayerSetTrackMute at 0x0205DC74). The flag
// is bit 7 of the save's byte at 0x125 (0x02088D01). The world map reads it again as it loads (overlay 8's 0x020D9828).
// It then mutes the two tracks again, or unmutes every track (0x0204D98C). With the flag set, two functions of the map
// return other values for World 8 (overlay 8's 0x020CE184 and 0x020DD038). A new save has the flag clear. Nobody has
// found the code that sets it. The base mutes the two tracks, and _FULL plays every track. This is a reading of the
// USA release's code. A run that called 0x0204D98C at the title sent the driver no command: the title's music doesn't
// play on the BGM's handles.
constexpr TrackState kTrackStates[] = {
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "", 0x027F, 0xFD80, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_1", 0x0400, 0xFBFF, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_2", 0x0600, 0xF9FF, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_3", 0x0610, 0xF9EF, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_4", 0x0650, 0xF9AF, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_5", 0x0654, 0xF9AB, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_6", 0x0674, 0xF98B, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_7", 0x067C, 0xF983, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_8", 0x067E, 0xF981, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_9", 0x067F, 0xF980, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_T_05_SP", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_T_05_SU", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_T_05_AU", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_T_05_WI", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_01", "_LOWER1", 0, 0, 235, 0xFCFF, -192},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_02", "_LOWER1", 0, 0, 235, 0xFCFF, -192},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_D_02", "_LOWER2", 0, 0, 215, 0xFCFF, -384},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_T_02", "_ENTRY", 0xFC7F, 0x0380, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_H_04", "_ENTRY", 0xFC05, 0x03FA, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ENTRY", 0xF948, 0x06B7, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND1", 0xFB48, 0x04B7, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND2", 0xFBC8, 0x0437, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND3", 0xFFC8, 0x0037, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND4", 0xFFCE, 0x0031, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND5", 0xFFCF, 0x0030, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_C_02", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_GYM_C05", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_C_07", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRBO", "wb_sound_data.sdat", "SEQ_BGM_C_08_B", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRAO", "wb_sound_data.sdat", "SEQ_BGM_C_08_W", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_T_03", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_H_01", "_FAR", 0, 0x03C0, 256, 0, 0},
    {"IRBO IRAO", "wb_sound_data.sdat", "SEQ_BGM_H_03", "_FAR", 0, 0x0380, 256, 0, 0},

    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_INNER", 0x027F, 0xFD80, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_1", 0x0400, 0xFBFF, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_2", 0x0600, 0xF9FF, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_3", 0x0610, 0xF9EF, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_4", 0x0650, 0xF9AF, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_5", 0x0654, 0xF9AB, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_6", 0x0674, 0xF98B, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_7", 0x067C, 0xF983, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_8", 0x067E, 0xF981, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_CHAMPROAD", "_ROUTE10_9", 0x067F, 0xF980, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_T_05_SP", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_T_05_SU", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_T_05_AU", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_T_05_WI", "_INDOORS", 0x03FF, 0xFC00, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_01", "_LOWER1", 0, 0, 235, 0xFCFF, -192},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_02", "_LOWER1", 0, 0, 235, 0xFCFF, -192},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_D_02", "_LOWER2", 0, 0, 215, 0xFCFF, -384},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_T_02", "_ENTRY", 0xFC7F, 0x0380, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_H_04", "_ENTRY", 0xFC05, 0x03FA, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ENTRY", 0xF948, 0x06B7, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND1", 0xFB48, 0x04B7, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND2", 0xFBC8, 0x0437, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND3", 0xFFC8, 0x0037, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND4", 0xFFCE, 0x0031, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_BATTLE_SUBWAY", "_ROUND5", 0xFFCF, 0x0030, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_ERECTRIC_GYM_01", "_ENTRY", 0xFCC0, 0x033F, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_ERECTRIC_GYM_01", "_STAGE1", 0xFCF2, 0x030D, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_C_02", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_C_07", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRDO", "swan_sound_data.sdat", "SEQ_BGM_C_08_W", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IREO", "swan_sound_data.sdat", "SEQ_BGM_C_08_B", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_T_03", "_FAR", 0, 0x0300, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_H_01", "_FAR", 0, 0x03C0, 256, 0, 0},
    {"IRDO IREO", "swan_sound_data.sdat", "SEQ_BGM_H_03", "_FAR", 0, 0x0380, 256, 0, 0},

    {"ADA APA", "data/sound/sound_data.sdat", "SEQ_TV_HOUSOU", "_ROOM", 0, 0, 256, 0, 0, 50},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_TV_HOUSOU", "_ROOM", 0, 0, 256, 0, 0, 50},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_PL_WIFIGAME", "_FAST", 0, 0, 406, 0, 0},
    {"IPK IPG", "data/sound/gs_sound_data.sdat", "SEQ_GS_WIFIGAME", "_FAST", 0, 0, 406, 0, 0},

    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_15", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_16", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_17", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_18", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_19", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_20", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_21", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_22", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_23", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_24", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_25", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_26", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_27", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_28", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_29", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_30", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_31", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_32", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_33", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_34", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_35", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_36", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_37", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_38", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_39", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_40", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_41", "_FINAL_LAP", 0, 0, 300, 0, 0},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_74", "_FINAL_LAP", 0, 0, 300, 0, 0},

    {"A2DE", "sound_data.sdat", "BGM_VS_CHIJOU", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_TRIDE", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_MUTEKI", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_HUGE", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_SABAKU", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_BOSS", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_CHIKA", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_MAME", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_WATER3", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_KAZAN_SOTO", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_SANBASHI", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_KUPPAJR", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_OBAKE", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_SHIRO", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_SWITCH", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_FINAL_KUPPA", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_ATHLETIC", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_MINIGAME", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_CHIJOU_CONT", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_AMB_YOUGAN", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_AMB_SABAKU", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_AMB_WATER", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_AMB_SKY", "_HURRY", 0, 0, 358, 0, 0},
    {"A2DE", "sound_data.sdat", "BGM_WORLD8", "", 0, 0, 256, 0, 0, -1, 0x0102, 0},
    {"A2DE", "sound_data.sdat", "BGM_WORLD8", "_FULL", 0, 0, 256, 0, 0, -1, 0, 0xFFFF},
};

// The commands of a row of kTrackStates, in the order that the game's code sends them: the player's volume, the tracks'
// volumes, the tempo, the pitch, then the mutes. The player's volume is its level in 0.1 dB (the driver's kDecibel) in
// the player's parameter at 0x06 (2 bytes). A track's volume is a fader in 0.1 dB of the tracks' parameter at 0x0A (2
// bytes): 0 for 127, -32768 for 0. The tempo's ratio is the player's parameter at 0x1A (2 bytes), and the pitch the
// tracks' parameter at 0x0C (2 bytes). A mute is the driver's command 8 with the tracks and mode 1, or mode 0 for none.
// The sequence plays on the driver's player 0.
snd::CommandList TrackCommands(const TrackState& state)
{
    using snd::CommandId;
    constexpr uint32_t kTwoBytes = 2u << 24;
    snd::CommandList commands;

    if (state.volume >= 0)
    {
        const int16_t level = snd::tables::kDecibel[std::min<int16_t>(state.volume, 127)];
        commands.push_back(
            {.id = CommandId::kPlayerParameter, .args = {0, 0x06, static_cast<uint32_t>(int32_t{level}), 2}});
    }
    if (state.on != 0)
    {
        commands.push_back({.id = CommandId::kTrackParameter, .args = {kTwoBytes, state.on, 0x0A, 0}});
    }
    if (state.off != 0)
    {
        commands.push_back({.id = CommandId::kTrackParameter, .args = {kTwoBytes, state.off, 0x0A, 0xFFFF8000}});
    }
    if (state.tempo_ratio != 256)
    {
        commands.push_back({.id = CommandId::kPlayerParameter, .args = {0, 0x1A, state.tempo_ratio, 2}});
    }
    if (state.pitch_tracks != 0)
    {
        commands.push_back(
            {.id = CommandId::kTrackParameter,
             .args = {kTwoBytes, state.pitch_tracks, 0x0C, static_cast<uint32_t>(static_cast<int32_t>(state.pitch))}});
    }
    if (state.muted != 0)
    {
        commands.push_back({.id = CommandId::kMuteTrack, .args = {0, state.muted, 1}});
    }
    if (state.unmuted != 0)
    {
        commands.push_back({.id = CommandId::kMuteTrack, .args = {0, state.unmuted, 0}});
    }

    return commands;
}

// Whether `code` is one of `games`: a list of game codes or their first three letters, with a space between them.
bool IsGameOf(std::string_view games, std::string_view code)
{
    while (!games.empty())
    {
        const std::size_t end = std::min(games.find(' '), games.size());
        if (code.starts_with(games.substr(0, end)))
        {
            return true;
        }
        games.remove_prefix(std::min(end + 1, games.size()));
    }

    return false;
}

// The sequence that GameProfileOf looks up.
struct Place
{
    std::string_view game_code;
    std::string_view file;
    uint32_t index_in_file;
    std::string_view label;
};

// Whether a row for `games` (see IsGameOf), the file `file` and the sequence `label` names the sequence at `place`.
// Every row names a sequence of the first archive in its file.
bool Names(std::string_view games, std::string_view file, std::string_view label, const Place& place)
{
    return IsGameOf(games, place.game_code) && place.file == file && place.index_in_file == 0 && place.label == label;
}

// Gives variable `index` the value `value`: 0 to 15 the player's, 16 to 31 the global ones.
void SetVariable(Variables& variables, uint8_t index, int16_t value)
{
    if (index < 16)
    {
        variables.local[index] = value;
    }
    else if (index < 32)
    {
        variables.global[index - 16u] = value;
    }
}

// Adds the rows of kPieces and Blue Rescue Team's settings for the sequence at `place`.
void AddChannelsAndReverb(const Place& place, GameSettings& settings)
{
    for (const Piece& piece : kPieces)
    {
        if (Names(piece.game, piece.file, piece.label, place))
        {
            settings.channels[piece.player] = piece.channels;
            if (piece.tracks.tracks != 0)
            {
                settings.track_channels.push_back(piece.tracks);
            }
            if (piece.reverb_volume)
            {
                settings.reverb =
                    Reverb{.size = 0x1000, .pcm8 = false, .sample_rate = 16000, .volume = *piece.reverb_volume};
            }
        }
    }

    if (IsGameOf("APHE", place.game_code) && place.file == "sound.sbin" && place.index_in_file == 0)
    {
        settings.reverb = kBlueRescueReverb;
        if (std::ranges::find(kBlueRescueWholeTracks, place.label) != kBlueRescueWholeTracks.end())
        {
            settings.track_channels.push_back({.tracks = 0xFFFF, .channels = 0xFFFA});
        }
        else if (place.label.starts_with("SND_BGM_"))
        {
            settings.track_channels.push_back({.tracks = 0x00FF, .channels = 0x8FFA});
            settings.track_channels.push_back({.tracks = 0xFF00, .channels = 0x7000});
        }
    }
}

// Adds the rows of kTrackStates for the sequence at `place`: the base's commands, and an arrangement for each other
// row.
void AddTrackStates(const Place& place, GameProfile& profile)
{
    for (const TrackState& state : kTrackStates)
    {
        if (!Names(state.games, state.file, state.label, place))
        {
            continue;
        }

        snd::CommandList commands = TrackCommands(state);
        if (state.suffix.empty())
        {
            profile.commands.insert(profile.commands.end(), commands.begin(), commands.end());
        }
        else
        {
            profile.arrangements.push_back({.suffix = std::string(state.suffix), .commands = std::move(commands)});
        }
    }
}

// Adds the first row of kVariablePieces for the sequence at `place`: the base's variables, and an arrangement for each
// later value of the first variable.
void AddVariables(const Place& place, GameProfile& profile)
{
    const auto named = [&place](const VariablePiece& piece)
    {
        return Names(piece.game, piece.file, piece.label, place);
    };
    const auto* const piece = std::ranges::find_if(kVariablePieces, named);
    if (piece == std::ranges::end(kVariablePieces))
    {
        return;
    }

    for (std::size_t i = 0; i < piece->values.size(); i++)
    {
        const auto [variable, value] = piece->values[i];
        if (i > 0 && variable == 0 && value == 0)
        {
            break;
        }

        SetVariable(profile.settings.variables, variable, value);
    }

    const auto [variable, base] = piece->values[0];
    for (int value = base + 1; value <= piece->last; value++)
    {
        profile.arrangements.push_back(
            {.suffix = std::string(piece->suffix) + std::to_string(value + piece->number_offset),
             .variables = {{variable, static_cast<int16_t>(value)}}});
    }
}

} // namespace

GameProfile GameProfileOf(std::string_view game_code, std::string_view file, uint32_t index_in_file,
                          std::string_view label)
{
    const Place place = {.game_code = game_code, .file = file, .index_in_file = index_in_file, .label = label};
    GameProfile profile;
    AddChannelsAndReverb(place, profile.settings);

    if (IsGameOf(kOutputEffectGames, game_code))
    {
        profile.settings.effect = OutputEffect{};
    }

    AddTrackStates(place, profile);
    AddVariables(place, profile);

    return profile;
}

} // namespace ntdlib::ntr

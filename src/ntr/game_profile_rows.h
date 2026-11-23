// SPDX-License-Identifier: MIT

// The rows of the DS games' profiles that game_profiles.cpp applies: the pieces with other channels or the capture
// units' reverb, the stream locks, the pieces whose variables the games set, and the tracks' states of the pieces. Only
// game_profiles.cpp includes this header. The rows therefore keep internal linkage.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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

// The Legend of Zelda: Spirit Tracks (BKIE and BKIP) plays its streams only in cutscenes. Overlay 88's demo object runs
// the BGM track of a cutscene file (/Event/dNN_NN.bin), one tick in each frame of the game, about 30 a second. A key of
// 1 on a stream number prepares the stream (0x0206D2C0). The preparation locks channels 6 and 7
// (NNS_SndStrmAllocChannel at 0x02006520, NNS_SndLockChannel at 0x020036A8). They stay locked until the stream stops.
// d20_03 prepares stream 120 at tick 7 and plays it at tick 2550. Between those ticks it starts SEQ_279, SEQ_280,
// SEQ_281 and SEQ_282 through the BGM manager (0x0206C988). These four sequences therefore play with channels 0xFF3F.
// The cutscene is the one start of them found. Both releases hold the same cutscene files and the same sequences.
// Runs of the game code played the BGM track of d20_03 with the stream and without it. With the stream, 272 of the 642
// channel runs of the four sequences end earlier, and every channel starts and stops in the same driver frame as on the
// driver's model with these channels. In four other cutscenes a stream's preparation locks the channels while a
// sequence plays. kStreamLocks gives those locks. In the rest, a stream's lock reaches no note.
//
// Phantom Hourglass (AZEE) also plays its streams in cutscenes, and a stereo stream also locks channels 6 and 7. Runs
// of eight cutscenes' BGM tracks (demo01, demo10, demo14, demo17, demo18_02, demo21_03, demo24 and demo26_02) found no
// note that sounds while a stream holds the channels: each sequence ends or fades out first. The game has no row.
constexpr TrackChannels kUnderStereoStream = {.tracks = 0xFFFF, .channels = 0xFF3F};

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
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_279", 0, 0, kUnderStereoStream, {}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_280", 0, 0, kUnderStereoStream, {}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_281", 0, 0, kUnderStereoStream, {}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_282", 0, 0, kUnderStereoStream, {}},
};

// A change of a stream's channels while a piece plays (ChannelLock).
struct StreamLock
{
    std::string_view game;
    std::string_view file;
    std::string_view label;
    ChannelLock lock;
};

// Spirit Tracks' cutscenes that prepare a stream while a sequence plays. The preparation locks channels 6 and 7, and
// the lock stops the sequence's notes on them. The frame of a row counts driver frames from the start of the sequence.
// Runs of the game code played the BGM track of each cutscene with the stream and without it. With the locks of a row,
// every channel of the sequence starts and stops in the same driver frame as in the run with the stream.
//
// - d04_04 starts SEQ_184 and prepares stream 22 82 ticks later. At 2.73 s, the lock ends a note's release 0.68 s
//   early.
// - d06_01 starts SEQ_194 and prepares stream 35 292 ticks later. At 9.76 s, the lock ends two notes that the sequence
//   holds to its end.
// - d15_01 starts SEQ_251 and prepares stream 92 795 ticks later. At 26.58 s, the lock ends two notes' releases 1.27 s
//   and 2.27 s early.
// - d24_01 stops stream 144 with a fade of 4 and starts SEQ_304 in the same tick. The stream holds channels 6 and 7
//   until its fade ends, a few frames into the sequence. The cutscene prepares stream 145 40 ticks later. At 1.33 s,
//   the lock ends two notes' releases 0.22 s and 0.71 s early. The run's unlock and lock came 10 and 260 driver frames
//   after the call that starts the sequence. The comparison puts the lock at frame 256, and the row puts the unlock at
//   frame 6. Any frame from 5 to 8 gives the same channels: no note starts in between.
constexpr StreamLock kStreamLocks[] = {
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_184", {.frame = 526, .channels = 0xC0}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_194", {.frame = 1875, .channels = 0xC0}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_251", {.frame = 5104, .channels = 0xC0}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_304", {.frame = 0, .channels = 0xC0}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_304", {.frame = 6, .channels = 0xC0, .unlock = true}},
    {"BKIE BKIP", "SoundData/final_sound_data.sdat", "SEQ_304", {.frame = 256, .channels = 0xC0}},
};

// The Legend of Zelda: Four Swords Anniversary Edition (KQ9E and KQ9V) has no row. The addresses here are those of
// KQ9E. In KQ9V, 0x02076FE0 is the same and the others are 0x20 higher. The sound start-up (0x0208CD84) first gives the
// players channels from section 0 of knife_sound.bin, in the loop at 0x0208CE5C (NNS_SndPlayerSetAllocatableChannel at
// 0x020BF7E0). Player 0 gets 0x0FFF, player 3 gets 0x3F00 and the others get 0xFF00. The start-up then calls 0x0208C508
// at 0x0208CEB0. That function calls NNS_SndInit (0x020BF164, known by its form). The player init that NNS_SndInit
// calls (0x020BFA48) sets the mask of every player to 0 at 0x020BFADC. NNS_SndArcPlayerSetup (0x020C1640, called
// through 0x0208BD04 at 0x0208CF28) then stores each player's mask from the archive at 0x020C1678. Every mask there is
// 0. NNS_SndPlayer passes a mask to the driver only when it isn't 0 (0x020BFD1C). Every player therefore keeps all 16
// channels.
//
// 0x0208C508 also gives the two stream objects channels 0 and 1. A stream start (0x02090558) locks the channel of the
// object through 0x020BF4C4 (NNS_SndLockChannel at 0x020BF3F8). A stream stop (0x020904F8) unlocks it through
// 0x020BF508. The game has 8 streams, with ids from 490. Each is mono: byte 0x12 of its first entry in section 3 of
// knife_sound.bin is 0. The play routine (0x0208D240) then starts only object 0 (0x0208D384). A stream therefore locks
// channel 0 while it plays. A sequence that starts while a stream plays gets channels 0xFFFE. The play routine leaves a
// stream playing when it starts a sequence. The stop request 0x8002xxxx to the sound request function (0x02076FE0)
// stops the stream (0x0208D50C). The game calls that function from 312 places, most with the id from a table or a
// register. The code therefore doesn't show which sequences start while a stream plays. The table has no rows for the
// game for that reason. Each sequence plays with all 16 channels.
//
// In runs of the game with input, streams played in the Realm of Memories, stage group 5 in the table at 0x02116700.
// The runs set the stage select's choice (0x0217707C) to it. There the game sent the stop request before each sequence
// it started. At a game over it stops stream 496 and starts BGM_SND423 and BGM_SND417 in the same frame (0x02062D20 to
// 0x02062D50). The stream's channel stops one driver frame before the first notes of the two sequences, and one of
// those notes takes channel 0. No sequence in those runs started while a stream held its channel.
//
// The ARM9 sends no command that writes a variable (SND commands 10 and 11). The game starts no capture, sends no mute,
// links no setter of a track volume or pitch, and sets the volume of every player to 127 (0x0208D96C and 0x0208D978).
// Starting a sequence pauses player 0 (0x0208D304) when the current BGM is a sequence (an id below 490) and the bit of
// the new sequence is set in section 2 of knife_sound.bin (0x0208D25C to 0x0208D2A4). Only BGM_SND423 has its bit set.
// It plays on player 11. The sound manager resumes player 0 once player 11 has stopped (0x0208D07C). The pause changes
// no sequence that plays alone. A request 0x8006xxxx to the sound request function sets the tempo ratio of the current
// BGM through 0x0208D770. No code in either image builds that request.

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
// parts, in PCM16 at 32,000 Hz: OutputEffect's defaults. OutputEffectCommands gives the effect's commands with those
// values. The effect changes the whole output. Every sequence of each game's archives therefore plays through it. New
// Super Mario Bros.' save setting picks the mode after the boot (0x02006444): 0 gives mode 1, 1 gives mode 2 and 2
// gives mode 0. A new save holds 0. The profile takes a new save's mode. The games are the USA releases (the codes A2DE
// and AMCE). Nobody has read the other releases' code.
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
// the driver stays set for a whole race. The game's write of the variable isn't read yet. Until it is, the base takes
// the first program, and the arrangements the other 12, named by their programs.
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

// Pokémon Black, White, Black 2 and White 2: the field's sound units give some zones' BGM states of their tracks. The
// rows below give the commands that the game's code sends for each state, in the form of TrackCommands. The zones are
// the zone table's (a/0/1/2) and their names its place names (a/0/0/2, file 89 in Black and 109 in White 2). White 2
// keeps Black's zones and tables, and the switch and dungeon units at other addresses (0x0203235C and 0x020316D4). They
// send the same commands as Black's.
//
// White and Black 2 hold the same tables, scripts and sound archives as Black and White 2, byte for byte. Their calls
// into the sound code match site for site at shifted addresses, and two differences remain. The title's cry is the
// first. Opelucid City's music is the second. The zone table gives SEQ_BGM_C_08_B in all four games. White's zone BGM
// function (0x02013BEC) plays SEQ_BGM_C_08_W for it (at 0x02013C1C). Black's function (0x02013BDC) doesn't. White 2
// does the same as White at 0x020189B8, and Black 2's function (0x02018968) does the same as Black's. Each game's
// Opelucid City row therefore names the sequence that it plays.
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
// the last car.
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
// The four games send a second DS a download-play program, Poké Transfer, with the file a/1/4/2: a NARC whose member 0
// is the sound archive of the program's minigame. The program plays SEQ_BGM_PALPARK_GAME from it. The minigame's
// countdown (0x02019C8C in the program of Black and White, 0x020160F4 in that of Black 2 and White 2) counts a round
// down from 7200 frames. When 1200 frames are left, the countdown calls the program's tuning routine with every track,
// a tempo ratio of 285 and a pitch of 85, and leaves the pan alone (0x02019DF0 and 0x02016258). The tuning routine sets
// the ratio through NNS_SndPlayerSetTempoRatio and the pitch through NNS_SndPlayerSetTrackPitch. The state stays set to
// the round's end. _HURRY gives it.
//
// Pokémon Diamond, Pearl and Platinum: a new game starts with the TV's scene. It plays SEQ_TV_HOUSOU on the field's
// handle at the archive's volume of 96 (Diamond's overlay 59, through 0x0200414C and 0x0200433C). The player's room
// then loads with no new BGM, and the TV's music plays on. As the room loads, its script sets the sequence's volume to
// 50: script command 0x283 in Diamond's script 982 (0x0200488C), and Platinum's script through 0x02004AD4. The music
// keeps that volume until the program ends and the town's music starts. _ROOM gives the room's volume. The start sets
// the player's volume to -24 (in 0.1 dB), and the call sets -81. Pearl takes Diamond's values.
//
// Pokémon Platinum, HeartGold and SoulSilver: the Wi-Fi Plaza's games play SEQ_PL_WIFIGAME (Platinum's overlay 114) and
// SEQ_GS_WIFIGAME (HeartGold's overlay 90, at 0x02259588) in scene 19. One of them raises the tempo near the end of
// each round: Platinum's overlay 115 and HeartGold's overlay 91. Once the round's counter reaches 660 (a round lasts
// 1,200 frames), the ratio rises from 258 to 406 over 58 frames. It stays there until the round ends, and the end sets
// 256 again. Each overlay's ramp and reset send the 58 ratios, then 256 (Platinum's 0x022658F8 and 0x02265A08,
// HeartGold's 0x02261A00 and 0x02261B10). _FAST gives the last ratio.
//
// Mario Kart DS: each race course plays its BGM, one of SEQ_15 to SEQ_41 and SEQ_74 (the course table of the USA
// release). On the race's final lap, the game plays the jingle SEQ_62, then starts the course's BGM again (0x0210D5C0)
// and sets its tempo ratio to 300 (0x02106828, 0x02106CE8 and 0x02106E30). The ratio holds until the race ends.
// _FINAL_LAP gives that ratio. The game's lap code (0x02106AB8) starts the jingle on another player at once. The sound
// frame (0x021061B8) waits until the jingle's handle is free (0x02106740), then starts the BGM again. The same list of
// commands sets the player's tempo ratio to 300 before the start.
//
// The final lap also raises the BGM by a semitone. After the new start, 0x0210FAAC sets a flag (0x0210682C). In each
// later frame of the race, the sound frame's call of 0x0210FB88 (through 0x0210FAF4) then sets the pitch of every track
// to 64 (0x0210FE58 to 0x0210FE90). The race runs while the word at 0x0217C800 is 1. Only the race's reset clears the
// flag (0x0210FB68). The first pitch goes out a frame after the start. _FINAL_LAP gives the pitch with the ratio. A run
// of the game's code from the sound update started SEQ_15 through 0x0210D7DC and called 0x0212C494 with 300, then
// 0x0210FAAC and 0x0210FAF4. It sent the ratio, then the tracks' parameter 0x0C with 64 on tracks 0xFFFF.
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
// The goal plays BGM_GOAL_FANFARE2 when the time's last two digits match, and BGM_COURSE_CLEAR otherwise. The star
// plays BGM_MUTEKI at 358. The course's BGM then starts again from its start at 358. In the Mario vs. Luigi courses,
// the code sets 256 after each jingle too (overlay 10's 0x020FB988, 0x020FB9D0, 0x020FBB54 and 0x020FBD70).
//
// New Super Mario Bros.: the World 8 map plays BGM_WORLD8. As the sequence starts, the code mutes tracks 1 and 8 while
// a flag of the save is clear (0x0204E7D8 to 0x0204E818, through NNS_SndPlayerSetTrackMute at 0x0205DC74). The flag
// is bit 7 of the save's byte at 0x125 (0x02088D01). The world map reads it again as it loads (overlay 8's 0x020D9828).
// It then mutes the two tracks again, or unmutes every track (0x0204D98C). With the flag set, two functions of the map
// return other values for World 8 (overlay 8's 0x020CE184 and 0x020DD038). A new save has the flag clear. Nobody has
// found the code that sets it. The base mutes the two tracks, and _FULL plays every track. The addresses are the USA
// release's. The title's music doesn't play on the BGM's handles.
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
    {"IRBO IRAO", "a/1/4/2", "SEQ_BGM_PALPARK_GAME", "_HURRY", 0, 0, 285, 0xFFFF, 85},
    {"IRDO IREO", "a/1/4/2", "SEQ_BGM_PALPARK_GAME", "_HURRY", 0, 0, 285, 0xFFFF, 85},

    {"ADA APA", "data/sound/sound_data.sdat", "SEQ_TV_HOUSOU", "_ROOM", 0, 0, 256, 0, 0, 50},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_TV_HOUSOU", "_ROOM", 0, 0, 256, 0, 0, 50},
    {"CPU", "data/sound/pl_sound_data.sdat", "SEQ_PL_WIFIGAME", "_FAST", 0, 0, 406, 0, 0},
    {"IPK IPG", "data/sound/gs_sound_data.sdat", "SEQ_GS_WIFIGAME", "_FAST", 0, 0, 406, 0, 0},

    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_15", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_16", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_17", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_18", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_19", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_20", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_21", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_22", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_23", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_24", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_25", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_26", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_27", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_28", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_29", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_30", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_31", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_32", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_33", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_34", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_35", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_36", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_37", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_38", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_39", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_40", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_41", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},
    {"AMCE", "data/Sound/sound_data.sdat", "SEQ_74", "_FINAL_LAP", 0, 0, 300, 0xFFFF, 64},

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

} // namespace
} // namespace ntdlib::ntr

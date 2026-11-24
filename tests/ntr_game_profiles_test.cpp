// SPDX-License-Identifier: MIT

// Tests of the DS games' profiles (ntdlib/ntr/game_profiles.h). The expected values come from the games' code at the
// addresses that src/ntr/game_profiles.cpp and src/ntr/game_profile_rows.h cite.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/snd/driver.h"

namespace
{

namespace ntr = ntdlib::ntr;
namespace snd = ntdlib::ntr::snd;
using ntr::GameProfile;
using ntr::GameProfileOf;
using snd::CommandId;
using xsflib::testing::Check;

// The size field of the driver's commands of a parameter of 2 bytes.
constexpr uint32_t kTwoBytes = 2u << 24;

// Whether `profile` sets nothing: the sequence plays as the archive gives it.
bool IsDefault(const GameProfile& profile)
{
    const ntr::GameSettings& settings = profile.settings;
    const auto unset = [](const std::optional<int16_t>& value)
    {
        return !value.has_value();
    };

    return std::ranges::all_of(settings.variables.local, unset) &&
           std::ranges::all_of(settings.variables.global, unset) && settings.channels.empty() &&
           settings.track_channels.empty() && !settings.reverb && !settings.effect && profile.commands.empty() &&
           profile.channel_locks.empty() && profile.arrangements.empty();
}

// Whether `reverb` holds the values of `expected`.
bool IsReverb(const std::optional<ntr::Reverb>& reverb, const ntr::Reverb& expected)
{
    return reverb && reverb->size == expected.size && reverb->pcm8 == expected.pcm8 &&
           reverb->sample_rate == expected.sample_rate && reverb->volume == expected.volume;
}

// Whether `channels` holds the entries of `expected`, in the same order.
bool AreTrackChannels(const std::vector<ntr::TrackChannels>& channels, const std::vector<ntr::TrackChannels>& expected)
{
    const auto same = [](const ntr::TrackChannels& a, const ntr::TrackChannels& b)
    {
        return a.tracks == b.tracks && a.channels == b.channels;
    };

    return std::ranges::equal(channels, expected, same);
}

// Pokémon Diamond's title gives the BGM's player 7 channels 0x7FFF and starts the capture units' reverb at volume 30.
// The ending starts it at volume 15 (0x0200516C and 0x0200433C). Platinum does the same (0x020053CC). The rows hold for
// every region of the games.
void TestPokemonTitleReverb()
{
    const GameProfile title = GameProfileOf("ADAE", "data/sound/sound_data.sdat", 0, "SEQ_TITLE00");
    const GameProfile ending = GameProfileOf("ADAE", "data/sound/sound_data.sdat", 0, "SEQ_BLD_ENDING");
    const GameProfile platinum = GameProfileOf("CPUE", "data/sound/pl_sound_data.sdat", 0, "SEQ_TITLE01");
    const GameProfile other_region = GameProfileOf("ADAJ", "data/sound/sound_data.sdat", 0, "SEQ_TITLE00");
    const GameProfile second_archive = GameProfileOf("ADAE", "data/sound/sound_data.sdat", 1, "SEQ_TITLE00");

    const std::map<uint32_t, uint16_t> kChannels = {{7, 0x7FFF}};
    Check(title.settings.channels == kChannels, "Diamond's title gives player 7 channels 0x7FFF");
    Check(IsReverb(title.settings.reverb, {.size = 0x1000, .pcm8 = false, .sample_rate = 16000, .volume = 30}),
          "Diamond's title starts the reverb at volume 30");
    Check(title.settings.track_channels.empty() && !title.settings.effect && title.commands.empty() &&
              title.arrangements.empty(),
          "Diamond's title sets nothing else");
    Check(IsReverb(ending.settings.reverb, {.size = 0x1000, .pcm8 = false, .sample_rate = 16000, .volume = 15}),
          "Diamond's ending starts the reverb at volume 15");
    Check(platinum.settings.channels == kChannels &&
              IsReverb(platinum.settings.reverb, {.size = 0x1000, .pcm8 = false, .sample_rate = 16000, .volume = 30}),
          "Platinum's title does the same in pl_sound_data.sdat");
    Check(other_region.settings.channels == kChannels, "the row holds for another region of Diamond");
    Check(IsDefault(second_archive), "the row names the first archive of its file alone");
}

// HeartGold's title gives player 7 channels 0x3FFF (0x02005AF8), and tracks 0 to 3 channels 0xA7FE (0x02005CC0).
void TestHeartGoldTitle()
{
    const GameProfile title = GameProfileOf("IPKE", "data/sound/gs_sound_data.sdat", 0, "SEQ_GS_POKEMON_THEME");

    Check(title.settings.channels == std::map<uint32_t, uint16_t>{{7, 0x3FFF}}, "HeartGold's player 7 takes 0x3FFF");
    Check(AreTrackChannels(title.settings.track_channels, {{.tracks = 0x000F, .channels = 0xA7FE}}),
          "HeartGold's tracks 0 to 3 take 0xA7FE");
    Check(!title.settings.reverb, "HeartGold starts no reverb");
}

// Blue Rescue Team's start-up starts the reverb with a buffer of 0x900 bytes at volume 45 (0x0200B1F8). Its BGM's
// function gives the five sequences of the table at 0x02086040 0xFFFA on every track. Every other BGM gets 0x8FFA on
// tracks 0 to 7 and 0x7000 on tracks 8 to 15 (0x0200A5F0).
void TestBlueRescueTeam()
{
    const GameProfile title = GameProfileOf("APHE", "sound.sbin", 0, "SND_BGM_M_SYS_TITLE_01");
    const GameProfile home = GameProfileOf("APHE", "sound.sbin", 0, "SND_BGM_M_HABITAT_HOME01");
    const GameProfile effect = GameProfileOf("APHE", "sound.sbin", 0, "SND_ME_M_LEVELUP");

    const ntr::Reverb kReverb = {.size = 0x900, .pcm8 = false, .sample_rate = 16000, .volume = 45};
    Check(IsReverb(title.settings.reverb, kReverb) && IsReverb(home.settings.reverb, kReverb) &&
              IsReverb(effect.settings.reverb, kReverb),
          "every sequence of sound.sbin plays with the reverb");
    Check(AreTrackChannels(title.settings.track_channels, {{.tracks = 0xFFFF, .channels = 0xFFFA}}),
          "the title's BGM takes 0xFFFA on every track");
    Check(AreTrackChannels(home.settings.track_channels,
                           {{.tracks = 0x00FF, .channels = 0x8FFA}, {.tracks = 0xFF00, .channels = 0x7000}}),
          "another BGM splits its tracks' channels");
    Check(effect.settings.track_channels.empty(), "a sequence other than a BGM keeps its tracks' channels");
    Check(title.settings.channels.empty(), "the archive's players keep their channels");
}

// Diamond's room sets the TV's music to volume 50 (script command 0x283, 0x0200488C). The call sets the player's level
// to -81 (0.1 dB).
void TestRoomVolume()
{
    const GameProfile tv = GameProfileOf("ADAE", "data/sound/sound_data.sdat", 0, "SEQ_TV_HOUSOU");

    const snd::CommandList kRoom = {
        {.id = CommandId::kPlayerParameter, .args = {0, 0x06, static_cast<uint32_t>(-81), 2}}};
    Check(tv.commands.empty(), "the TV's music starts at the archive's volume");
    Check(tv.arrangements.size() == 1 && tv.arrangements[0].suffix == "_ROOM" && tv.arrangements[0].variables.empty() &&
              tv.arrangements[0].commands == kRoom,
          "_ROOM sets the player's level to -81");
}

// Black's Victory Road plays tracks 0 to 6 and 9 (0x0202BF54), and Route 10 adds one track a zone. White's zone BGM
// function plays SEQ_BGM_C_08_W in place of SEQ_BGM_C_08_B (0x02013C1C). Chargestone Cave plays SEQ_BGM_D_02 6
// semitones lower at 215/256 of its tempo in zones 197 and 354 (0x0202B2CC).
void TestBlackAndWhiteZones()
{
    const GameProfile road = GameProfileOf("IRBO", "wb_sound_data.sdat", 0, "SEQ_BGM_D_CHAMPROAD");
    const GameProfile white_c08 = GameProfileOf("IRAO", "wb_sound_data.sdat", 0, "SEQ_BGM_C_08_W");
    const GameProfile white_c08_black = GameProfileOf("IRAO", "wb_sound_data.sdat", 0, "SEQ_BGM_C_08_B");
    const GameProfile cave = GameProfileOf("IRBO", "wb_sound_data.sdat", 0, "SEQ_BGM_D_02");

    const snd::CommandList kRoad = {
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x027F, 0x0A, 0}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0xFD80, 0x0A, 0xFFFF8000}},
    };
    Check(road.commands == kRoad, "Victory Road silences tracks 7, 8 and 10 to 15");
    Check(road.arrangements.size() == 9 && road.arrangements[0].suffix == "_ROUTE10_1" &&
              road.arrangements[8].suffix == "_ROUTE10_9",
          "Route 10's nine zones give arrangements");
    Check(white_c08.arrangements.size() == 1 && white_c08.arrangements[0].suffix == "_FAR",
          "White's Opelucid City plays SEQ_BGM_C_08_W from afar");
    Check(IsDefault(white_c08_black), "White has no row for SEQ_BGM_C_08_B");

    const snd::CommandList kLower = {
        {.id = CommandId::kPlayerParameter, .args = {0, 0x1A, 215, 2}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0xFCFF, 0x0C, static_cast<uint32_t>(-384)}},
    };
    Check(cave.arrangements.size() == 2 && cave.arrangements[1].suffix == "_LOWER2" &&
              cave.arrangements[1].commands == kLower,
          "_LOWER2 sets the tempo, then the pitch of every track but 8 and 9");
}

// Poké Transfer's minigame raises the tempo and the pitch of SEQ_BGM_PALPARK_GAME, from a/1/4/2, for a round's last
// 1200 frames (0x02019DF0 in the download-play program of Black and White, 0x02016258 in that of Black 2 and White 2).
void TestPalParkHurry()
{
    const GameProfile black = GameProfileOf("IRBO", "a/1/4/2", 0, "SEQ_BGM_PALPARK_GAME");
    const GameProfile white_2 = GameProfileOf("IRDO", "a/1/4/2", 0, "SEQ_BGM_PALPARK_GAME");
    const GameProfile box = GameProfileOf("IRBO", "a/1/4/0", 0, "SEQ_BGM_PALPARK_BOX");

    const snd::CommandList kHurry = {
        {.id = CommandId::kPlayerParameter, .args = {0, 0x1A, 285, 2}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0xFFFF, 0x0C, 85}},
    };
    for (const GameProfile* game : {&black, &white_2})
    {
        Check(game->commands.empty() && game->arrangements.size() == 1 && game->arrangements[0].suffix == "_HURRY" &&
                  game->arrangements[0].commands == kHurry,
              "_HURRY sets the tempo ratio to 285, then the pitch of every track to 85");
    }
    Check(IsDefault(box), "the box's piece has no row");
}

// New Super Mario Bros. sets the BGM's tempo ratio to 358 once a level's time reaches 100 (0x020A27A4 to 0x020A27C4).
// BGM_WORLD8 starts with tracks 1 and 8 muted while a flag of the save is clear (0x0204E7D8 to 0x0204E818). The red
// coin's effect writes global variable 1 with the coin's number less 1 (0x0204E0C8).
void TestNewSuperMarioBros()
{
    const GameProfile muteki = GameProfileOf("A2DE", "sound_data.sdat", 0, "BGM_MUTEKI");
    const GameProfile world8 = GameProfileOf("A2DE", "sound_data.sdat", 0, "BGM_WORLD8");
    const GameProfile coin = GameProfileOf("A2DE", "sound_data.sdat", 0, "SE_OBJ_GET_RED_COIN");

    const snd::CommandList kHurry = {{.id = CommandId::kPlayerParameter, .args = {0, 0x1A, 358, 2}}};
    Check(muteki.commands.empty() && muteki.arrangements.size() == 1 && muteki.arrangements[0].suffix == "_HURRY" &&
              muteki.arrangements[0].commands == kHurry,
          "_HURRY sets the tempo ratio to 358");

    const snd::CommandList kMuted = {{.id = CommandId::kMuteTrack, .args = {0, 0x0102, snd::kMuteNoStop}}};
    const snd::CommandList kFull = {{.id = CommandId::kMuteTrack, .args = {0, 0xFFFF, snd::kMuteOff}}};
    Check(world8.commands == kMuted, "BGM_WORLD8 starts with tracks 1 and 8 muted");
    Check(world8.arrangements.size() == 1 && world8.arrangements[0].suffix == "_FULL" &&
              world8.arrangements[0].commands == kFull,
          "_FULL unmutes every track");

    Check(coin.settings.variables.global[1] == 0 && !coin.settings.variables.global[0] &&
              !coin.settings.variables.local[1],
          "the first red coin writes 0 to global variable 1");
    bool coins = coin.arrangements.size() == 7;
    for (std::size_t i = 0; coins && i < coin.arrangements.size(); i++)
    {
        const std::map<uint8_t, int16_t> variables = {{17, static_cast<int16_t>(i + 1)}};
        coins = coin.arrangements[i].suffix == "_COIN" + std::to_string(i + 2) &&
                coin.arrangements[i].variables == variables && coin.arrangements[i].commands.empty();
    }
    Check(coins, "coins 2 to 8 write 1 to 7");
}

// Mario Kart DS's SEQARC_0_193 takes its program from local variable 0, and SEQARC_0_290 pans from local variable 2
// by steps of local variable 1.
void TestMarioKartVariables()
{
    const GameProfile voice = GameProfileOf("AMCE", "data/Sound/sound_data.sdat", 0, "SEQARC_0_193");
    const GameProfile sweep = GameProfileOf("AMCE", "data/Sound/sound_data.sdat", 0, "SEQARC_1_290");

    Check(voice.settings.variables.local[0] == 0, "the voice takes program 0");
    Check(voice.arrangements.size() == 12 && voice.arrangements[0].suffix == "_PROGRAM1" &&
              voice.arrangements[11].suffix == "_PROGRAM12" &&
              voice.arrangements[11].variables == std::map<uint8_t, int16_t>{{0, 12}},
          "programs 1 to 12 give arrangements");
    Check(
        sweep.settings.variables.local[1] == 0 && sweep.settings.variables.local[2] == 64 && sweep.arrangements.empty(),
        "the sweep plays at the centre without steps");
}

// Mario Kart DS's final lap starts the course's BGM again at a tempo ratio of 300 (0x02106828). From the next frame
// on, the race's sound frame sets the pitch of every track to 64, a semitone up (0x0210FE58 to 0x0210FE90).
void TestMarioKartFinalLap()
{
    const GameProfile first = GameProfileOf("AMCE", "data/Sound/sound_data.sdat", 0, "SEQ_15");
    const GameProfile last = GameProfileOf("AMCE", "data/Sound/sound_data.sdat", 0, "SEQ_74");
    const GameProfile title = GameProfileOf("AMCE", "data/Sound/sound_data.sdat", 0, "SEQ_75");

    const snd::CommandList kFinalLap = {
        {.id = CommandId::kPlayerParameter, .args = {0, 0x1A, 300, 2}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0xFFFF, 0x0C, 64}},
    };
    Check(first.commands.empty() && first.arrangements.size() == 1 && first.arrangements[0].suffix == "_FINAL_LAP" &&
              first.arrangements[0].commands == kFinalLap,
          "_FINAL_LAP sets the tempo ratio to 300, then every track a semitone up");
    Check(last.arrangements.size() == 1 && last.arrangements[0].commands == kFinalLap, "SEQ_74 has the final lap too");
    Check(title.arrangements.empty() && title.commands.empty(), "the title's music has no final lap");
}

// New Super Mario Bros.' boot starts the SDK's output effect in mode 1 (0x0204F1DC, through 0x02060BEC), and so does
// Mario Kart DS's (0x0212F690): a buffer of 4,096 bytes in two parts, PCM16 at 32,000 Hz. The effect changes the whole
// output. Every sequence of every archive of the games therefore plays through it.
void TestOutputEffect()
{
    const GameProfile race = GameProfileOf("AMCE", "data/Sound/sound_data.sdat", 0, "SEQ_15");
    const GameProfile mario = GameProfileOf("A2DE", "sound_data.sdat", 0, "BGM_TRIDE");
    const GameProfile minigame = GameProfileOf("A2DE", "mgvs_sound_data.sdat", 0, "NCS_BGM_PERFECT");
    const GameProfile diamond = GameProfileOf("ADAE", "data/sound/sound_data.sdat", 0, "SEQ_TITLE00");

    const auto is_games_effect = [](const GameProfile& profile)
    {
        const std::optional<ntr::OutputEffect>& effect = profile.settings.effect;
        return effect && effect->mode == ntr::OutputMode::kSurround && effect->size == 0x1000 && !effect->pcm8 &&
               effect->sample_rate == 32000 && effect->interval == 2 && !profile.settings.reverb;
    };
    Check(is_games_effect(race), "Mario Kart DS plays through the surround");
    Check(race.arrangements.size() == 1 && race.arrangements[0].suffix == "_FINAL_LAP",
          "a course's BGM keeps its final lap");
    Check(is_games_effect(mario), "New Super Mario Bros. plays through the surround");
    Check(is_games_effect(minigame), "so does its second archive");
    Check(!diamond.settings.effect, "Diamond starts no output effect");
}

// Spirit Tracks' BGM manager plays SEQ_36 as SEQ_35 in its mixer's state 6, and SEQ_40 as SEQ_39 in state 7 (0x0206CBC8
// to 0x0206CF44, the table at 0x020AA534). Those states set the player's levels to -52 and -47 (0.1 dB), the tracks'
// faders and a tempo ratio of 253. The manager writes local variable 0 with 0 after a start of SEQ_39 (0x0206CF50), and
// after a start of SEQ_37 that follows SEQ_39 (0x0206CF70). A new start of SEQ_27 takes state 20 (0x0206CFF0). The
// state sets its tracks' faders. The rows hold for the USA and European releases (BKIE, BKIP).
void TestSpiritTracks()
{
    constexpr std::string_view kArchive = "SoundData/final_sound_data.sdat";
    const GameProfile seq36 = GameProfileOf("BKIE", kArchive, 0, "SEQ_36");
    const GameProfile seq40 = GameProfileOf("BKIP", kArchive, 0, "SEQ_40");
    const GameProfile seq37 = GameProfileOf("BKIE", kArchive, 0, "SEQ_37");

    const snd::CommandList kState6 = {
        {.id = CommandId::kPlayerParameter, .args = {0, 0x06, static_cast<uint32_t>(-52), 2}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x0001, 0x0A, static_cast<uint32_t>(-47)}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x01E0, 0x0A, 0xFFFF8000}},
        {.id = CommandId::kPlayerParameter, .args = {0, 0x1A, 253, 2}},
    };
    Check(seq36.commands == kState6 && seq36.arrangements.empty(), "SEQ_36 plays SEQ_35's level in state 6");

    const snd::CommandList kState7 = {
        {.id = CommandId::kPlayerParameter, .args = {0, 0x06, static_cast<uint32_t>(-47), 2}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x0082, 0x0A, 0xFFFF8000}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x0104, 0x0A, static_cast<uint32_t>(-30)}},
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x0008, 0x0A, static_cast<uint32_t>(-6)}},
    };
    Check(seq40.commands == kState7, "SEQ_40 plays SEQ_39 in state 7");
    Check(seq40.settings.variables.local[0] == 0 && seq40.arrangements.size() == 1 &&
              seq40.arrangements[0].suffix == "_INTRO" &&
              seq40.arrangements[0].variables == std::map<uint8_t, int16_t>{{0, -1}},
          "SEQ_40 starts at section 0, and _INTRO plays the intro");

    Check(seq37.commands.empty() && !seq37.settings.variables.local[0] && seq37.arrangements.size() == 1 &&
              seq37.arrangements[0].suffix == "_RESUME" &&
              seq37.arrangements[0].variables == std::map<uint8_t, int16_t>{{0, 0}},
          "SEQ_37 starts with its intro, and _RESUME at section 0");
    Check(IsDefault(GameProfileOf("BKIE", kArchive, 0, "SEQ_35")), "SEQ_35 plays as the archive gives it");

    const snd::CommandList kState20 = {
        {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0xC004, 0x0A, 0xFFFF8000}}};
    Check(GameProfileOf("BKIE", kArchive, 0, "SEQ_27").commands == kState20,
          "SEQ_27 silences tracks 2, 14 and 15 at the archive's level");
}

// The player's level, the tempo's ratio and the 16 tracks' faders that a list of commands sets.
struct Mix
{
    std::optional<int32_t> level;
    std::optional<uint32_t> tempo_ratio;
    std::array<std::optional<int32_t>, 16> faders;
};

Mix MixOf(const snd::CommandList& commands)
{
    Mix mix;
    for (const snd::Command& command : commands)
    {
        if (command.id == CommandId::kPlayerParameter && command.args[1] == 0x06)
        {
            mix.level = static_cast<int32_t>(command.args[2]);
        }
        else if (command.id == CommandId::kPlayerParameter && command.args[1] == 0x1A)
        {
            mix.tempo_ratio = command.args[2];
        }
        else if (command.id == CommandId::kTrackParameter && command.args[2] == 0x0A)
        {
            for (std::size_t track = 0; track < mix.faders.size(); track++)
            {
                if ((command.args[1] >> track & 1) != 0)
                {
                    mix.faders[track] = static_cast<int32_t>(command.args[3]);
                }
            }
        }
    }

    return mix;
}

// SEQ_39's intro: SEQ_39's handler in the mixer (0x0206B528) holds state 1's volume and tempo, with every track at 127,
// until the player's tick counter reaches 504. The mixer then fades to state 18 over 15 frames (0x0206A9FC). The
// values below are the commands that the game's code sent in each frame of the fade, on SEQ_39 started after SEQ_37:
// steps 1 to 15. The first frame that read a count of 504 or more sent step 0, the intro's values again. The game's
// frames came in every second frame of the screen.
void TestSpiritTracksIntro()
{
    constexpr std::string_view kArchive = "SoundData/final_sound_data.sdat";
    const GameProfile seq39 = GameProfileOf("BKIE", kArchive, 0, "SEQ_39");
    const GameProfile seq40 = GameProfileOf("BKIP", kArchive, 0, "SEQ_40");
    Check(seq39.arrangements.size() == 1 && seq39.arrangements[0].suffix == "_INTRO" &&
              seq39.arrangements[0].tick_commands.size() == 1 && seq39.arrangements[0].tick_commands[0].tick == 504 &&
              seq39.arrangements[0].tick_commands[0].video_frames == 2 &&
              seq39.arrangements[0].tick_commands[0].frames.size() == 15,
          "SEQ_39_INTRO fades in 15 of the game's frames, each of two frames of the screen, once 504 ticks have run");
    if (seq39.arrangements.size() != 1 || seq39.arrangements[0].tick_commands.size() != 1 ||
        seq39.arrangements[0].tick_commands[0].frames.size() != 15)
    {
        return;
    }

    const Mix intro = MixOf(seq39.arrangements[0].commands);
    Check(intro.level == -42 && intro.tempo_ratio == 256u &&
              std::ranges::all_of(intro.faders, [](const std::optional<int32_t>& f) { return f == 0; }),
          "SEQ_39_INTRO plays its intro at state 1's level and tempo, with every track at full volume");

    // Each frame's level, tempo, and faders of tracks 0, 3 and 6. Tracks 2, 4 and 8 fade as track 0 does. The others
    // stay at 0.
    constexpr std::array<int32_t, 15> kLevels = {-42, -42, -41, -41, -41, -40, -40, -40,
                                                 -40, -40, -40, -39, -39, -39, -38};
    constexpr std::array<uint32_t, 15> kTempos = {256, 256, 256, 256, 255, 255, 255, 255,
                                                  255, 254, 254, 254, 254, 254, 253};
    constexpr std::array<int32_t, 15> kTrack0 = {-6,  -12, -19,  -26,  -35,  -43,  -54,   -65,
                                                 -79, -94, -114, -138, -175, -230, -32768};
    constexpr std::array<int32_t, 15> kTrack3 = {-1, -3, -5, -7, -9, -11, -13, -15, -17, -20, -22, -24, -26, -29, -32};
    constexpr std::array<int32_t, 15> kTrack6 = {-2,  -5,  -8,  -11, -14, -17, -20, -23,
                                                 -27, -31, -34, -38, -42, -47, -52};
    bool same = true;
    const std::vector<snd::CommandList>& frames = seq39.arrangements[0].tick_commands[0].frames;
    for (std::size_t step = 0; step < frames.size(); step++)
    {
        const Mix mix = MixOf(frames[step]);
        for (std::size_t track = 0; track < mix.faders.size(); track++)
        {
            const bool like_track0 = track == 0 || track == 2 || track == 4 || track == 8;
            const int32_t expected = like_track0  ? kTrack0[step]
                                     : track == 3 ? kTrack3[step]
                                     : track == 6 ? kTrack6[step]
                                                  : 0;
            same = same && mix.faders[track] == expected;
        }
        same = same && mix.level == kLevels[step] && mix.tempo_ratio == kTempos[step];
    }
    Check(same, "SEQ_39_INTRO's fade to state 18 matches the game's commands frame by frame");

    // SEQ_40 plays SEQ_39 in state 7. Its intro fades to state 7.
    const Mix last = seq40.arrangements.empty() || seq40.arrangements[0].tick_commands.empty()
                         ? Mix{}
                         : MixOf(seq40.arrangements[0].tick_commands[0].frames.back());
    Check(last.level == -47 && last.tempo_ratio == 256u && last.faders[1] == -32768 && last.faders[7] == -32768 &&
              last.faders[2] == -30 && last.faders[8] == -30 && last.faders[3] == -6 && last.faders[0] == 0,
          "SEQ_40_INTRO fades to state 7");

    Check(GameProfileOf("BKIE", kArchive, 0, "SEQ_37").arrangements[0].tick_commands.empty(),
          "SEQ_37's arrangements have no fade");
}

// The run (the USA release on ntdlib's DS machine) started SEQ_18 as the game does with nothing playing, through
// 0x0206C988 in state 0, and called the train sound update (overlay 26, 0x0212E584) in each frame at a speed held at 0,
// the train assumed to stand at a station. The intro played at the level and tempo of state 1 with every track at full
// volume. The mixer sent the mutes of state 2 from the start, and those of state 3 in the 61st frame after it. Once 456
// ticks had run, it faded to state 3 in 85 frames. Tracks 2, 3, 4, 8, 10, 11, 12 and 13 faded as track 0 did. The
// others stayed at 0.
void TestSpiritTracksTrainIntro()
{
    const GameProfile seq18 = GameProfileOf("BKIE", "SoundData/final_sound_data.sdat", 0, "SEQ_18");
    const auto intro = std::ranges::find_if(
        seq18.arrangements, [](const ntr::ProfileArrangement& arrangement) { return arrangement.suffix == "_INTRO"; });

    if (intro == seq18.arrangements.end() || intro->tick_commands.size() != 2 ||
        intro->tick_commands[0].frames.size() != 85)
    {
        Check(false, "SEQ_18_INTRO has a fade of 85 frames and a list of mutes");
        return;
    }

    const ntr::TickCommands& fade = intro->tick_commands[0];
    const ntr::TickCommands& standing = intro->tick_commands[1];
    const Mix mix = MixOf(intro->commands);
    Check(fade.tick == 456 && fade.video_frames == 2, "SEQ_18_INTRO fades once 456 ticks have run");
    Check(mix.level == -36 && mix.tempo_ratio == 256u &&
              std::ranges::all_of(mix.faders, [](const std::optional<int32_t>& f) { return f == 0; }),
          "SEQ_18_INTRO plays its intro at state 1's level and tempo, with every track at full volume");
    Check(intro->commands.back() == snd::Command{.id = CommandId::kMuteTrack, .args = {0, 0xFFFF, snd::kMuteOff}},
          "SEQ_18_INTRO starts with every track unmuted, as state 2 sets");
    const snd::CommandList kStanding = {{.id = CommandId::kMuteTrack, .args = {0, 0x02E2, snd::kMuteOff}},
                                        {.id = CommandId::kMuteTrack, .args = {0, 0x3101, snd::kMuteStop}}};
    Check(standing.tick == 0 && standing.video_frames == 2 && standing.frames.size() == 61 &&
              standing.frames.back() == kStanding &&
              std::all_of(standing.frames.begin(), standing.frames.end() - 1,
                          [](const snd::CommandList& frame) { return frame.empty(); }),
          "SEQ_18_INTRO mutes state 3's tracks in the 61st frame after the start");

    constexpr std::array<int32_t, 85> kLevels = {
        -36, -37, -37, -38, -38, -39, -39, -40, -40, -41, -41, -42, -42, -43, -43, -44, -45, -45, -46, -46, -47, -47,
        -48, -48, -49, -49, -50, -50, -51, -51, -52, -52, -53, -54, -54, -56, -56, -57, -57, -58, -58, -59, -59, -60,
        -60, -62, -62, -63, -63, -64, -65, -65, -67, -67, -68, -68, -69, -69, -71, -71, -72, -72, -73, -73, -75, -75,
        -76, -78, -78, -79, -79, -81, -81, -82, -82, -84, -84, -85, -85, -87, -87, -89, -89, -90, -92};
    constexpr std::array<uint32_t, 85> kTempos = {
        256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 253, 253,
        253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 251, 251, 251, 251, 251, 251, 251, 251, 251, 251, 251, 251, 251, 251, 250};
    constexpr std::array<int32_t, 85> kTrack0 = {
        -1,   -1,   -3,   -3,   -5,   -6,   -7,   -8,   -9,   -10,  -12,  -12,  -14,  -15,  -17,  -17,  -19,
        -20,  -22,  -23,  -24,  -25,  -27,  -28,  -30,  -31,  -33,  -34,  -36,  -37,  -39,  -40,  -42,  -43,
        -46,  -47,  -49,  -51,  -53,  -54,  -57,  -58,  -61,  -62,  -65,  -67,  -70,  -71,  -74,  -76,  -79,
        -81,  -85,  -86,  -90,  -92,  -96,  -98,  -103, -105, -110, -112, -117, -120, -125, -128, -134, -138,
        -145, -148, -156, -161, -170, -175, -186, -192, -205, -212, -230, -240, -265, -281, -325, -361, -32768};
    constexpr std::array<int32_t, 85> kTrack1 = {
        0,   -1,  -1,  -1,  -2,  -2,  -3,  -3,  -3,  -4,  -4,  -5,  -6,  -6,  -6,  -6,  -7,  -8,  -8,  -9,  -9,  -9,
        -10, -10, -11, -12, -12, -12, -13, -13, -14, -14, -15, -16, -16, -17, -17, -17, -18, -18, -19, -20, -20, -21,
        -22, -22, -23, -23, -23, -24, -24, -25, -26, -26, -27, -27, -28, -29, -29, -30, -31, -31, -32, -32, -33, -34,
        -34, -35, -36, -36, -37, -37, -38, -39, -39, -40, -41, -41, -42, -42, -43, -45, -45, -46, -47};
    constexpr std::array<int32_t, 85> kTrack9 = {
        0,   -1,  -1,  -1,  -2,  -3,  -3,  -3,  -4,  -5,  -6,  -6,  -6,  -7,  -7,  -8,  -9,  -9,  -9,  -10, -11, -12,
        -12, -12, -13, -13, -14, -15, -16, -16, -17, -17, -18, -18, -19, -20, -20, -21, -22, -23, -23, -23, -24, -25,
        -25, -26, -27, -28, -28, -29, -30, -30, -31, -32, -33, -33, -34, -35, -36, -36, -37, -38, -38, -39, -40, -41,
        -41, -42, -43, -45, -45, -46, -47, -47, -48, -49, -51, -51, -52, -53, -54, -54, -56, -57, -58};
    constexpr uint16_t kLikeTrack0 = 0x3D1D;
    bool same = true;
    for (std::size_t step = 0; step < fade.frames.size(); step++)
    {
        const Mix frame = MixOf(fade.frames[step]);
        for (std::size_t track = 0; track < frame.faders.size(); track++)
        {
            const int32_t expected = (kLikeTrack0 >> track & 1) != 0 ? kTrack0[step]
                                     : track == 1                    ? kTrack1[step]
                                     : track == 9                    ? kTrack9[step]
                                                                     : 0;
            same = same && frame.faders[track] == expected;
        }
        same = same && frame.level == kLevels[step] && frame.tempo_ratio == kTempos[step];
    }
    Check(same, "SEQ_18_INTRO's fade to state 3 matches the game's commands frame by frame");
}

// The mix that a sequence's base commands and then `arrangement`'s commands leave: the player's level and tempo ratio,
// each track's fader (0 where none is sent) and each track's mute mode (0, not muted, where none is sent).
struct Steady
{
    std::optional<int32_t> level;
    std::optional<uint32_t> tempo_ratio;
    std::array<int32_t, 16> faders = {};
    std::array<int32_t, 16> mutes = {};

    friend bool operator==(const Steady&, const Steady&) = default;
};

Steady SteadyOf(const GameProfile& profile, std::string_view suffix)
{
    snd::CommandList commands = profile.commands;
    for (const ntr::ProfileArrangement& arrangement : profile.arrangements)
    {
        if (arrangement.suffix == suffix)
        {
            commands.insert(commands.end(), arrangement.commands.begin(), arrangement.commands.end());
        }
    }

    Steady steady;
    const Mix mix = MixOf(commands);
    steady.level = mix.level;
    steady.tempo_ratio = mix.tempo_ratio;
    for (std::size_t track = 0; track < steady.faders.size(); track++)
    {
        steady.faders[track] = mix.faders[track].value_or(0);
    }
    for (const snd::Command& command : commands)
    {
        if (command.id == CommandId::kMuteTrack)
        {
            for (std::size_t track = 0; track < steady.mutes.size(); track++)
            {
                if ((command.args[1] >> track & 1) != 0)
                {
                    steady.mutes[track] = static_cast<int32_t>(command.args[2]);
                }
            }
        }
    }

    return steady;
}

// A state of a Spirit Tracks piece, and the mix that the game's code left on the player in a run.
struct SceneCase
{
    std::string_view label;
    std::string_view suffix;
    Steady steady;
};

constexpr int32_t kOff = -32768;

// Spirit Tracks' scene states (game_profiles.cpp and game_profile_rows.h cite the code). The expected mixes are the
// last values of each parameter that the game's code sent in runs of the USA release on ntdlib's DS machine. Each run
// booted to the title, started the piece at 8 s through the BGM manager's start (0x0206C988) in the row's state, or set
// the state at once through the mixer's 0x0206B32C after a start that the manager gives another state, and read the
// commands up to 30 s. The runs of SEQ_65 set the mixer's height (0x9C of 0x0225FCC0) to 0xACD0, 0xE6A9, 0x14759 and
// 0x1A669 after the start. The runs of SEQ_18 turned off its intro handler (0x0206B3DC patched to an unconditional
// branch). A row whose level is unset keeps the archive's. Where a run and the profile differ only between an unset
// value and its default (a tempo ratio of 256, a level of 0 dB), the row gives the profile's form.
void TestSpiritTracksScenes()
{
    constexpr std::string_view kArchive = "SoundData/final_sound_data.sdat";
    const std::vector<SceneCase> cases = {
        {"SEQ_18",
         "",
         {.level = -36,
          .tempo_ratio = {},
          .faders = {0, -12, 0, 0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
          .mutes = {0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_18",
         "_RESUME",
         {.level = -36,
          .tempo_ratio = {},
          .faders = {0, -12, 0, 0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
          .mutes = {0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_18",
         "_SLOW",
         {.level = -63,
          .tempo_ratio = 256,
          .faders = {kOff, -47, kOff, 0, 0, kOff, 0, 0, kOff, -30, 0, 0, kOff, kOff, 0, 0}}},
        {"SEQ_18",
         "_STOPPED",
         {.level = -92,
          .tempo_ratio = 250,
          .faders = {kOff, -47, kOff, kOff, kOff, 0, 0, 0, kOff, -58, kOff, kOff, kOff, kOff, 0, 0},
          .mutes = {3, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 3, 3, 0, 0}}},
        {"SEQ_23",
         "_STOPPED",
         {.level = -102,
          .tempo_ratio = 252,
          .faders = {kOff, 0, 0, 0, kOff, kOff, 0, 0, 0, kOff, kOff, 0, 0, 0, 0, 0}}},
        {"SEQ_27",
         "",
         {.level = {}, .tempo_ratio = {}, .faders = {0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, kOff, kOff}}},
        {"SEQ_27",
         "_PHASE2",
         {.level = -5, .tempo_ratio = 256, .faders = {0, 0, 0, 0, 0, 0, 0, 0, kOff, kOff, 0, kOff, kOff, kOff, 0, 0}}},
        {"SEQ_80",
         "",
         {.level = -12, .tempo_ratio = {}, .faders = {0, kOff, 0, kOff, 0, kOff, 0, 0, kOff, 0, kOff, 0, 0, 0, 0, 0}}},
        {"SEQ_80",
         "_CLOSER",
         {.level = -12, .tempo_ratio = 263, .faders = {0, -28, 0, -28, 0, -28, 0, 0, -28, 0, -28, 0, 0, 0, 0, 0}}},
        {"SEQ_80",
         "_CLOSEST",
         {.level = 0, .tempo_ratio = 278, .faders = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_87",
         "",
         {.level = {}, .tempo_ratio = {}, .faders = {kOff, 0, 0, 0, 0, kOff, 0, 0, 0, 0, kOff, 0, kOff, 0, 0, 0}}},
        {"SEQ_87",
         "_PHASE2",
         {.level = -30, .tempo_ratio = 263, .faders = {0, 0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, kOff, 0, 0}}},
        {"SEQ_88",
         "_PHASE2",
         {.level = -30, .tempo_ratio = 263, .faders = {0, 0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, kOff, 0, 0}}},
        {"SEQ_96", "", {.level = {}, .tempo_ratio = {}, .faders = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, kOff, 0, 0}}},
        {"SEQ_96",
         "_PHASE2",
         {.level = -5, .tempo_ratio = 256, .faders = {0, kOff, kOff, 0, 0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_97",
         "_PHASE2",
         {.level = -5, .tempo_ratio = 256, .faders = {0, kOff, kOff, 0, 0, 0, kOff, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_153",
         "",
         {.level = {}, .tempo_ratio = {}, .faders = {0, 0, 0, 0, 0, 0, 0, kOff, kOff, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_153",
         "_PHASE2",
         {.level = -17,
          .tempo_ratio = 256,
          .faders = {0, kOff, kOff, kOff, kOff, kOff, -88, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_6",
         "",
         {.level = {}, .tempo_ratio = {}, .faders = {0, 0, 0, 0, kOff, kOff, 0, kOff, kOff, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_6",
         "_FULL",
         {.level = -6, .tempo_ratio = 256, .faders = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"SEQ_65",
         "",
         {.level = -47,
          .tempo_ratio = 255,
          .faders = {0, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff}}},
        {"SEQ_65",
         "_HEIGHT2",
         {.level = -47,
          .tempo_ratio = 256,
          .faders = {0, kOff, 0, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff}}},
        {"SEQ_65",
         "_HEIGHT3",
         {.level = -43,
          .tempo_ratio = 256,
          .faders = {kOff, 0, 0, kOff, kOff, 0, kOff, 0, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff}}},
        {"SEQ_65",
         "_HEIGHT4",
         {.level = -32,
          .tempo_ratio = 257,
          .faders = {kOff, 0, 0, 0, 0, 0, kOff, 0, kOff, kOff, kOff, kOff, kOff, kOff, kOff, kOff}}},
    };

    for (const SceneCase& scene : cases)
    {
        const Steady steady = SteadyOf(GameProfileOf("BKIE", kArchive, 0, scene.label), scene.suffix);
        Check(steady == scene.steady,
              std::string(scene.label) + std::string(scene.suffix) + " plays the mix that the game's code sets");
    }

    const GameProfile seq18 = GameProfileOf("BKIP", kArchive, 0, "SEQ_18");
    std::vector<std::string> suffixes;
    for (const ntr::ProfileArrangement& arrangement : seq18.arrangements)
    {
        suffixes.push_back(arrangement.suffix);
    }
    Check(suffixes == std::vector<std::string>{"_RESUME", "_INTRO", "_SLOW", "_STOPPED"},
          "SEQ_18 has _RESUME, then the train's states");
}

// Spirit Tracks' cutscene d20_03 starts SEQ_279 to SEQ_282 while stream 120 holds channels 6 and 7 (the preparation at
// 0x0206D2C0). Every track of the four takes channels 0xFF3F in both releases. In four other cutscenes a stream's
// preparation locks channels 6 and 7 while a sequence plays, on the driver frames of runs of the game's code. SEQ_185
// starts after the stream of its cutscene has stopped, and keeps the default profile.
void TestSpiritTracksStream()
{
    constexpr std::string_view kArchive = "SoundData/final_sound_data.sdat";
    const std::vector<ntr::TrackChannels> kUnderStream = {{.tracks = 0xFFFF, .channels = 0xFF3F}};
    const std::map<uint32_t, uint16_t> kArchiveChannels = {{0, 0}};
    const std::map<std::string_view, std::vector<ntr::ChannelLock>> kLocks = {
        {"SEQ_184", {{.frame = 526, .channels = 0xC0}}},
        {"SEQ_194", {{.frame = 1875, .channels = 0xC0}}},
        {"SEQ_251", {{.frame = 5104, .channels = 0xC0}}},
        {"SEQ_304",
         {{.frame = 0, .channels = 0xC0},
          {.frame = 6, .channels = 0xC0, .unlock = true},
          {.frame = 256, .channels = 0xC0}}},
    };

    for (const std::string_view game : {"BKIE", "BKIP"})
    {
        for (const std::string_view label : {"SEQ_279", "SEQ_280", "SEQ_281", "SEQ_282"})
        {
            const GameProfile profile = GameProfileOf(game, kArchive, 0, label);
            Check(AreTrackChannels(profile.settings.track_channels, kUnderStream) &&
                      profile.settings.channels == kArchiveChannels && profile.commands.empty() &&
                      profile.channel_locks.empty() && profile.arrangements.empty(),
                  std::string(game) + "'s " + std::string(label) + " leaves channels 6 and 7 to the stream");
        }

        for (const auto& [label, locks] : kLocks)
        {
            const GameProfile profile = GameProfileOf(game, kArchive, 0, label);
            Check(
                profile.channel_locks == locks && profile.settings.track_channels.empty() &&
                    profile.settings.channels.empty() && profile.commands.empty() && profile.arrangements.empty(),
                std::string(game) + "'s " + std::string(label) + " gives channels 6 and 7 to the stream on its frames");
        }

        Check(IsDefault(GameProfileOf(game, kArchive, 0, "SEQ_185")),
              std::string(game) + "'s SEQ_185 plays as the archive gives it");
    }
}

// Phantom Hourglass at sea (overlay 15's music object): SEQ_101_STOPPED plays the mode of a ship at rest, and SEQ_103
// plays without track 7. The expected mixes are the mixes that the object's code left on the players in runs of the
// USA release on ntdlib's DS machine. The runs moved the object's code and tables into free memory at the title, set
// its inputs and called its frame in each game frame: the ship at rest, the ship sailing, and the fog with and
// without the Ghost Ship's volume.
void TestPhantomHourglassSea()
{
    constexpr std::string_view kArchive = "SoundData/final_sound_data.sdat";
    Steady stopped{.level = {}, .tempo_ratio = 250};
    stopped.faders = {kOff, -60, kOff, -60, kOff, -60, kOff, kOff, -60, -60, kOff, 0, 0, 0, 0, 0};
    stopped.mutes = {3, 0, 3, 0, 3, 0, 3, 3, 0, 0, 3, 0, 0, 0, 0, 0};
    const GameProfile seq101 = GameProfileOf("AZEE", kArchive, 0, "SEQ_101");
    Check(SteadyOf(seq101, "") == Steady{}, "SEQ_101 plays the sailing mix of the archive");
    Check(SteadyOf(seq101, "_STOPPED") == stopped, "SEQ_101_STOPPED plays the mix of a ship at rest");

    Steady fog;
    fog.faders[7] = kOff;
    Check(SteadyOf(GameProfileOf("AZEE", kArchive, 0, "SEQ_103"), "") == fog, "SEQ_103 plays without track 7");
    Check(SteadyOf(GameProfileOf("AZEE", kArchive, 0, "SEQ_103"), "_GHOST_SHIP") == Steady{},
          "SEQ_103_GHOST_SHIP plays track 7 at full volume");
    Check(IsDefault(GameProfileOf("AZEE", kArchive, 0, "SEQ_104")), "SEQ_104 plays as the archive gives it");
    Check(IsDefault(GameProfileOf("AZEE", kArchive, 0, "SEQ_167")), "SEQ_167's lower levels have no arrangement");
}

// A sequence, a file or a game that no row names plays as the archive gives it.
void TestNoProfile()
{
    Check(IsDefault(GameProfileOf("ADAE", "data/sound/sound_data.sdat", 0, "SEQ_TOWN01_D")), "an unlisted sequence");
    Check(IsDefault(GameProfileOf("ADAE", "data/sound/other.sdat", 0, "SEQ_TITLE00")), "an unlisted file");
    Check(IsDefault(GameProfileOf("YFEE", "data/sound/sound_data.sdat", 0, "SEQ_TITLE00")), "an unlisted game");
    Check(IsDefault(GameProfileOf("", "", 0, "SEQ_TITLE00")), "a loose archive");
}

// Four Swords Anniversary Edition gives every player all 16 channels and writes no variable. The table has no rows for
// the game. Its sequences play as the archive gives them. That includes the jingles on players 11 and 12.
void TestFourSwords()
{
    for (const std::string_view game : {"KQ9E", "KQ9V"})
    {
        for (const std::string_view label : {"BGM_SND400", "BGM_SND419", "BGM_SND423", "BGM_SND425", "BGM_SND426"})
        {
            Check(IsDefault(GameProfileOf(game, "knife_sound.sdat", 0, label)),
                  std::string(label) + " in " + std::string(game) + " plays as the archive gives it");
        }
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("DS games' profiles",
                                    []
                                    {
                                        TestPokemonTitleReverb();
                                        TestHeartGoldTitle();
                                        TestBlueRescueTeam();
                                        TestRoomVolume();
                                        TestBlackAndWhiteZones();
                                        TestPalParkHurry();
                                        TestNewSuperMarioBros();
                                        TestMarioKartVariables();
                                        TestMarioKartFinalLap();
                                        TestOutputEffect();
                                        TestSpiritTracks();
                                        TestSpiritTracksIntro();
                                        TestSpiritTracksTrainIntro();
                                        TestSpiritTracksScenes();
                                        TestSpiritTracksStream();
                                        TestPhantomHourglassSea();
                                        TestNoProfile();
                                        TestFourSwords();
                                    });
}

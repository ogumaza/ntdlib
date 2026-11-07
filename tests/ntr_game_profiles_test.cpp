// SPDX-License-Identifier: MIT

// Tests of the DS games' profiles (ntdlib/ntr/game_profiles.h). The expected values come from the games' code at the
// addresses that src/ntr/game_profiles.cpp cites, and from the runs of the games' code on ntdlib's DS machine that its
// comments give.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
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
           profile.arrangements.empty();
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

// Diamond's room sets the TV's music to volume 50 (script command 0x283, 0x0200488C). A run of the game's code sent
// the player's level of -81 (0.1 dB).
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

    const snd::CommandList kMuted = {{.id = CommandId::kMuteTrack, .args = {0, 0x0102, 1}}};
    const snd::CommandList kFull = {{.id = CommandId::kMuteTrack, .args = {0, 0xFFFF, 0}}};
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

// A sequence, a file or a game that no row names plays as the archive gives it.
void TestNoProfile()
{
    Check(IsDefault(GameProfileOf("ADAE", "data/sound/sound_data.sdat", 0, "SEQ_TOWN01_D")), "an unlisted sequence");
    Check(IsDefault(GameProfileOf("ADAE", "data/sound/other.sdat", 0, "SEQ_TITLE00")), "an unlisted file");
    Check(IsDefault(GameProfileOf("YFEE", "data/sound/sound_data.sdat", 0, "SEQ_TITLE00")), "an unlisted game");
    Check(IsDefault(GameProfileOf("", "", 0, "SEQ_TITLE00")), "a loose archive");
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
                                        TestNewSuperMarioBros();
                                        TestMarioKartVariables();
                                        TestOutputEffect();
                                        TestNoProfile();
                                    });
}

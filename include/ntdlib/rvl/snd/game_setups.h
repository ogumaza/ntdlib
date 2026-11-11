// SPDX-License-Identifier: MIT

// What some Wii games set up for their sounds beyond what the sound archive gives: the tracks that a sound starts
// with, the later sets of tracks that the game gives it, the values that the game writes to its variables and its
// sends. A program finds each sound by its label. The addresses are in the USA release's main.dol unless a comment
// names another file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ntdlib::rvl::snd
{

// The SHA-1 of each game's main.dol. A channel's main program is content 1 of the title. Each channel stores it
// uncompressed. Pokémon Rumble's is content 1 of the WiiWare title after LZ77 decompression.
inline constexpr char kWiiSportsSha1[] = "8bb422971b88b5551a37de98db69557df7b46637";       // USA, Rev 1
inline constexpr char kWiiPlaySha1[] = "0da5e7e51135219f580ad011d1b635bc83569bb9";         // USA, Rev 1
inline constexpr char kWiiPlayEuropeSha1[] = "323de3380c4ed9d26337d1219d46e527f2068d5e";   // Europe
inline constexpr char kSmoothMovesSha1[] = "ab32545dc32724f1e4eeeecc05189af722b8addd";     // USA
inline constexpr char kCityFolkSha1[] = "c87967b9ac0a943ba04ac16d875f5f79b4c46516";        // USA
inline constexpr char kMarioKartSha1[] = "1b367209919fcc225c74a155dc46f29c5d64b9a2";       // USA
inline constexpr char kMarioKartEuropeSha1[] = "ac7d72448630ade7655fc8bc5fd7a6543cb53a49"; // Europe
inline constexpr char kWiiMusicSha1[] = "a9ab9a8a8f14aa58d9bf4e05d72d29115ec589ab";        // USA
inline constexpr char kWiiMusicEuropeSha1[] = "2397197a29365b60d41f82b8f299ca5f2c4f9a2e";  // Europe
inline constexpr char kWiiShopChannelSha1[] = "aecf1c655b67c6412998cbc71bf58eba2e4edf03";  // World, v20
inline constexpr char kMiiChannelSha1[] = "5e0325b32438065bbc33fb82b755a250d93a9f9b";      // World, v6
inline constexpr char kPhotoChannelSha1[] = "b739b39949d3f3baada78171ef2d3bf36d478669";    // 1.1
inline constexpr char kForecastChannelSha1[] = "3c3bdec3e81441325e093c49380163e9d744597f";
inline constexpr char kNewsChannelSha1[] = "1bb643fa5f5e11930849b5efd514f08d30ead975";
inline constexpr char kPokemonRumbleSha1[] = "fd9a2c00c97e420a42355e2c27f3dc0ebbd3d8f9";

// A sound that a game starts with only some of its tracks, and the tracks that sound then, a bit for each.
struct StartTracks
{
    std::string_view sound;
    uint16_t tracks;
};

// A later set of tracks of such a sound: the state of the game that plays them, in words, the tracks, and the tempo
// that the state plays at, as a ratio.
struct TrackVariant
{
    std::string_view sound;
    std::string_view state;
    uint16_t tracks;
    float tempo_ratio = 1.0f;
};

// A value that a game writes to a variable of a sound's sequence as it starts the sound.
struct StartVariable
{
    std::string_view sound;
    uint8_t variable;
    int16_t value;
};

// Wii Sports
//
// Golf's wind, SE_LV_Wind, plays nothing while its variable 0 is 0. In every frame, golf holds the sound while the
// hole's wind is 1 or more (0x802b8c38). It writes the wind's speed to variable 0, and the wind's direction from the
// camera, 0 to 7, to variable 1 (0x802b7df8, from 0x802b8cb0). The courses draw each hole's speed from 0 to 15, or from
// part of that range (0x8029ced4 to 0x8029cf20, through 0x8029dcf4). The values below are a speed of 8, the middle of
// the speeds that sound, and direction 0. A higher speed plays the gusts louder and higher, with shorter waits between
// them. The direction sets how the gusts pan.
inline constexpr std::array<StartVariable, 2> kWiiSportsWind = {{
    {"SE_LV_Wind", 0, 8},
    {"SE_LV_Wind", 1, 0},
}};

// Wii Play
//
// Six minigames change the tracks of their music as a game goes on, through SeqSound::SetTrackMute (0x8003fa7c) and
// SetTrackVolume (0x8003fa84). kWiiPlayStarts holds the tracks that sound as the music starts, and kWiiPlayVariants
// each later set of tracks.
// - Pose Mii plays RP_BOM_BGM_PLAY (sound 0x22) in its states 8 and 9 only (0x802dd7e0). Each frame, 0x802dcef8 takes
//   the mask of the state: 0x1 in states 8 and 9. Only track 0 plays.
// - Table Tennis starts RP_PNP_BGM_PLAY (sound 0x6b) for one player and RP_PNP_BGM_PLAY_VS (0x6c) for two (0x80235760,
//   by the mode at 0x8022d724). Both start without tracks 7 to 11 and 15 (0x802359b0). The rally's arrangement plays
//   tracks 0 to 3, 7 to 11, 14 and 15. 0x80235350 sets it: it mutes the tracks outside 0xcb8f (from 0x80235a14), lets
//   those of 0xcb8f | 0xc00 play and sets tracks 7, 8, 9, 11 and 15 to full volume. One player's game plays it at each
//   tenth count of a rally from 10 on (0x80235540). Two players' match plays it each time the ball goes into play (the
//   match's phase 4, 0x8022cdc4). Three bars later, the game fades tracks 7, 8, 9, 11 and 15 down to 0.02 (-34 dB) in
//   49 frames, mutes track 10 and lets tracks 4 to 6 play again: the start's tracks. Track 12 counts the bars in the
//   sequence's variable 0 (0x80235394). It plays no note, and the masks leave it out. Once the match's higher score
//   reaches 11 (0x8022cdb4), the game no longer fades (0x802353e4), and the rally's arrangement stays. The game also
//   sets the tempo of a rally.
// - Laser Hockey starts RP_HKY_BGM_ALL_PLAY (sound 0x44) without tracks 0, 12, 13 and 14 (0x80220b04). Each frame,
//   0x8022074c lets tracks 0, 12 and 14 play once 30 s or less of the match are left. A match can also end at 8 goals
//   (0x8021a83c). In such a match, a player's seventh goal puts the music in state 3 (0x80216f1c). 0x8022074c then
//   lets tracks 0, 13 and 14 play. Nothing mutes those tracks again.
// - Billiards starts RP_BIL_BGM_PLAY (sound 0x4e) without tracks 3 and 4 (0x802c6b20). The game's state picks the
//   tracks (0x802c6988). On the beat after a change, 0x802c65a0 sets them. States 0, 1, 2 and 6 play the start's
//   tracks. State 3 adds track 3. It follows the crowd's booing (0x802c4918). State 4 plays every track. It follows a
//   cheer (0x802c4044). States 2 and 3 halve the sound's volume. State 5 ends the game: a drum fill replaces the music
//   (0x802c67f8).
// - Find Mii plays RP_WLY_BGM_PLAY_GUSU (sound 0x7a) as its time runs out (0x8028d8cc). On a beat, 0x8028d65c sets the
//   tracks of a mask from the table at 0x803247a0 to full volume and the others to 0. The seconds left pick the mask.
//   The music fades in at 20 s left (+0x18), or later if it starts with 25 s left or less. At 20 s, track 9 plays.
//   Track 0 joins at 18 s left, track 1 at 16 s, track 2 at 14 s and track 3 at 12 s. At 10 s, tracks 4 and 12 join.
//   At 6 s, only tracks 0, 3 and 4 play. The music then fades out. Track 11 never plays.
// - Tanks starts RP_TNK_BGM_LOOP2 (sound 0x3b) at 0x802682d0. 0x80268108 then plays the tracks in a cell of the table
//   at 0x80323298. The strongest kind of enemy tank still in play (1 to 9, 0x8025c914) picks the row. The number of
//   enemy tanks left, up to 4, picks the column. The first row plays tracks 0, 4 and 10 in each column. Each other mask
//   in the table is a variant, in the table's order.
inline constexpr std::array<StartTracks, 7> kWiiPlayStarts = {{
    {"RP_BOM_BGM_PLAY", 0x0001},
    {"RP_PNP_BGM_PLAY", 0x007f},      // tracks 0 to 6
    {"RP_PNP_BGM_PLAY_VS", 0x4077},   // tracks 0, 1, 2, 4, 5, 6 and 14
    {"RP_HKY_BGM_ALL_PLAY", 0x003e},  // tracks 1 to 5
    {"RP_BIL_BGM_PLAY", 0x00a7},      // tracks 0, 1, 2, 5 and 7
    {"RP_WLY_BGM_PLAY_GUSU", 0x0200}, // track 9
    {"RP_TNK_BGM_LOOP2", 0x0411},     // tracks 0, 4 and 10
}};

inline constexpr std::array<TrackVariant, 34> kWiiPlayVariants = {{
    {"RP_PNP_BGM_PLAY", "rally", 0x0f8f},    // tracks 0 to 3 and 7 to 11
    {"RP_PNP_BGM_PLAY_VS", "rally", 0xcf87}, // tracks 0, 1, 2, 7 to 11, 14 and 15
    {"RP_HKY_BGM_ALL_PLAY", "last 30 s", 0x503f},
    {"RP_HKY_BGM_ALL_PLAY", "match point", 0x603f},
    {"RP_HKY_BGM_ALL_PLAY", "last 30 s at match point", 0x703f},
    {"RP_BIL_BGM_PLAY", "state 3", 0x00af},
    {"RP_BIL_BGM_PLAY", "state 4", 0x00bf},
    {"RP_WLY_BGM_PLAY_GUSU", "17 or 18 s left", 0x0201},
    {"RP_WLY_BGM_PLAY_GUSU", "15 or 16 s left", 0x0203},
    {"RP_WLY_BGM_PLAY_GUSU", "13 or 14 s left", 0x0207},
    {"RP_WLY_BGM_PLAY_GUSU", "11 or 12 s left", 0x020f},
    {"RP_WLY_BGM_PLAY_GUSU", "7 to 10 s left", 0x121f},
    {"RP_WLY_BGM_PLAY_GUSU", "last 6 s", 0x0019},
    {"RP_TNK_BGM_LOOP2", "arrangement 2", 0x0491},
    {"RP_TNK_BGM_LOOP2", "arrangement 3", 0x0493},
    {"RP_TNK_BGM_LOOP2", "arrangement 4", 0x04c3},
    {"RP_TNK_BGM_LOOP2", "arrangement 5", 0x04d3},
    {"RP_TNK_BGM_LOOP2", "arrangement 6", 0x04d9},
    {"RP_TNK_BGM_LOOP2", "arrangement 7", 0x0419},
    {"RP_TNK_BGM_LOOP2", "arrangement 8", 0x0459},
    {"RP_TNK_BGM_LOOP2", "arrangement 9", 0x04e3},
    {"RP_TNK_BGM_LOOP2", "arrangement 10", 0x04f3},
    {"RP_TNK_BGM_LOOP2", "arrangement 11", 0x04f9},
    {"RP_TNK_BGM_LOOP2", "arrangement 12", 0x0543},
    {"RP_TNK_BGM_LOOP2", "arrangement 13", 0x0573},
    {"RP_TNK_BGM_LOOP2", "arrangement 14", 0x05f9},
    {"RP_TNK_BGM_LOOP2", "arrangement 15", 0x0547},
    {"RP_TNK_BGM_LOOP2", "arrangement 16", 0x0567},
    {"RP_TNK_BGM_LOOP2", "arrangement 17", 0x05f7},
    {"RP_TNK_BGM_LOOP2", "arrangement 18", 0x05fd},
    {"RP_TNK_BGM_LOOP2", "arrangement 19", 0x0713},
    {"RP_TNK_BGM_LOOP2", "arrangement 20", 0x07b9},
    {"RP_TNK_BGM_LOOP2", "arrangement 21", 0x07f9},
    {"RP_TNK_BGM_LOOP2", "arrangement 22", 0x07ed},
}};

// Animal Crossing: City Folk
//
// A piece of City Folk's music that the game plays with only some of its tracks: the tracks that sound in fine
// weather, in rain and in snow.
struct WeatherTracks
{
    std::string_view sound;
    std::array<uint16_t, 3> tracks;
};

// City Folk's main.dol holds three masks of tracks for each hourly piece (BGM_OUTDOOR_00 to 23, at 0x8047e9f0) and for
// each piece of town music (BGM_TOWN_EARLY to BGM_TOWN_MIDNIGHT, at 0x8047ea90). As a piece starts, 0x801cc23c takes
// the mask of the music's state. 0x8007e000 gives state 1 for weather 0 to 2, state 2 for rain (3 and 4) and state 3
// for snow (5 and 6). The game then sets the tracks outside the mask to volume 0 (SeqSound::SetTrackVolume, 0x80267774,
// from 0x801cc490).
inline constexpr std::array<WeatherTracks, 28> kCityFolkPieces = {{
    {"BGM_OUTDOOR_00", {0x00ff, 0x01a0, 0x06a4}}, {"BGM_OUTDOOR_01", {0x07ff, 0x07f8, 0x1ff8}},
    {"BGM_OUTDOOR_02", {0x007f, 0x0187, 0x07a7}}, {"BGM_OUTDOOR_03", {0x03ff, 0x1cb8, 0x6cb8}},
    {"BGM_OUTDOOR_04", {0x009f, 0x00e4, 0x03e4}}, {"BGM_OUTDOOR_05", {0x002f, 0x00f4, 0x03a4}},
    {"BGM_OUTDOOR_06", {0x05ff, 0x1a48, 0x7a48}}, {"BGM_OUTDOOR_07", {0x09ff, 0x1e3c, 0x7e3c}},
    {"BGM_OUTDOOR_08", {0x447f, 0x05c3, 0xa5c3}}, {"BGM_OUTDOOR_09", {0x009f, 0x006d, 0x032d}},
    {"BGM_OUTDOOR_10", {0x04ff, 0x03d5, 0x1bd5}}, {"BGM_OUTDOOR_11", {0x13ff, 0x0edc, 0x6edc}},
    {"BGM_OUTDOOR_12", {0x00ff, 0x0338, 0x0f3c}}, {"BGM_OUTDOOR_13", {0x00ff, 0x4fb0, 0x7eb0}},
    {"BGM_OUTDOOR_14", {0x03ff, 0x0f28, 0x3f2a}}, {"BGM_OUTDOOR_15", {0x007f, 0x03ac, 0x0fac}},
    {"BGM_OUTDOOR_16", {0x07ff, 0x3d70, 0xf570}}, {"BGM_OUTDOOR_17", {0x02ff, 0x0d5c, 0x3d5c}},
    {"BGM_OUTDOOR_18", {0x077f, 0x03ac, 0x1b6c}}, {"BGM_OUTDOOR_19", {0x01ff, 0x0752, 0x1f52}},
    {"BGM_OUTDOOR_20", {0x007f, 0x00f8, 0x03f8}}, {"BGM_OUTDOOR_21", {0x1f7f, 0x6bc1, 0xebc1}},
    {"BGM_OUTDOOR_22", {0x19ff, 0x1870, 0x1e70}}, {"BGM_OUTDOOR_23", {0x003f, 0x183c, 0xfc3c}},
    {"BGM_TOWN_EARLY", {0x99ff, 0xc600, 0xe600}}, {"BGM_TOWN_DAY", {0x07ff, 0x3800, 0x7800}},
    {"BGM_TOWN_NIGHT", {0x01ff, 0x1c20, 0x1e20}}, {"BGM_TOWN_MIDNIGHT", {0x003f, 0x01c0, 0x03c0}},
}};

// The start of the town music also sets its track 15 to volume 0 (0x801cbc34), in every weather. Only code that fades
// the track by the distance to a source (0x801cbec8) raises it again. Nothing in the game calls the functions that turn
// that code on (0x8007a778 and 0x80079294).
inline constexpr uint16_t kCityFolkTownTrack15 = 0x8000;

// The tracks of `piece` that sound in weather `weather`: 0 for fine weather, 1 for rain and 2 for snow.
constexpr uint16_t CityFolkTracks(const WeatherTracks& piece, std::size_t weather)
{
    const uint16_t silenced = piece.sound.starts_with("BGM_TOWN_") ? kCityFolkTownTrack15 : uint16_t{0};
    return static_cast<uint16_t>(piece.tracks[weather] & ~silenced);
}

// The museum's music (BGM_MUSEUM) takes the mask of one of seven arrangements, the music's states 16 to 22, from
// 0x8047ea80. The game starts the music in the first (0x801cb9bc). The jump table at 0x80500528 then gives stages 31 to
// 36 and 38 states 17 to 21. Stages 30 and 37 keep state 16. No stage gets state 22.
inline constexpr std::string_view kCityFolkMuseum = "BGM_MUSEUM";
inline constexpr std::array<uint16_t, 7> kCityFolkMuseumTracks = {0x00e7, 0x0063, 0x0063, 0x1863,
                                                                  0x007b, 0x0763, 0x0700};
static_assert(kCityFolkMuseumTracks[1] == kCityFolkMuseumTracks[2], "arrangements 2 and 3 play the same tracks");

// The arrangements that City Folk starts its music in: each piece of kCityFolkPieces in fine weather, then the
// museum's first arrangement.
constexpr std::array<StartTracks, kCityFolkPieces.size() + 1> CityFolkStarts()
{
    std::array<StartTracks, kCityFolkPieces.size() + 1> starts{};
    std::size_t s = 0;
    for (const WeatherTracks& piece : kCityFolkPieces)
    {
        starts[s++] = {piece.sound, CityFolkTracks(piece, 0)};
    }

    starts[s] = {kCityFolkMuseum, kCityFolkMuseumTracks[0]};

    return starts;
}

inline constexpr std::array<StartTracks, kCityFolkPieces.size() + 1> kCityFolkStarts = CityFolkStarts();

// The arrangements that City Folk doesn't start its music in. Each hourly piece and each piece of town music plays in
// rain and in snow too. The museum's music plays in arrangements 2 to 6. Arrangements 2 and 3 play the same tracks. No
// stage plays arrangement 7.
constexpr std::array<TrackVariant, 2 * kCityFolkPieces.size() + 4> CityFolkVariants()
{
    std::array<TrackVariant, 2 * kCityFolkPieces.size() + 4> variants{};
    std::size_t v = 0;
    for (const WeatherTracks& piece : kCityFolkPieces)
    {
        variants[v++] = {piece.sound, "rain", CityFolkTracks(piece, 1)};
        variants[v++] = {piece.sound, "snow", CityFolkTracks(piece, 2)};
    }

    variants[v++] = {kCityFolkMuseum, "arrangements 2 and 3", kCityFolkMuseumTracks[1]};
    variants[v++] = {kCityFolkMuseum, "arrangement 4", kCityFolkMuseumTracks[3]};
    variants[v++] = {kCityFolkMuseum, "arrangement 5", kCityFolkMuseumTracks[4]};
    variants[v] = {kCityFolkMuseum, "arrangement 6", kCityFolkMuseumTracks[5]};

    return variants;
}

inline constexpr std::array<TrackVariant, 2 * kCityFolkPieces.size() + 4> kCityFolkVariants = CityFolkVariants();

// The start of the music (0x801cbbbc) sets the sound's send to bus A, where the game's reverb is. The pieces of
// kCityFolkPieces send 0.25. K.K. Slider's songs, sounds 0x1a29 to 0x1a77, send 0.28. The labels of those songs start
// with kCityFolkSongPrefix.
inline constexpr float kCityFolkMusicSend = 0.25f;
inline constexpr float kCityFolkSongSend = 0.28f;
inline constexpr std::string_view kCityFolkSongPrefix = "BGM_KK";

// Mario Kart Wii
//
// Two pieces of the menus' music play some of their tracks: SEQ_O_SELECT (sound 0x55 of revo_kart.brsar), the music of
// the menus, and SEQ_O_EARTH (sound 1), the music of the Wi-Fi menus. The addresses are offsets in the sections of the
// USA release's module, files/rel/StaticR.rel. The European release has the same code and tables at other offsets.
// - Each section of the menus plays the music of a group of the archive (.text + 0xf5010). SEQ_O_SELECT is in group 7
//   (GRP_MENU), the group of sections 0x48 to 0x54. SEQ_O_EARTH is in group 3, that of sections 0x55 to 0x67 and 0x7f
//   to 0x8b.
// - The game gives each page of the menus a state from 0 to 6 (.text + 0xf52cc). As a page opens, the menus' sound
//   scene (.text + 0x20089c) keeps the page and fades the music's tracks to the state's volumes in 30 frames (.text +
//   0x1e9b90). States 4 and 6 change nothing. State 5 turns the music down to 0.55. In the Wi-Fi menus, pages 0x6e,
//   0x6f, 0x78 and 0x79 take state 3.
// - As the music starts, .text + 0x1e814c sets the volumes of the tracks (SeqSound::SetTrackVolume, through .text +
//   0x1e92bc). SEQ_O_SELECT takes those of the kept page's state: state 1 or 2, or else state 0. SEQ_O_EARTH takes
//   those of the menus' section: state 2 in sections 0x56 and 0x57, state 3 in sections 0x58, 0x59 and 0x5e to 0x67,
//   and state 0 in the others.
// - The volumes of each state are in the table at .rodata + 0x11508. SEQ_O_SELECT's state 0 plays track 0 alone, state
//   1 tracks 0, 1 and 3, and state 2 tracks 0, 1 and 2. In states 1 and 2, track 0 plays at 0.93 of its volume. A set
//   of tracks holds no volume. SEQ_O_EARTH's state 0 plays tracks 1, 2, 4 and 6, state 1 tracks 0, 1, 2, 4, 6, 8, 9
//   and 11, state 2 tracks 1, 2, 4, 5, 6, 8, 9, 11 and 13, and state 3 every track.
// The main menu (section 0x48, page 0x69) is in state 0. The Wi-Fi menus start in section 0x55, in state 0. The
// variants follow the order in which the menus first reach the states.
inline constexpr std::array<StartTracks, 2> kMarioKartStarts = {{
    {"SEQ_O_SELECT", 0x0001},
    {"SEQ_O_EARTH", 0x0056},
}};

inline constexpr std::array<TrackVariant, 5> kMarioKartVariants = {{
    {"SEQ_O_SELECT", "state 1", 0x000b},
    {"SEQ_O_SELECT", "state 2", 0x0007},
    {"SEQ_O_EARTH", "state 1", 0x0b57},
    {"SEQ_O_EARTH", "state 2", 0x2b76},
    {"SEQ_O_EARTH", "state 3", 0x3fff},
}};

// Seven of the sound effects read their variable 0. The game writes it with a value for each play, as it starts the
// effect, through its wrapper of SeqSoundHandle::WriteVariable (.text + 0x2041fc). The table holds the value of the
// effect's first play. Where play decides it, that is the first play from the start of a Grand Prix's first race, with
// controller 1:
// - SE_CN_RBW_RING gets 0, 2, 4, 6 or 8 by the ring (.text + 0x206c4c to 0x206cdc). The first ring gives 0.
// - SE_CN_RBW_STAR_RING gets 2 times the ring's count (.text + 0x1f60c0), or a parameter times the count (.text +
//   0x1faea0). The count starts at 0 (.text + 0x1af120 and 0x1af8d4).
// - SE_RC_RANK_UP and SE_RC_RANK_DOWN get 12 minus the new rank (.text + 0x202c10). The rank counts from 1: the HUD
//   shows frame rank - 1 (.text + 0x2de2e0). A lower rank than the last plays RANK_UP, any other change RANK_DOWN
//   (.text + 0x2de488). From 12th on the grid, the first RANK_UP reaches 11th and gives 1. The first RANK_DOWN can
//   come only after it. At the earliest, it returns to 12th and gives 0.
// - SE_UI_BTN_OK and SE_UI_CTRL_ON get the controller's channel, 0 to 3 (.text + 0x2018d0 to 0x201940). Controller 1
//   gives 0.
// SE_CC_MSR_GATE_PASS keeps -1. It gets the number of gates on the course minus the gate's number (.text + 0x36e2e4).
// The course's file gives both.
inline constexpr std::array<StartVariable, 6> kMarioKartValues = {{
    {"SE_CN_RBW_RING", 0, 0},
    {"SE_CN_RBW_STAR_RING", 0, 0},
    {"SE_RC_RANK_UP", 0, 1},
    {"SE_RC_RANK_DOWN", 0, 0},
    {"SE_UI_BTN_OK", 0, 0},
    {"SE_UI_CTRL_ON", 0, 0},
}};

// Wii Music
//
// The main menu plays RP_MUSIC_MAINMENU_BACKTRACK (sound 0x42c of rp_Music_sound.brsar, 0x803cced8) with some of its
// 15 tracks. The menu's state (+0x104 of the menu's object, from the menu's choice at 0x80299b8c) picks them. As a
// state starts, 0x8029da6c sets levels for groups of tracks. Each frame, 0x8029de40 fades the levels toward the state's
// targets over 60 frames. It then sets the tracks' volumes through SeqSound::SetTrackVolume (0x800bd810): tracks 1 to
// 5 to the larger of two levels, track 6 to the largest of three, and tracks 8 to 11 and 12 to 14 to one level each.
// Tracks 0 and 7 always play. States 0, 2 and 4 play tracks 0 to 7. The game starts the menu in state 0. The menu's
// carousel plays tracks 1 to 6 at 0.74 or 0.51, by the stop that faces the front. State 2 plays them at 0.9. The main
// menu never plays tracks 8 to 14 together.
//
// Pitch Perfect starts RP_KKM_BEATS_LOOP (sound 0x352), or RP_KKM_BEATS_LOOP_MAKE_SCORE (0x353) in its modes 12 and
// 13, at 0x8021d60c. It then silences the tracks outside a mask through the music manager's track fader (0x801bc904).
// The round's level, at +0x8 of the round's record, picks the mask: 0x0b below level 2, 0x1b at levels 2 and 3, 0x3b
// at 4 to 6 and 0x7b at 7 to 9. Once the round's time (+0xc) reads 10 s or less, 0x8021e058 adds track 2.
// RP_KKM_BEATS_LOOP_MAKE_SCORE has five tracks. Its levels 2 to 9 play the same ones.
inline constexpr std::array<StartTracks, 3> kWiiMusicStarts = {{
    {"RP_MUSIC_MAINMENU_BACKTRACK", 0x00ff},
    {"RP_KKM_BEATS_LOOP", 0x000b},
    {"RP_KKM_BEATS_LOOP_MAKE_SCORE", 0x000b},
}};

inline constexpr std::array<TrackVariant, 13> kWiiMusicVariants = {{
    {"RP_MUSIC_MAINMENU_BACKTRACK", "state 1", 0x0f81},       // tracks 0, 7 and 8 to 11
    {"RP_MUSIC_MAINMENU_BACKTRACK", "state 3", 0x70c1},       // tracks 0, 6, 7 and 12 to 14
    {"RP_MUSIC_MAINMENU_BACKTRACK", "states 5 to 7", 0x0081}, // tracks 0 and 7
    {"RP_KKM_BEATS_LOOP", "levels 2 and 3", 0x001b},
    {"RP_KKM_BEATS_LOOP", "levels 4 to 6", 0x003b},
    {"RP_KKM_BEATS_LOOP", "levels 7 to 9", 0x007b},
    {"RP_KKM_BEATS_LOOP", "with track 2", 0x000f},
    {"RP_KKM_BEATS_LOOP", "levels 2 and 3 with track 2", 0x001f},
    {"RP_KKM_BEATS_LOOP", "levels 4 to 6 with track 2", 0x003f},
    {"RP_KKM_BEATS_LOOP", "levels 7 to 9 with track 2", 0x007f},
    {"RP_KKM_BEATS_LOOP_MAKE_SCORE", "levels 2 to 9", 0x001b},
    {"RP_KKM_BEATS_LOOP_MAKE_SCORE", "with track 2", 0x000f},
    {"RP_KKM_BEATS_LOOP_MAKE_SCORE", "levels 2 to 9 with track 2", 0x001f},
}};

// Pitch Perfect starts its phrases and the jingles of its pictures at 0x8021d138: RP_KKM_SE_PHRASE_1_1 to 1_6 and the
// 13 sounds whose labels start with RP_KKM_SE_IMG_. 0x8021d04c picks them from its jump table. 0x8021d138 then sets
// the sound's send to bus A to the scene's send for that bus (0x803cded0). As Pitch Perfect starts, it sets up the
// buses from row 14 of the table at 0x8059f978 (0x8021e30c). That row puts the reverb on bus A with a send of 0.2.
inline constexpr std::array<std::string_view, 2> kPitchPerfectSendPrefixes = {"RP_KKM_SE_PHRASE_1_", "RP_KKM_SE_IMG_"};
inline constexpr float kPitchPerfectSend = 0.2f;

// Whether the sound labelled `label` gets Pitch Perfect's send: its label starts with one of kPitchPerfectSendPrefixes.
constexpr bool IsPitchPerfectSendSound(std::string_view label)
{
    for (const std::string_view prefix : kPitchPerfectSendPrefixes)
    {
        if (label.starts_with(prefix))
        {
            return true;
        }
    }

    return false;
}

// The Mii Channel
//
// The music, SEQ_TMP_BGM, starts without a send (0x80018a2c). As it starts, the channel silences its tracks 9, 10 and
// 11 (SeqSoundHandle::SetTrackSilence, 0x800fdf0c). The bytes at +0x11b8 to +0x11ba of the channel's sound object say
// which of them play. The scene function (0x80018018, by the jump table at 0x80233a20) sets them again. At boot it lets
// track 9 play: every track but 10 and 11 sounds. The channel's code names none of its scenes but the root scene
// (0x80206548). Its scene factory (0x80010484) gives each scene a number. A variant's state gives a scene's number, or
// the sound that the code plays with the change of state:
// - "scene 4": track 10 alone of the three. Scene 4 sets it (0x8002c2d8).
// - "menu out": tracks 9 and 11. The code lets track 11 play as it plays SEQ_DOOL_MENU_OUT. With SEQ_DOOL_INTO_MENU,
//   it silences track 11 again. Neither change touches tracks 9 and 10.
// - "scenes 1 and 3": none of the three. Scenes 1 and 3 set it (0x80011b30 and 0x8001755c).
// - "plaza speed up": the tracks of boot at a tempo ratio of 1.0878 (0x8001862c). The code sets the ratio as it plays
//   SEQ_PLAZA_SPEED_UP. It sets the ratio back to 1 with SEQ_PLAZA_SPEED_DOWN.
// The channel also turns the music down to a volume of 0.4 and back up (0x8001868c and 0x800186e4).
//
// Each of the channel's four starters of sound effects (0x80018bb4, 0x80018cf8, 0x80018ddc and 0x80019474) sends 0.2 of
// its sound to bus A (BasicSound::SetFxSend, 0x800f73a0).
inline constexpr std::array<StartTracks, 1> kMiiChannelStarts = {{
    {"SEQ_TMP_BGM", 0xf3ff},
}};

inline constexpr std::array<TrackVariant, 4> kMiiChannelVariants = {{
    {"SEQ_TMP_BGM", "scene 4", 0xf5ff},
    {"SEQ_TMP_BGM", "menu out", 0xfbff},
    {"SEQ_TMP_BGM", "scenes 1 and 3", 0xf1ff},
    {"SEQ_TMP_BGM", "plaza speed up", 0xf3ff, 1.0878f},
}};

inline constexpr float kMiiChannelEffectSend = 0.2f;

} // namespace ntdlib::rvl::snd

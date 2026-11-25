// SPDX-License-Identifier: MIT

// The states in which Luigi's Mansion's code plays a piece of music once the piece has started: the four boss pieces
// and LuigiSings, the music of the mansion's rooms. The addresses are in the main.dol of the USA release (GLME01).
//
// A state of a boss piece is a number. The game's code passes it to the function that sets the piece's tracks
// (0x80188898). State 0 is the piece's start: the game's start of a piece (0x801889fc) applies it when the piece's
// entry in the sound table has flag 0x04000000 (0x80188c9c to 0x80188cac). The fight's code passes the other numbers.
// A state of LuigiSings is a place, Luigi's health and two flags. The game's code picks the piece's tracks and its
// tempo from them in each frame.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ntdlib::jaudio::luigis_mansion
{

// A state of a boss piece: the piece's JAudio sound ID, the state's number, the children of the piece's track 0 that
// the state plays at volume 0 (bit n for child n), and the piece's tempo.
//
// For each state, the table at 0x8038ec08 gives a byte for each of the 16 children: 0 sets the child's volume to 0, 1
// sets it to 1, and 2 leaves it as it is. A child that has 2 in one state has 2 in every state of its piece. It keeps
// the volume of 1 that it starts with. The function sets the volumes and the tempo at once, with no fade.
struct BossState
{
    uint32_t sound = 0;
    uint8_t state = 0;
    uint16_t silent = 0;
    float tempo = 1.0f;

    // The volume that the state gives child `child` of track 0.
    constexpr float Volume(unsigned child) const
    {
        return child < 16 && (silent >> child & 1) != 0 ? 0.0f : 1.0f;
    }
};

// The tempo that 0x8018c77c gives TelesaBattle's state 1 for a count b: 1 + 0.43 x (15 - min(b, 15)) / 15, with 0.43
// and 15 at 0x804dd0d8 and 0x804dd0dc, in single precision in the game's order (0x8018c7d0 to 0x8018c820).
constexpr float TelesaBattleTempo(uint8_t count)
{
    const int below = 15 - (count < 15 ? count : 15);
    return 1.0f + 0.43f * static_cast<float>(below) / 15.0f;
}

// The count of the first call that sets TelesaBattle's state 1. A Dolphin run of the USA release reached the first
// split with b = 15. Later splits, as minis are caught, play faster and aren't modelled.
inline constexpr uint8_t kTelesaBattleFirstCount = 15;

// The four boss pieces' states. The rows of the table at 0x8038ec08 start at 0x8038ec40 (BabyBattle), 0x8038ec70
// (BlueBattle), 0x8038eca0 (TelesaBattle) and 0x8038ecd0 (KuppaBattle), 16 bytes a state. The tempos of BabyBattle,
// BlueBattle and KuppaBattle are floats from 0x8038ed00, 0x8038ed0c and 0x8038ed24, one a state. The function sets
// them through 0x801981c0. It sets no tempo for TelesaBattle.
//
// - BabyBattle (Chauncey, map 10) and BlueBattle (Bogmire, map 13): a callback (0x8018a990) sets state 1 while the
//   sound event byte at 0x804d91b4 is 8, and state 0 once it is 0 again. Luigi's code sets event 8 at 0x800b3e68.
//   0x8018bbb8 sets state 2 with 1 and state 0 with 0. Luigi's code calls it at 0x800b1c8c and 0x800b1cb8.
// - TelesaBattle (Boolossus): 0x8018c77c(a, b) sets state 0 and moves the tempo over 30 frames to 1 when a is 0. When a
//   isn't 0 (clrlwi. at 0x8018c7a4), it sets state 1 and moves the tempo over 30 frames to TelesaBattleTempo(b).
//   Boolossus's code calls it with a = 0 and b = 1 at 0x80137798 and at 0x80138814 (state 0), and with a = 1 at
//   0x801384a0 (state 1). That call's b is the byte at +0x23c of Boolossus's object (0x8013849c), which 0x80138e68 sets
//   at 0x80137390: a count of the objects of type 0xd6 in a list. The count changes during the fight, and so does the
//   tempo. The list uses the tempo at the count of the first call, kTelesaBattleFirstCount.
// - KuppaBattle (King Boo): 0x8018c848 sets state 1 with a non-zero argument (clrlwi. at 0x8018c870, called with 1 at
//   0x800a617c) and state 0 with 0 (called at 0x800a6330).
//
// The table also has a state 2 for TelesaBattle and KuppaBattle (0x8038ecc0 and 0x8038ecf0, and KuppaBattle's tempo
// 1.43). No code passes state 2 for those pieces, and the list leaves them out.
inline constexpr std::array kBossStates{
    BossState{0x8000002b, 0, 0x3f01},                                             // BabyBattle
    BossState{0x8000002b, 1, 0x1132},                                             // BabyBattle
    BossState{0x8000002b, 2, 0x0232, 1.16f},                                      // BabyBattle
    BossState{0x8000002e, 0, 0x1f00},                                             // BlueBattle
    BossState{0x8000002e, 1, 0x03b0},                                             // BlueBattle
    BossState{0x8000002e, 2, 0x00a8, 1.091f},                                     // BlueBattle
    BossState{0x80000031, 0, 0xf000},                                             // TelesaBattle
    BossState{0x80000031, 1, 0x0f15, TelesaBattleTempo(kTelesaBattleFirstCount)}, // TelesaBattle
    BossState{0x80000052, 0, 0xf800},                                             // KuppaBattle
    BossState{0x80000052, 1, 0x04b7, 1.47f},                                      // KuppaBattle
};

// The words that name a boss state in the names of the piece's files: "state" and the state's number. State 0 is the
// piece's start, and its file has the piece's name alone.
inline std::string LabelOf(const BossState& boss)
{
    if (boss.state == 0)
    {
        return {};
    }

    return "state " + std::to_string(boss.state);
}

// LuigiSings's sound ID.
inline constexpr uint32_t kLuigiSings = 0x80000007;

// Where LuigiSings plays, by the accompaniment that it gives the tune. The lit room is a dark room's place once its
// lights are on.
enum class SingingPlace : uint8_t
{
    kHallway,
    kRoom,
    kOutdoors,
    kUnaccompanied,
    kLitRoom,
};

// The words for a place in the names of its states.
constexpr std::string_view NameOf(SingingPlace place)
{
    switch (place)
    {
    case SingingPlace::kHallway:
        return "hallway";
    case SingingPlace::kRoom:
        return "room";
    case SingingPlace::kOutdoors:
        return "outdoors";
    case SingingPlace::kUnaccompanied:
        return "unaccompanied";
    case SingingPlace::kLitRoom:
        return "lit room";
    }

    return "";
}

// A room of map 2 that gives a place its accompaniment, and the room's fields of the room table (roominfo):
// sound_echo_parameter, sound_room_size and sound_room_code. 0x8002d90c reads the table. The game packs the fields
// at 0x8018d48c (RoomSound).
struct SingingRoom
{
    uint8_t room = 0;
    uint8_t echo = 0;
    uint8_t size = 0;
    uint8_t code = 0;
};

// The room fields packed as the game packs them: the echo, the size << 6 and the code << 8.
constexpr uint32_t RoomSound(const SingingRoom& room)
{
    return room.echo | static_cast<uint32_t>(room.size) << 6 | static_cast<uint32_t>(room.code) << 8;
}

// The first room of the room table that gives a place its accompaniment. 0x80189488 picks the accompaniment: children 0
// to 3 and 11 in a room whose sound_room_code is 6 or 7, and in room 35 while it's dark (hallway). Child 9 in a room
// whose sound_room_size is 2 (outdoors). Children 8 and 10 in the other rooms (room). Rooms 19, 32 and 65 get none
// (unaccompanied). The lit room is room 0 with its lights on.
constexpr SingingRoom RoomOf(SingingPlace place)
{
    switch (place)
    {
    case SingingPlace::kHallway:
        return {6, 10, 0, 7};
    case SingingPlace::kOutdoors:
        return {11, 0, 2, 1};
    case SingingPlace::kUnaccompanied:
        return {19, 10, 0, 6};
    case SingingPlace::kRoom:
    case SingingPlace::kLitRoom:
        return {0, 10, 1, 0};
    }

    return {};
}

// The children of track 0 that accompany the tune in a place (bit n for child n).
constexpr uint16_t AccompanimentOf(SingingPlace place)
{
    switch (place)
    {
    case SingingPlace::kHallway:
        return 0x080f;
    case SingingPlace::kRoom:
        return 0x0500;
    case SingingPlace::kOutdoors:
        return 0x0200;
    case SingingPlace::kUnaccompanied:
    case SingingPlace::kLitRoom:
        return 0;
    }

    return 0;
}

// A state of LuigiSings. The game's start of the piece (0x8018c274) and its frame (0x8018bd84) pick the children that
// play. Children 4 to 7 of track 0 play the tune, Luigi's humming and whistling. One of them plays at a time: child 4
// while Luigi has more than 70 of his 100 points of health, child 5 while he has more than 40, and child 6 below that.
// Child 7 plays alone while a room's lights are on, unless the room is outdoors. 0x80198e4c mutes the others. The
// accompaniment plays at a volume of 1 and the rest of children 0 to 11 at 0. Children 12 to 15 play in every state.
// The frame sets the tempo to 0.6 + 0.4 x the health / 100. With the lights on, it scales that by the time between
// Luigi's steps, to 12 / 16 x 1.2 of it while he stands still. A state takes the most health of its band.
//
// shrunk: a poison mushroom (EnNegativeItem) has shrunk Luigi. His state 56 (0x800d0e4c) calls 0x80189468 with 1
// (0x800b13e0 to 0x800b1424), which sets the float at 0x804d783c to 2.0. The frame gives children 4 to 6 that float
// through 0x801991b4, and the tune plays an octave higher. Child 7 doesn't take it. Once the state's timer runs out,
// the game calls 0x80189468 with 0 (0x800b4848 to 0x800b48a0).
//
// humming_muted: the tune is muted, and the accompaniment plays alone. 0x80189378 and 0x801893f0 store their argument
// in the byte at 0x804d91b9. While it is 1 and LuigiSings plays, they write 255 to the tune byte at 0x804d7840 and mute
// children 4 to 7 (0x8018b9a4 through 0x80198e4c). LuigiSings's start mutes them too while the byte is 1 (0x8018c464 to
// 0x8018c478). The game sets the byte to 1 while a message window is open: 0x8004225c, called in each frame from
// 0x8000bb18, calls 0x80189378 with 1 while 0x80044058 finds a window open, and with 0 once none is (0x80042794 to
// 0x800427cc).
struct SingingState
{
    SingingPlace place = SingingPlace::kHallway;
    uint8_t health = 100;
    bool shrunk = false;
    bool humming_muted = false;

    // The child of track 0 that plays the tune, or -1 for none.
    constexpr int TuneChild() const
    {
        if (humming_muted)
        {
            return -1;
        }

        if (place == SingingPlace::kLitRoom)
        {
            return 7;
        }

        return health > 70 ? 4 : health > 40 ? 5 : 6;
    }

    // The children of track 0 that play at a volume of 1 (bit n for child n).
    constexpr uint16_t Playing() const
    {
        const int tune = TuneChild();
        const uint16_t tune_bit = tune < 0 ? 0 : static_cast<uint16_t>(1u << tune);
        return static_cast<uint16_t>(0xf000 | AccompanimentOf(place) | tune_bit);
    }

    // The volume of child `child` of track 0.
    constexpr float Volume(unsigned child) const
    {
        return child < 16 && (Playing() >> child & 1) != 0 ? 1.0f : 0.0f;
    }

    // The tempo, as a factor of the sequence's.
    constexpr float Tempo() const
    {
        const float tempo = 0.6f + 0.4f * static_cast<float>(health) / 100.0f;
        return place == SingingPlace::kLitRoom ? 12.0f / 16.0f * 1.2f * tempo : tempo;
    }
};

// LuigiSings's states. The shrunk states take 100 HP in each place but the lit room. The states with the humming muted
// take 100 HP in each place with an accompaniment. Without the tune, the health changes only the tempo. In the lit
// room and without an accompaniment, the piece is silent with the tune muted.
inline constexpr std::array kSingingStates{
    SingingState{SingingPlace::kHallway, 100},
    SingingState{SingingPlace::kHallway, 70},
    SingingState{SingingPlace::kHallway, 40},
    SingingState{SingingPlace::kRoom, 100},
    SingingState{SingingPlace::kRoom, 70},
    SingingState{SingingPlace::kRoom, 40},
    SingingState{SingingPlace::kOutdoors, 100},
    SingingState{SingingPlace::kOutdoors, 70},
    SingingState{SingingPlace::kOutdoors, 40},
    SingingState{SingingPlace::kUnaccompanied, 100},
    SingingState{SingingPlace::kUnaccompanied, 70},
    SingingState{SingingPlace::kUnaccompanied, 40},
    SingingState{SingingPlace::kLitRoom, 100},
    SingingState{SingingPlace::kHallway, 100, true},
    SingingState{SingingPlace::kRoom, 100, true},
    SingingState{SingingPlace::kOutdoors, 100, true},
    SingingState{SingingPlace::kUnaccompanied, 100, true},
    SingingState{SingingPlace::kHallway, 100, false, true},
    SingingState{SingingPlace::kRoom, 100, false, true},
    SingingState{SingingPlace::kOutdoors, 100, false, true},
};

// The words that name a state of LuigiSings in the names of the piece's files: the place, the health with "HP" (in
// every place but the lit room), then "shrunk" or "no humming" for the two flags.
inline std::string LabelOf(const SingingState& singing)
{
    std::string label(NameOf(singing.place));
    if (singing.place != SingingPlace::kLitRoom)
    {
        label += " " + std::to_string(singing.health) + " HP";
    }

    if (singing.shrunk)
    {
        label += " shrunk";
    }

    if (singing.humming_muted)
    {
        label += " no humming";
    }

    return label;
}

} // namespace ntdlib::jaudio::luigis_mansion

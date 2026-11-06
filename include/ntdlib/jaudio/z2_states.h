// SPDX-License-Identifier: MIT

// The states in which Twilight Princess's sound layer, Z2, plays a piece of music once the piece has started, and the
// states that its scenes and its other starters start more pieces in. RulesOf in sequence_export/jaudio/game.h reads
// the table. PieceStateOf in flipper/z2_params.h gives the Z2 driver a state from it. HD's code gives some pieces other
// states (kTwilightPrincessHdStates). The starts that Z2SeqMgr::bgmStart gives five pieces are in z2_starts.h.

#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace ntdlib::jaudio
{

// Children of a piece's track 0 at one volume: bit n for child n.
struct ChildGroup
{
    uint16_t children = 0;
    float volume = 1.0f;
};

// A status that the game's code writes to port 9 of a piece's root track in each frame while port 10 holds `section`.
// The piece's sequence gives its section on port 10. A status of 0 is none.
struct SectionStatus
{
    uint8_t section = 0;
    uint8_t status = 0;
};

// A state of a piece of music: the piece's JAudio sound ID, the suffix of the state's name, the children of track 0
// whose volumes the state sets other than 1, the value that the game's code writes to port 9 of the piece's root
// track with the state, or -1 for none, and the statuses that it writes by the section instead. A state without a
// suffix is the piece's start. Its name is the piece's. Any other state's name is the piece's with the suffix added.
//
// status_later marks a status that the game writes once the piece plays, such as an actor's as a fight goes on. Many
// sequences clear port 9 in their first tick. A status written with the start is then lost. The driver in
// drivers/jaudio/z2.h therefore writes this one 30 frames after the start. sequence_export's JAudio performer writes
// every status after the first tick.
struct MusicState
{
    uint32_t sound = 0;
    std::string_view suffix;
    std::array<ChildGroup, 3> groups{};
    int32_t status_port = -1;
    std::array<SectionStatus, 2> section_statuses{};
    bool status_later = false;

    // The volume that the state gives child `child` of track 0.
    constexpr float Volume(unsigned child) const
    {
        for (const ChildGroup& g : groups)
        {
            if (child < 16 && (g.children >> child & 1) != 0)
            {
                return g.volume;
            }
        }
        return 1.0f;
    }
};

// A state built from its groups.
constexpr MusicState State(uint32_t sound, std::string_view suffix, std::initializer_list<ChildGroup> groups,
                           int32_t status_port = -1, std::initializer_list<SectionStatus> section_statuses = {})
{
    MusicState state{sound, suffix, {}, status_port, {}};
    std::size_t i = 0;
    for (const ChildGroup& g : groups)
    {
        state.groups[i++] = g;
    }
    i = 0;
    for (const SectionStatus& s : section_statuses)
    {
        state.section_statuses[i++] = s;
    }
    return state;
}

// A state whose status the game writes once the piece plays (MusicState::status_later).
constexpr MusicState LaterState(uint32_t sound, std::string_view suffix, std::initializer_list<ChildGroup> groups,
                                int32_t status_port)
{
    MusicState state = State(sound, suffix, groups, status_port);
    state.status_later = true;
    return state;
}

// Twilight Princess's states. They hold on the GameCube and on the Wii. The addresses are in the GameCube's USA
// main.dol, then in the Wii's (the USA release's third version).
//
// Z2SeqMgr::changeBgmStatus (0x802aff8c, 0x80294254) sets the tracks of the main music by a status, and
// changeSubBgmStatus (0x802b1df4, 0x80295dd0) those of the music that plays over it. Both set children of track 0
// through setChildTrackVolume (0x802b3fec, 0x802977a4). changeBgmStatus also writes the status to port 9 of the root
// track (Z2SoundStarter::setPortData, 0x802aaedc, 0x8028f360). changeSubBgmStatus writes it for some pieces. A scene's
// start calls changeBgmStatus with the room's number for seven pieces, and with 0 for four more
// (Z2SceneMgr::sceneBgmStart, 0x802ba294, 0x8029c6c4). The state of room 0, or of status 0, starts those pieces here.
// subBgmStart (0x802af49c, 0x802939d8) calls changeSubBgmStatus with 0 or 1 for four pieces. The battle music's layers
// follow the fight (startBattleBgm at 0x802b4eb0, setBattleSearched at 0x802b421c, setBattleDistState at 0x802b4498
// and setBattleSeqState at 0x802b4844; on the Wii 0x80298288, 0x80297998, 0x80297bac and 0x80297e38).
//
// A state that plays what another plays is left out. So are the states between two others, such as a layer that an
// actor's distance fades in. A status that only lowers the whole piece, through muteSceneBgm, isn't a state here. A
// status that changes only port 9 is a state when the piece's sequence reads the value. The game moves the volumes to a
// new state over up to 143 frames. A state here plays its volumes from the start.
inline constexpr std::array kTwilightPrincessStates{
    // Z2BGM_FIELD_LINK_DAY: Hyrule Field's day music, as Link walks (no children 5 to 11) and as he rides (no children
    // 12 to 15). z2_starts.h has the piece's two starts. Its sequence is a chain of sections. It gives its section on
    // port 10. At a section's end, it reads a status from port 9, may jump on it, and clears the port after a jump.
    // Z2SeqMgr::fieldBgmFramework (0x802b594c, 0x80298b64) reads port 10 in each frame and writes a status by the
    // situation. HD's code at 0x02c26620 writes the same statuses. The sequence is the same file in the three releases.
    // The sections below go by the names that the sequence prints (POS_A1 and so on).
    //
    // The two starts play the intro, then sections A1 to A6 in a loop. bgmStart writes 0 and Link's standing writes 13.
    // No section jumps on either.
    //
    // Status 12: Link has moved for 150 frames. A1 goes on to D1, and the D sections loop.
    State(0x01000000, "_status12", {{0x0fe0, 0.0f}}, 12),
    State(0x01000000, "_riding_status12", {{0xf000, 0.0f}}, 12),
    // Statuses 4 to 7: enemies are near. Each leads to the B sections' loop by way of its entry. The code writes 4 in
    // A1, A5 and A6, and goes to B1 at the end of A1. It writes 7 in A2 (B3), 6 in A3 (B5) and 5 in A4 (B7).
    State(0x01000000, "_status4", {{0x0fe0, 0.0f}}, 4),
    State(0x01000000, "_riding_status4", {{0xf000, 0.0f}}, 4),
    State(0x01000000, "_status5", {{0x0fe0, 0.0f}}, 5),
    State(0x01000000, "_riding_status5", {{0xf000, 0.0f}}, 5),
    State(0x01000000, "_status6", {{0x0fe0, 0.0f}}, 6),
    State(0x01000000, "_riding_status6", {{0xf000, 0.0f}}, 6),
    State(0x01000000, "_status7", {{0x0fe0, 0.0f}}, 7),
    State(0x01000000, "_riding_status7", {{0xf000, 0.0f}}, 7),
    // Status 10: evening comes outside the D sections. A1 goes on to the ending E1, and the piece ends.
    State(0x01000000, "_status10", {{0x0fe0, 0.0f}}, 10),
    State(0x01000000, "_riding_status10", {{0xf000, 0.0f}}, 10),
    // Status 11: evening comes in the D sections. Only they jump on it, to the ending E2. Status 12 leads there.
    State(0x01000000, "_status11", {{0x0fe0, 0.0f}}, -1, {{1, 12}, {17, 11}}),
    State(0x01000000, "_riding_status11", {{0xf000, 0.0f}}, -1, {{1, 12}, {17, 11}}),
    // Statuses 8 and 9: the code starts the piece itself once no music plays, with 8 before 8 o'clock and 9 from then
    // on. The first tick reads them. 8 plays F1 and G1 in place of the intro, and 9 plays G1. A1 to A6 follow.
    State(0x01000000, "_status8", {{0x0fe0, 0.0f}}, -1, {{0, 8}}),
    State(0x01000000, "_riding_status8", {{0xf000, 0.0f}}, -1, {{0, 8}}),
    State(0x01000000, "_status9", {{0x0fe0, 0.0f}}, -1, {{0, 9}}),
    State(0x01000000, "_riding_status9", {{0xf000, 0.0f}}, -1, {{0, 9}}),
    // Status 3: Link rides and stands, with no enemy very far. The code writes 3 in A3, each time A3 plays. A3 goes on
    // to C1, and C1 back to A1. Link on foot never gets it.
    State(0x01000000, "_riding_status3", {{0xf000, 0.0f}}, -1, {{3, 3}}),

    // Z2BGM_TOAL_VILLEGE: status 1 plays children 0 to 7 at 0.65, child 8 at 0, and children 9 and 10 at 1.
    State(0x01000005, "_status1", {{0x00ff, 0.65f}, {0x0100, 0.0f}}, 1),

    // Z2BGM_DUNGEON_LV5: rooms 1 and 2 play children 8 to 13 in place of 0 to 4. The other rooms play 0 to 4.
    State(0x01000037, "", {{0x3f00, 0.0f}}),
    State(0x01000037, "_status1", {{0x001f, 0.0f}}, 1),

    // Z2BGM_CASTLE_TOWN: rooms 0 and 1 and the shops (status 5) play children 0 to 4. Room 2 plays 2 to 6, room 3
    // plays 0, 1, 4 and 10 to 12, and room 4 plays 3 and 7 to 9.
    State(0x0100003b, "", {{0x1fe0, 0.0f}}),
    State(0x0100003b, "_status2", {{0x1f83, 0.0f}}, 2),
    State(0x0100003b, "_status3", {{0x03ec, 0.0f}}, 3),
    State(0x0100003b, "_status4", {{0x1c77, 0.0f}}, 4),

    // Z2BGM_HOLY_FOREST: status 0 plays children 0 to 3. Status 1 plays 0 to 3 at 0.6 and 4 to 6 at 1.
    State(0x01000041, "", {{0x00f0, 0.0f}}),
    State(0x01000041, "_status1", {{0x000f, 0.6f}, {0x0080, 0.0f}}, 1),

    // Z2BGM_DEMO08: status 0 leaves out children 4 to 7. Status 1 brings in child 4.
    State(0x0100004e, "", {{0x00f0, 0.0f}}),
    State(0x0100004e, "_status1", {{0x00e0, 0.0f}}, 1),

    // Z2BGM_DEMO10: status 0 leaves out children 5 to 7. Status 1 plays them, children 0 and 1 at 0.3, children 2 and
    // 3 at 0.6, and child 4 at 0.
    State(0x01000038, "", {{0x00e0, 0.0f}}),
    State(0x01000038, "_status1", {{0x0003, 0.3f}, {0x000c, 0.6f}, {0x0010, 0.0f}}, 1),

    // Z2BGM_BOSS_SNOWWOMAN_1: status 1 plays children 10 to 12 in place of child 9.
    State(0x0100004d, "_status1", {{0x0200, 0.0f}}, 1),

    // Z2BGM_DUNGEON_LV8: rooms 0 and 11 play children 2 to 5. The other rooms play 6 to 10. From 100 on, the status
    // comes from an actor: children 6 to 10 play, and 11 to 14 come in as the status falls to 200 (getParamByExp).
    State(0x0100003e, "", {{0x7fc0, 0.0f}}),
    State(0x0100003e, "_status1", {{0x783c, 0.0f}}, 1),
    State(0x0100003e, "_status100", {{0x003c, 0.0f}}, 100),

    // Z2BGM_DUNGEON_LV9_02: rooms 8 and 12 set the tracks. Over 3000, the status comes from Link's height: from 3150
    // to 6150 it moves the volumes from room 12's to every child's but 0 to 2 (linearTransform). For any other status,
    // the code passes volumes that it never sets. They hold what earlier code left in the registers. In the runs, the
    // GameCube passed 0 and the Wii passed stray values. The rules leave those rooms at the start's volumes.
    State(0x0100003f, "_status8", {{0x0007, 0.0f}, {0x0078, 0.5f}, {0x0200, 0.8f}}, 8),
    State(0x0100003f, "_status12", {{0x0278, 0.0f}, {0x0180, 0.5f}}, 12),
    State(0x0100003f, "_status6150", {{0x0007, 0.0f}}, 6150),

    // Z2BGM_VS_GANON_02: status 1 plays children 10 to 13 in place of 0 to 7.
    State(0x01000087, "_status1", {{0x00ff, 0.0f}}, 1),

    // Z2BGM_VS_GANON_04: statuses 4 to 6 leave out some of children 12 to 15. Status 3 plays every child. The sequence
    // reads its 1, 2 and 3 on port 9. The boss's demo_camera (module d_a_b_gnd) writes all six. The section of status 1
    // asks the game for status 7 (synccpu 0x1007), which leaves out children 12 to 15. The sections of 2 and 3 ask for
    // status 8 (0x1008), which plays them.
    LaterState(0x01000088, "_status4", {{0xc000, 0.0f}}, 4),
    LaterState(0x01000088, "_status5", {{0x3000, 0.0f}}, 5),
    LaterState(0x01000088, "_status6", {{0xf000, 0.0f}}, 6),
    LaterState(0x01000088, "_status3", {}, 3),
    LaterState(0x01000088, "_status1", {{0xf000, 0.0f}}, 1),
    LaterState(0x01000088, "_status2", {}, 2),

    // Z2BGM_HORSE_BATTLE over the main music: changeSubBgmStatus's status 1 plays children 11 to 13 in place of 0 and
    // 1. Status 2 plays the start's children and writes 2 to port 9, which the sequence reads.
    State(0x01000004, "_status1", {{0x0003, 0.0f}}),
    State(0x01000004, "_status2", {{0x3800, 0.0f}}, 2),

    // Z2BGM_FACE_OFF_BATTLE2: subBgmStart's status 0 leaves out children 11 to 13. Status 1 plays them in place of 0
    // and 10.
    State(0x01000096, "", {{0x3800, 0.0f}}),
    State(0x01000096, "_status1", {{0x0401, 0.0f}}),

    // Z2BGM_RODEO: subBgmStart's status 1 plays child 9 without 10 to 14. Status 2 plays 10 to 14 in place of 9.
    State(0x01000029, "", {{0x7c00, 0.0f}}, 1),
    State(0x01000029, "_status2", {{0x0200, 0.0f}}, 2),

    // Z2BGM_VARIANT: subBgmStart's status 1 only writes the port. A few frames after the piece starts, its sequence
    // calls the game's onVariantBgmJumpEnd (0x802b299c, 0x8029694c), which sets children 8 to 10 to 0. Status 2 plays
    // them in place of child 0. A later status 1 leaves them out again. Status 3 only writes 3 to port 9.
    State(0x01000039, "", {{0x0700, 0.0f}}, 1),
    State(0x01000039, "_status2", {{0x0001, 0.0f}}, 2),
    State(0x01000039, "_status3", {{0x0700, 0.0f}}, 3),

    // Z2BGM_BATTLE_TWILIGHT: startBattleBgm's status 0 or 1 plays child 4. Status 2 plays 2 to 5, and status 3 plays 5
    // to 12. The Wii's code writes the status to port 9 too (0x80296930). The GameCube's doesn't.
    State(0x0100001b, "", {{0x1fec, 0.0f}}),
    State(0x0100001b, "_status2", {{0x1fc0, 0.0f}}),
    State(0x0100001b, "_status3", {{0x001c, 0.0f}}),

    // Z2BGM_BATTLE_NORMAL: startBattleBgm leaves out children 2, 3, 12 and 14 until an enemy has found Link
    // (setBattleSearched), and child 1 while the enemy isn't close (setBattleDistState; the child is the byte at
    // 0x8045086c and 0x8051fc5c). setBattleSeqState's state 2 plays every child, with 1 on port 9. Its state 3 then
    // writes 2 to port 9.
    State(0x0100000f, "", {{0x500e, 0.0f}}),
    State(0x0100000f, "_near", {{0x500c, 0.0f}}),
    State(0x0100000f, "_searched", {{0x0002, 0.0f}}),
    State(0x0100000f, "_near_searched", {}),
    LaterState(0x0100000f, "_port9_1", {}, 1),
    LaterState(0x0100000f, "_port9_2", {}, 2),
    // setBattleSeqState's state 3 straight after state 1 writes 3 to port 9.
    LaterState(0x0100000f, "_port9_3", {}, 3),

    // The fights below change only on port 9. A boss's module writes the status as the fight goes on, through
    // changeBgmStatus or changeSubBgmStatus. The sequence reads the port, waits for its next bar and jumps to the
    // section of that status. A state writes the status after the first frame, so it plays that section after the first
    // bar.
    // A status that a piece's sequence reads but no module writes isn't a state. Z2BGM_IB_MBOSS has the sequence of
    // Z2BGM_MAGNE_GORON, and its actor (d_a_e_th) writes no status.
    //
    // Z2BGM_BOSSBABA_2: b_bq_damage (d_a_b_bq) writes 2, then 1. The sequence's 3 has no writer.
    LaterState(0x0100000e, "_status1", {}, 1),
    LaterState(0x0100000e, "_status2", {}, 2),

    // Z2BGM_MAGNE_GORON: glab writes 2 and 3, and jump writes 1 and 4 (d_a_e_gob).
    LaterState(0x0100001d, "_status1", {}, 1),
    LaterState(0x0100001d, "_status2", {}, 2),
    LaterState(0x0100001d, "_status3", {}, 3),
    LaterState(0x0100001d, "_status4", {}, 4),

    // Z2BGM_SUMO: the wrestlers' moves write 1 to 3, a win writes 4 and a loss writes 5 (d_a_npc_wrestler).
    LaterState(0x01000024, "_status1", {}, 1),
    LaterState(0x01000024, "_status2", {}, 2),
    LaterState(0x01000024, "_status3", {}, 3),
    LaterState(0x01000024, "_status4", {}, 4),
    LaterState(0x01000024, "_status5", {}, 5),

    // Z2BGM_BOSSFIREMAN_0: e_fm_down and damage_check write 1 and 2 (d_a_e_fm). The sequence ignores the module's 3 and
    // 4.
    LaterState(0x01000025, "_status1", {}, 1),
    LaterState(0x01000025, "_status2", {}, 2),

    // Z2BGM_BOSS_OCTAEEL_1: fish_normal (d_a_b_ob) writes 2 while Link is close to the boss, then 1. A 1 plays what the
    // start plays.
    LaterState(0x01000031, "_status2", {}, 2),

    // Z2BGM_BOSS_ZANT: changeBgmStatus maps a status s before it writes it: 2s - 1 up to 6, and 2(s - 7) from 7 to 12.
    // The boss's create (d_a_b_zant) passes the phase of the fight, 1 to 6, right after bgmStart. Its executeRoomChange
    // passes the phase plus 7, later. So the port gets 1 to 11, one or the other way for each phase. The sequence plays
    // a section for each. It doesn't clear port 9 as it starts.
    State(0x01000062, "_status1", {}, 1),
    LaterState(0x01000062, "_status2", {}, 2),
    State(0x01000062, "_status3", {}, 3),
    LaterState(0x01000062, "_status4", {}, 4),
    State(0x01000062, "_status5", {}, 5),
    LaterState(0x01000062, "_status6", {}, 6),
    State(0x01000062, "_status7", {}, 7),
    LaterState(0x01000062, "_status8", {}, 8),
    State(0x01000062, "_status9", {}, 9),
    LaterState(0x01000062, "_status10", {}, 10),
    State(0x01000062, "_status11", {}, 11),

    // Z2BGM_TN_MBOSS and Z2BGM_TN_MBOSS_LV9: subBgmStart writes status 1 as it starts them, and the sequence clears it
    // in its first tick. The boss's executeChangeDemo and executeZakoChangeDemo (d_a_b_tn) write 1 again later.
    LaterState(0x0100006c, "_status1", {}, 1),
    LaterState(0x010000a8, "_status1", {}, 1),

    // Z2BGM_HARAGIGANT_BTL02: mBattle2_spinner_damage_check (d_a_b_ds) writes 2. executeBattle2Damage writes 1, which
    // plays what the start plays.
    LaterState(0x0100008c, "_status2", {}, 2),

    // Z2BGM_DRAGON_BTL02: week_hit_check (d_a_b_dr) writes 2. executeWeekHit writes 1, which plays what the
    // start plays.
    LaterState(0x01000090, "_status2", {}, 2),

    // Z2BGM_GOMA_BTL01: b_gm_drop writes 1 as the boss lands, and 2 as it climbs back or takes a hit (d_a_b_gm).
    LaterState(0x01000094, "_status1", {}, 1),
    LaterState(0x01000094, "_status2", {}, 2),
};

// Twilight Princess HD's states: the table above, with two changes that HD's code makes (Zelda.rpx of the USA release,
// version 81). Runs of HD's changeBgmStatus (0x02c2286c), changeSubBgmStatus (0x02c214e4) and sceneBgmStart
// (0x02c1954c) on a CPU emulator logged each call of setChildTrackVolume (0x02c20b24) and Z2SoundStarter::setPortData
// (0x02c2e138). The runs covered every piece from 0x01000000 to 0x010000c0, with statuses 0 to 17 and ten more up to
// 6150. The other states match the table. So do the four that change only port 9.
//
// Z2BGM_DUNGEON_LV9_02: HD tests the status for 8 and 0x33, not 12 (0x02c241f4 and 0x02c241fc). Either gives room
// 8's volumes. Any other status up to 3000 plays children 3 to 6 at 0.3 without 0 to 2 and 7 to 9. sceneBgmStart passes
// the room as the status. So that state starts the piece in every room but 8 and 51. From 3001 to 3150, HD gives room
// 12's volumes. From 3150 to 6150, HD moves them to 6150's.
//
// Z2BGM_BATTLE_TWILIGHT: HD's changeSubBgmStatus writes the status to port 9. The Wii's code does too. The piece's
// sequence doesn't read the port.
//
// Z2BGM_VS_GANON_04: HD's statuses from 9 up play children 12 and 13 at 0.3 without 14 and 15. No caller that passes
// them for this piece was found. The table leaves them out.
inline constexpr auto kTwilightPrincessHdStates = []
{
    constexpr uint32_t kHyruleCastle = 0x0100003f;
    constexpr uint32_t kTwilightBattle = 0x0100001b;
    constexpr std::array castle{
        State(kHyruleCastle, "", {{0x0387, 0.0f}, {0x0078, 0.3f}}),
        State(kHyruleCastle, "_status8", {{0x0007, 0.0f}, {0x0078, 0.5f}, {0x0200, 0.8f}}, 8),
        State(kHyruleCastle, "_status3150", {{0x0278, 0.0f}, {0x0180, 0.5f}}, 3150),
        State(kHyruleCastle, "_status6150", {{0x0007, 0.0f}}, 6150),
    };
    constexpr std::size_t kCastleBefore = 3;
    std::array<MusicState, kTwilightPrincessStates.size() - kCastleBefore + castle.size()> hd{};
    std::size_t n = 0;
    for (const MusicState& state : kTwilightPrincessStates)
    {
        if (state.sound == kHyruleCastle)
        {
            if (state.suffix == "_status8")
            {
                for (const MusicState& c : castle)
                {
                    hd[n++] = c;
                }
            }
            continue;
        }
        hd[n] = state;
        if (state.sound == kTwilightBattle)
        {
            hd[n].status_port = state.suffix.empty() ? 1 : state.suffix == "_status2" ? 2 : 3;
        }
        n++;
    }
    return hd;
}();

// The HD table fills every place. A place left empty stops the build here, and a write past the end stops it above.
static_assert(kTwilightPrincessHdStates.back().sound != 0 && kTwilightPrincessHdStates[0].sound != 0);

} // namespace ntdlib::jaudio

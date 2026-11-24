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

#include "game_profile_rows.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"

namespace ntdlib::ntr
{
namespace
{

// The commands of a row of kTrackStates, in the order that the game's code sends them: the player's volume, the tracks'
// volumes, the tempo, the pitch, then the mutes. The player's volume is its level in 0.1 dB (the driver's kDecibel) in
// the player's parameter at 0x06 (2 bytes). A track's volume is a fader in 0.1 dB of the tracks' parameter at 0x0A (2
// bytes): 0 for 127, -32768 for 0. The tempo's ratio is the player's parameter at 0x1A (2 bytes), and the pitch the
// tracks' parameter at 0x0C (2 bytes). A mute is the driver's command 8 with the tracks and mode 1, or mode 0 for none.
// The sequence plays on the driver's player 0.
snd::CommandList TrackCommands(const TrackState& state)
{
    using snd::CommandId;
    constexpr uint32_t kTwoBytes = 2u << snd::kTrackParameterSizeShift;
    snd::CommandList commands;

    if (state.volume >= 0)
    {
        const int16_t level = snd::tables::kDecibel[std::min<int16_t>(state.volume, 127)];
        commands.push_back({.id = CommandId::kPlayerParameter,
                            .args = {0, snd::kPlayerFaderOffset, static_cast<uint32_t>(int32_t{level}), 2}});
    }
    if (state.on != 0)
    {
        commands.push_back(
            {.id = CommandId::kTrackParameter, .args = {kTwoBytes, state.on, snd::kTrackFaderOffset, 0}});
    }
    if (state.off != 0)
    {
        commands.push_back(
            {.id = CommandId::kTrackParameter, .args = {kTwoBytes, state.off, snd::kTrackFaderOffset, 0xFFFF8000}});
    }
    if (state.tempo_ratio != 256)
    {
        commands.push_back(
            {.id = CommandId::kPlayerParameter, .args = {0, snd::kPlayerTempoRatioOffset, state.tempo_ratio, 2}});
    }
    if (state.pitch_tracks != 0)
    {
        commands.push_back({.id = CommandId::kTrackParameter,
                            .args = {kTwoBytes, state.pitch_tracks, snd::kTrackPitchOffset,
                                     static_cast<uint32_t>(static_cast<int32_t>(state.pitch))}});
    }
    if (state.muted != 0)
    {
        commands.push_back({.id = CommandId::kMuteTrack, .args = {0, state.muted, snd::kMuteNoStop}});
    }
    if (state.unmuted != 0)
    {
        commands.push_back({.id = CommandId::kMuteTrack, .args = {0, state.unmuted, snd::kMuteOff}});
    }

    return commands;
}

// The Legend of Zelda: Spirit Tracks: the BGM manager (0x020B5198, in overlay 0's data) starts every piece of music on
// player 0 through 0x0206C988 (request, fade, volume, mixer state). Before the start, it replaces some requests with
// another sequence:
//
// - By the story's flags (the word at 0x14 of the flags that 0x027E09B8 points to) and the scene (the word that
//   0x027E09A4 points to), from 0x0206C9CC to 0x0206CB70: SEQ_18 plays SEQ_23 in scene 5 without flag 0x20 and in
//   scene 7 without flag 0x80. SEQ_19 plays nothing with flag 0x2000000. SEQ_37 to SEQ_40 play SEQ_41 with flag
//   0x4000000 and without flag 0x4. SEQ_44 and SEQ_45 play SEQ_46 and SEQ_47 without flag 0x20, and SEQ_50 and SEQ_51
//   play SEQ_52 and SEQ_53 without flag 0x80. SEQ_60 plays SEQ_59 with flag 0x2000 of the flags' word at 0x1C. SEQ_76
//   plays SEQ_77 outside scenes 19 and 66. SEQ_97 keeps SEQ_96 playing when SEQ_96 plays already. Each of these plays
//   the other label as that label's conversion gives it. The rows below therefore leave them out.
// - Then, from 0x0206CBC8 to 0x0206CF44, SEQ_34, 36, 38, 40, 43, 45, 47, 49, 51 and 53 play the sequence before
//   them (SEQ_45 plays SEQ_44, for instance), each in a state of the manager's mixer from 5 to 14. The rows below give
//   each request's state. The archive gives each pair the same file and bank. SEQ_36, 43, 45, 47, 49, 51 and 53 have a
//   volume of 40 in the archive. The sequences that play in their place have volumes of 110, 126, 108, 108, 110, 96
//   and 96. Where the sequence plays already, the
//   manager fades the mixer to the state without a new start (0x0206D160).
// - A new start of SEQ_39 takes state 18 (0x0206CCCC). A new start of SEQ_27 takes state 20, of SEQ_87 and 88 state 30,
//   and of SEQ_96 and 97 state 28 (0x0206CFD4 to 0x0206D014). A new start of SEQ_33, 35, 37, 42, 44, 46, 48, 50 or
//   52 takes state 0 (0x0206CC38 to 0x0206CF08). Every other request takes the state that its caller gives.
//
// The mixer's states are the table at 0x020AA534 (kSpiritTracksMixer): 32 states of 20 bytes, read as `index * 20`
// (0x0206AC64 to 0x0206ACB0). The mixer sets the player's volume (NNS_SndPlayerSetVolume at 0x02002D74: the level adds
// to the archive's volume), the tempo's ratio (0x02002E98), the pitch of every track (0x02002E38, 0 in every state) and
// the volume of each track (0x02002E08). A start in a state sets the state at once (the mixer's 0x0206B32C).
//
// 0x0206C988 takes each request with the scene's number (loaded at 0x0206CB50 and 0x0206CA78) and the flags' word, and
// starts the sequence through the handle at 0x020B51A0 (NNS_SndPlayerGetSeqNo at 0x02002F04 reads it back).
// MixerCommands below gives the commands that each start sends. The European releases use the same archive, code and
// table, 0x20 bytes higher. Only the addresses in their literal pools differ.
struct MixerState
{
    uint8_t volume; // the player's volume, from 0 to 127
    uint8_t tempo;  // the tempo's ratio less 128
    std::array<uint8_t, 16> tracks;
};

constexpr std::array<MixerState, 32> kSpiritTracksMixer = {{
    {127, 128, {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}},
    {95, 128, {127, 110, 127, 127, 127, 0, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}},
    {70, 128, {0, 74, 0, 127, 127, 0, 127, 127, 0, 90, 127, 127, 0, 0, 127, 127}},
    {50, 122, {0, 74, 0, 0, 0, 127, 127, 127, 0, 65, 0, 0, 0, 0, 127, 127}},
    {50, 124, {0, 127, 127, 127, 0, 0, 127, 127, 127, 0, 0, 127, 127, 127, 127, 127}},
    {90, 128, {0, 0, 0, 0, 65, 0, 0, 116, 116, 127, 127, 127, 127, 127, 127, 127}},
    {80, 125, {74, 127, 127, 127, 127, 0, 0, 0, 0, 127, 127, 127, 127, 127, 127, 127}},
    {90, 128, {127, 0, 90, 118, 127, 127, 127, 0, 90, 127, 127, 127, 127, 127, 127, 127}},
    {80, 128, {127, 120, 127, 127, 0, 110, 0, 0, 127, 0, 127, 127, 127, 127, 127, 127}},
    {90, 128, {0, 0, 127, 100, 53, 105, 53, 0, 127, 127, 127, 127, 127, 127, 127, 127}},
    {80, 128, {127, 0, 127, 0, 0, 127, 0, 0, 127, 127, 127, 127, 127, 127, 127, 127}},
    {80, 128, {127, 0, 127, 0, 0, 127, 0, 0, 127, 127, 127, 127, 127, 127, 127, 127}},
    {77, 128, {0, 65, 65, 127, 0, 95, 100, 0, 0, 66, 127, 127, 127, 127, 127, 127}},
    {74, 128, {0, 65, 127, 127, 127, 62, 0, 90, 0, 0, 127, 127, 127, 127, 127, 127}},
    {74, 128, {0, 65, 127, 127, 127, 62, 0, 90, 0, 0, 127, 127, 127, 127, 127, 127}},
    {110, 128, {127, 0, 127, 0, 127, 0, 127, 127, 0, 127, 0, 127, 127, 127, 127, 127}},
    {110, 135, {127, 92, 127, 92, 127, 92, 127, 127, 92, 127, 92, 127, 127, 127, 127, 127}},
    {127, 150, {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}},
    {100, 125, {0, 127, 0, 88, 0, 127, 70, 127, 0, 127, 127, 127, 127, 127, 127, 127}},
    {127, 128, {127, 127, 127, 127, 0, 0, 127, 0, 0, 127, 127, 127, 127, 127, 127, 127}},
    {127, 128, {127, 127, 0, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 0, 0}},
    {127, 128, {127, 127, 127, 127, 127, 127, 127, 127, 0, 0, 127, 0, 0, 0, 127, 127}},
    {107, 127, {127, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {107, 128, {127, 0, 127, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {112, 128, {0, 127, 127, 0, 0, 127, 0, 127, 0, 0, 0, 0, 0, 0, 0, 0}},
    {127, 129, {0, 127, 127, 127, 127, 127, 0, 127, 0, 0, 0, 0, 0, 0, 0, 0}},
    {127, 128, {127, 127, 127, 127, 127, 127, 127, 0, 0, 127, 127, 127, 127, 127, 127, 127}},
    {127, 128, {127, 0, 0, 0, 0, 0, 46, 127, 127, 127, 127, 127, 127, 127, 127, 127}},
    {127, 128, {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 0, 127, 127}},
    {127, 128, {127, 0, 0, 127, 127, 127, 0, 127, 127, 127, 127, 127, 127, 127, 127, 127}},
    {127, 128, {0, 127, 127, 127, 127, 0, 127, 127, 127, 127, 0, 127, 0, 127, 127, 127}},
    {127, 135, {127, 127, 127, 0, 127, 127, 127, 127, 127, 127, 127, 127, 127, 0, 127, 127}},
}};

// A request that Spirit Tracks' BGM manager starts as `played`, a sequence of the archive with the archive's volume
// `volume`, in the mixer's state `state`. Besides the manager's rows, four pieces start in a state that their scene's
// code gives. Overlay 96 starts SEQ_6 in state 19 (0x02179D04). Overlay 23 starts SEQ_153 in state 26, or 27 once one
// of three checks of 0x020EF444 passes (0x020EB410 to 0x020EB470). Overlay 91 starts SEQ_80 in state 0 (0x02174720),
// and its frame (0x02175410) fades the mixer at once to the band of a distance (+0x880): state 15 above 0x4333, 16
// above 0x2800 and 17 below. The base takes the far band. The train (overlay 26) sets the state of SEQ_18 by its speed
// (+0x08) and a timer (+0x10), at 0x0212E604: 1 at a speed of 193 or more, 2 while it runs slower, and 3 once it has
// stood for 60 frames or runs backwards at -28 or less. The manager keeps the train state at 0x48 (0x0212E764) and
// starts SEQ_18 in it (0x0206C3C8), 1 at first. The base takes state 1, the speed the train keeps most. Of the 78
// calls of 0x0206C988, 75 pass state 0. The others are SEQ_6's, SEQ_153's and SEQ_96's (state 28, as the manager
// gives it). SEQ_65 plays in the mix that its handler gives (kSpiritTracksScenes): the base takes state 22.
struct ManagerPiece
{
    std::string_view label;
    std::string_view played;
    uint8_t volume;
    uint8_t state;
};

constexpr std::string_view kSpiritTracksArchive = "SoundData/final_sound_data.sdat";
constexpr ManagerPiece kSpiritTracksPieces[] = {
    {"SEQ_34", "SEQ_33", 110, 5},  {"SEQ_36", "SEQ_35", 110, 6},    {"SEQ_38", "SEQ_37", 100, 8},
    {"SEQ_40", "SEQ_39", 104, 7},  {"SEQ_43", "SEQ_42", 126, 9},    {"SEQ_45", "SEQ_44", 108, 10},
    {"SEQ_47", "SEQ_46", 108, 11}, {"SEQ_49", "SEQ_48", 110, 12},   {"SEQ_51", "SEQ_50", 96, 13},
    {"SEQ_53", "SEQ_52", 96, 14},  {"SEQ_39", "SEQ_39", 104, 18},   {"SEQ_27", "SEQ_27", 120, 20},
    {"SEQ_87", "SEQ_87", 90, 30},  {"SEQ_88", "SEQ_88", 90, 30},    {"SEQ_96", "SEQ_96", 120, 28},
    {"SEQ_97", "SEQ_97", 120, 28}, {"SEQ_6", "SEQ_6", 118, 19},     {"SEQ_18", "SEQ_18", 112, 1},
    {"SEQ_80", "SEQ_80", 127, 15}, {"SEQ_153", "SEQ_153", 104, 26}, {"SEQ_65", "SEQ_65", 88, 22},
};

// A state that a scene's code fades a playing piece to, and holds for a stretch (0x0206D160, through the mixer's
// 0x0206A878): the arrangement `suffix` of the piece `label`. The piece has the archive's volume `volume`.
//
// - SEQ_6: overlay 96 fades to state 0 while an object's state (+0x0A of 0x02070540's result) is 0 or 4, and back to
//   19 otherwise (0x02179D0C to 0x02179D2E).
// - SEQ_18 and SEQ_23: the train states above. For SEQ_23, states 1 and 2 give 0, and 3 gives 4 (0x0212E714).
// - SEQ_27: overlay 92 holds state 21 while a timer (+0x240) runs between 1 and 541, and 20 otherwise (0x02177A9C to
//   0x02177AD4).
// - SEQ_80: the distance's two nearer bands (overlay 91's 0x02175410).
// - SEQ_87 and SEQ_88: in scenes 33 and 78 (0x0211C6FC), overlay 34 fades to state 31 in one phase of its object
//   (0x0211B3CC) and to 30 in two others (0x0211BDB4, 0x0211C180).
// - SEQ_96 and SEQ_97: overlay 75 fades to state 29 after a count in the first phase of its fight (0x021586C8 to
//   0x02158700), and to 28 again in a later phase (0x02158B80).
// - SEQ_153: overlay 23 fades to state 27 at an event (0x020ECF54), and to 26 again when a countdown ends
//   (0x020ED9B4).
// - SEQ_65: the mixer's handler (0x0206B668) sets the whole mix in each frame by a height at 0x9C of the mixer. Only
//   overlay 102 writes that height (0x0206BDB4). Below 0x3998 the player is silent, and above it the volume of state 22
//   rises with the height. States 22 to 25 play as they are at the heights 0xACD0, 0xE6A9, 0x14759 and 0x1A669 (the
//   constants at 0x0206B9B4 to 0x0206B9C4, and runs of the USA release). Every height between two of them gives a mix
//   of two states. The base takes state 22, and _HEIGHT2 to _HEIGHT4 states 23 to 25. The mixes between have no
//   arrangement.
//
// Each of these states silences or lowers some tracks of the piece, and the scene holds it while its condition holds.
// A fade between two states takes 8 to 60 frames (0x0206A8A0 to 0x0206A960). An arrangement sets the state from the
// start.
struct SceneState
{
    std::string_view label;
    std::string_view suffix;
    uint8_t volume;
    uint8_t state;
};

constexpr SceneState kSpiritTracksScenes[] = {
    {"SEQ_6", "_FULL", 118, 0},      {"SEQ_18", "_SLOW", 112, 2},    {"SEQ_18", "_STOPPED", 112, 3},
    {"SEQ_23", "_STOPPED", 100, 4},  {"SEQ_27", "_PHASE2", 120, 21}, {"SEQ_80", "_CLOSER", 127, 16},
    {"SEQ_80", "_CLOSEST", 127, 17}, {"SEQ_87", "_PHASE2", 90, 31},  {"SEQ_88", "_PHASE2", 90, 31},
    {"SEQ_96", "_PHASE2", 120, 29},  {"SEQ_97", "_PHASE2", 120, 29}, {"SEQ_153", "_PHASE2", 104, 27},
    {"SEQ_65", "_HEIGHT2", 88, 23},  {"SEQ_65", "_HEIGHT3", 88, 24}, {"SEQ_65", "_HEIGHT4", 88, 25},
};

// SEQ_18's handler in the mixer (0x0206B3C8) mutes tracks in each frame by the state (0x0206B4A0 to 0x0206B510): state
// 1 unmutes 0xFFDF and mutes track 5, state 2 unmutes every track, and state 3 unmutes 0x02E2 and mutes 0x3101. A
// muted track starts no notes. The game's SDK sends a mute with mode 3 (runs of the USA release). The commands of
// `state`, or none.
snd::CommandList Seq18Mutes(uint8_t state)
{
    using snd::CommandId;
    switch (state)
    {
    case 1:
        return {{.id = CommandId::kMuteTrack, .args = {0, 0xFFDF, snd::kMuteOff}},
                {.id = CommandId::kMuteTrack, .args = {0, 0x0020, snd::kMuteStop}}};
    case 2:
        return {{.id = CommandId::kMuteTrack, .args = {0, 0xFFFF, snd::kMuteOff}}};
    case 3:
        return {{.id = CommandId::kMuteTrack, .args = {0, 0x02E2, snd::kMuteOff}},
                {.id = CommandId::kMuteTrack, .args = {0, 0x3101, snd::kMuteStop}}};
    default:
        return {};
    }
}

// Adds the commands that set the tracks' faders of `state` to `commands`: one command for each volume, for the tracks
// that take it, in the order of their first track. Without `full_too`, the tracks at 127 keep their fader at 0.
void AddTrackFaders(const MixerState& state, bool full_too, snd::CommandList& commands)
{
    constexpr uint32_t kTwoBytes = 2u << snd::kTrackParameterSizeShift;
    uint16_t done = 0;
    for (std::size_t track = 0; track < state.tracks.size(); track++)
    {
        const uint8_t volume = state.tracks[track];
        if ((volume == 127 && !full_too) || (done >> track & 1) != 0)
        {
            continue;
        }

        uint16_t tracks = 0;
        for (std::size_t other = track; other < state.tracks.size(); other++)
        {
            if (state.tracks[other] == volume)
            {
                tracks |= static_cast<uint16_t>(1u << other);
            }
        }
        done |= tracks;
        const int32_t fader = snd::tables::kDecibel[volume];
        commands.push_back({.id = snd::CommandId::kTrackParameter,
                            .args = {kTwoBytes, tracks, snd::kTrackFaderOffset, static_cast<uint32_t>(fader)}});
    }
}

// The driver's commands of a row of kSpiritTracksPieces: the player's volume, the tracks' volumes, then the tempo. The
// player's level is the sum of the levels of the archive's volume and the state's volume (kDecibel). It replaces the
// level that the archive gives the label. A row whose sequence is its label, in a state at full volume, keeps the
// archive's level. The tracks at full volume keep their fader at 0.
snd::CommandList MixerCommands(const ManagerPiece& piece)
{
    using snd::CommandId;
    const MixerState& state = kSpiritTracksMixer[piece.state];
    snd::CommandList commands;

    if (piece.played != piece.label || state.volume != 127)
    {
        const int32_t level = snd::tables::kDecibel[piece.volume] + snd::tables::kDecibel[state.volume];
        commands.push_back(
            {.id = CommandId::kPlayerParameter, .args = {0, snd::kPlayerFaderOffset, static_cast<uint32_t>(level), 2}});
    }

    AddTrackFaders(state, false, commands);
    if (state.tempo != 128)
    {
        commands.push_back(
            {.id = CommandId::kPlayerParameter, .args = {0, snd::kPlayerTempoRatioOffset, state.tempo + 128u, 2}});
    }

    return commands;
}

// The driver's commands that set every value of `mix` for a sequence with the archive's volume `archive_volume`: the
// player's level, the tracks' faders, then the tempo's ratio.
snd::CommandList FullMixCommands(uint8_t archive_volume, const MixerState& mix)
{
    using snd::CommandId;
    const int32_t level = snd::tables::kDecibel[archive_volume] + snd::tables::kDecibel[mix.volume];
    snd::CommandList commands = {
        {.id = CommandId::kPlayerParameter, .args = {0, snd::kPlayerFaderOffset, static_cast<uint32_t>(level), 2}}};
    AddTrackFaders(mix, true, commands);
    commands.push_back(
        {.id = CommandId::kPlayerParameter, .args = {0, snd::kPlayerTempoRatioOffset, mix.tempo + 128u, 2}});

    return commands;
}

// SEQ_39's handler in the mixer (0x0206B528) runs in each frame in which the handle plays SEQ_39 (the mixer's update at
// 0x0206B348), the player's tick counter (NNS_SndPlayerGetTick at 0x02002FD4) is below 504 and the mixer's flag at 0x99
// is clear. It sets the mixer's volume, tempo and pitch to state 1's, with every track at 127, and sends them. It then
// sets a fade of 15 frames toward the state that the start gave. The fade takes one step in each frame (0x0206A9FC):
// the value plus (target - value) * step / 15 (0x0206AC28). The handler's next run sets the values back. The fade
// therefore goes on only once the counter reaches 504, the end of the intro. The first frame that reads a count of 504
// or more sends step 0, the intro's values, and the next 15 frames send steps 1 to 15. A start sets its state and
// clears the flag (0x0206B32C). The manager sets the flag after it writes variable 0 (0x0206D088). SEQ_39 and SEQ_40
// therefore play their intro (_INTRO, variable 0 at -1) in this mix, then fade to their state. A start at section 0
// plays in the state from its start. The game runs a frame in every second frame of the screen: on a run, its lists
// come every 33.43 ms.
//
// SEQ_18's handler (0x0206B3C8) does the same while the counter is below 456 and the flag at 0x98 is clear, with a fade
// of 85 frames (0x0206B47C). The request path's start, 0x0206C3C8, sets the flag (0x0206C40C). A start with nothing
// playing goes through 0x0206C988 from 0x0206C2A0. It clears the flag and passes state 0. From then on, the train sound
// update (overlay 26, 0x0212E584) sets the state in each frame by the train speed (0x0212E604, above). The profile
// assumes that the game starts the piece this way where the train stands at a station, and that the scene leaves the
// flag at +0x64 clear. A set flag would set state 1 once. No run of that scene shows it yet. A standing train sets
// state 2 at once, and state 3 once the speed has held at 0 for 60 frames. The timer counts the frames since the speed
// last changed (0x0212E594 to 0x0212E5AC). Where the train has stood before the piece starts, state 3 comes earlier.
// The mutes of SEQ_18 go on in each frame, in the intro too. A run of the USA release (SEQ_18 started through
// 0x0206C988 in state 0, the train sound update called in each frame at a speed held at 0 from the start) sent the
// mutes of state 2 from the start, those of state 3 in the 61st frame after the start, then the fade to state 3. _INTRO
// gives that intro: every track playing, the mutes of state 3 from the 61st frame, and the fade to state 3.
struct IntroHandler
{
    std::string_view played;
    uint32_t ticks;
    int fade_frames;
    bool train_start = false; // the train at a station sets the state after the start (AddTrainStart)
};

constexpr uint8_t kIntroState = 1;
constexpr uint8_t kTrainStartState = 2;
constexpr uint8_t kTrainStandingState = 3;
constexpr std::size_t kTrainStandingFrames = 60;
constexpr IntroHandler kIntroHandlers[] = {{"SEQ_39", 504, 15}, {"SEQ_18", 456, 85, true}};
constexpr uint32_t kSpiritTracksVideoFrames = 2;

// Gives `intro`, the _INTRO arrangement of a row of kSpiritTracksPieces that `handler` plays, the mix of the intro and
// the fade that follows it: to the state of a standing train for a train start, and to the row's state otherwise.
void AddIntroFade(const ManagerPiece& piece, const IntroHandler& handler, ProfileArrangement& intro)
{
    MixerState from = {
        .volume = kSpiritTracksMixer[kIntroState].volume, .tempo = kSpiritTracksMixer[kIntroState].tempo, .tracks = {}};
    from.tracks.fill(127);
    const MixerState& to = kSpiritTracksMixer[handler.train_start ? kTrainStandingState : piece.state];
    const snd::CommandList intro_mix = FullMixCommands(piece.volume, from);
    intro.commands.insert(intro.commands.end(), intro_mix.begin(), intro_mix.end());

    // Step 0 repeats the intro's values. The frame that sends it starts the fade.
    TickCommands fade = {.tick = handler.ticks, .video_frames = kSpiritTracksVideoFrames};
    for (int step = 1; step <= handler.fade_frames; step++)
    {
        const auto at = [step, &handler](uint8_t a, uint8_t b)
        {
            return static_cast<uint8_t>(a + (b - a) * step / handler.fade_frames);
        };
        MixerState mix = {.volume = at(from.volume, to.volume), .tempo = at(from.tempo, to.tempo), .tracks = {}};
        for (std::size_t track = 0; track < mix.tracks.size(); track++)
        {
            mix.tracks[track] = at(from.tracks[track], to.tracks[track]);
        }
        fade.frames.push_back(FullMixCommands(piece.volume, mix));
    }
    intro.tick_commands.push_back(std::move(fade));
}

// Gives `intro`, the _INTRO arrangement of SEQ_18, the mutes of the train states at a station: those of state 2 from
// the start, and those of state 3 in the frame after the train has stood for kTrainStandingFrames frames.
void AddTrainStart(ProfileArrangement& intro)
{
    const snd::CommandList start = Seq18Mutes(kTrainStartState);
    intro.commands.insert(intro.commands.end(), start.begin(), start.end());

    // The frame that starts the piece reads the tick counter and sends nothing. Each later frame sends one list.
    TickCommands standing = {.tick = 0, .video_frames = kSpiritTracksVideoFrames};
    standing.frames.resize(kTrainStandingFrames);
    standing.frames.push_back(Seq18Mutes(kTrainStandingState));
    intro.tick_commands.push_back(std::move(standing));
}

// A value that a game's code writes to a variable of a sequence (0 to 15 the player's, 16 to 31 the global ones) as it
// starts the sequence. A row without a suffix gives the base, and another row an arrangement.
struct VariableState
{
    std::string_view games; // game codes, or their first three letters for every region, with a space between them
    std::string_view file;  // the path of the image's file that holds the archive
    std::string_view label;
    std::string_view suffix;
    uint8_t variable;
    int16_t value;
};

// Spirit Tracks: 17 sequences of player 0 open with a jump to one of 16 sections by local variable 0, then write the
// section that they reach to local variable 1 as they play. With -1, the archive's value, no jump is taken: they play
// an intro of 144 to 1,560 ticks, then section 0 and on to their loop. The BGM manager writes variable 0 after a start
// (NNS_SndPlayerWriteVariable at 0x02002F9C, in the same list of commands as the start), in three cases:
//
// - 0x0206D0CC starts a piece and writes the section that the manager keeps at 0x2C (modulo 16, 0 for a negative
//   value). 0x0206C3C8 starts SEQ_18 and SEQ_23 that way. So do the field's music (0x02080A14), overlay 21's
//   0x020F8ED0, overlay 31's 0x020FD73C and 0x020FDBA4, and overlay 63's 0x0215CA88. 0x0206D114 stops a piece and keeps
//   its variable 1 at 0x2C.
// - 0x0206C988 writes the kept section after a start of SEQ_39 or 40 that doesn't follow SEQ_37 or 38, of SEQ_37 or 38
//   after SEQ_39, 40 or 54, and of SEQ_41 after SEQ_7, SEQ_33 or SEQ_304. It sets the kept section to 0 first
//   (0x0206CF50 to 0x0206CFAC). It writes it after a start of SEQ_64 after SEQ_76 or 110, and of SEQ_67 or 69 after
//   SEQ_77 (0x0206CB88 to 0x0206CBC4).
// - A change of scene (0x0206C7DC) keeps a section for the next start (0x4C of the manager). The next start then writes
//   it (0x0206CFB0).
//
// SEQ_39 and SEQ_40 therefore start at section 0, without the intro, on every start but those after SEQ_37 or 38:
// _INTRO gives those. The others start with the intro, and _RESUME gives a start at section 0. A start at a later
// section plays the same loop from another point. The profile leaves those out. A start of SEQ_18 through 0x0206C3C8
// writes the kept section. 0x0206C464 sets that section when the piece stops. SEQ_39 after SEQ_18, SEQ_37 after SEQ_39,
// SEQ_41 after SEQ_33, SEQ_67 and 69 after SEQ_77, and SEQ_64 after SEQ_110 write 0. SEQ_37 after SEQ_1 writes nothing.
//
// The game's request of an effect (0x0206DB24, id = archive * 1024 + index) writes 180 to variable 0 after it starts
// id 2 (0x0206DBC4 to 0x0206DBD8). SEQARC_0_2 waits that many ticks before its end (0xA1 0x80 0x00 in its track 0).
constexpr VariableState kVariableStates[] = {
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_18", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_18", "_INTRO", 0, -1},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_23", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_24", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_37", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_38", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_39", "", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_39", "_INTRO", 0, -1},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_40", "", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_40", "_INTRO", 0, -1},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_41", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_59", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_64", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_66", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_67", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_68", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_69", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_70", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_71", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQ_351", "_RESUME", 0, 0},
    {"BKIE BKIP", kSpiritTracksArchive, "SEQARC_0_2", "", 0, 180},
};

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

// Adds the rows of kStreamLocks for the sequence at `place`, in the table's order.
void AddStreamLocks(const Place& place, GameProfile& profile)
{
    for (const StreamLock& row : kStreamLocks)
    {
        if (Names(row.game, row.file, row.label, place))
        {
            profile.channel_locks.push_back(row.lock);
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

// Adds the commands of Spirit Tracks' BGM manager for the sequence at `place` (kSpiritTracksPieces) to the base's.
void AddManagerPieces(const Place& place, GameProfile& profile)
{
    for (const ManagerPiece& piece : kSpiritTracksPieces)
    {
        if (Names("BKIE BKIP", kSpiritTracksArchive, piece.label, place))
        {
            snd::CommandList commands = MixerCommands(piece);
            if (piece.played == "SEQ_18")
            {
                const snd::CommandList mutes = Seq18Mutes(piece.state);
                commands.insert(commands.end(), mutes.begin(), mutes.end());
            }
            profile.commands.insert(profile.commands.end(), commands.begin(), commands.end());
        }
    }
}

// Adds the rows of kVariableStates for the sequence at `place`: the base's values, and an arrangement for each other
// row.
void AddVariableStates(const Place& place, GameProfile& profile)
{
    for (const VariableState& state : kVariableStates)
    {
        if (!Names(state.games, state.file, state.label, place))
        {
            continue;
        }

        if (state.suffix.empty())
        {
            SetVariable(profile.settings.variables, state.variable, state.value);
        }
        else
        {
            profile.arrangements.push_back(
                {.suffix = std::string(state.suffix), .variables = {{state.variable, state.value}}});
        }
    }
}

// Adds an arrangement for each row of kSpiritTracksScenes of the sequence at `place`: every value of the row's state,
// and SEQ_18's mutes.
void AddSceneStates(const Place& place, GameProfile& profile)
{
    for (const SceneState& scene : kSpiritTracksScenes)
    {
        if (!Names("BKIE BKIP", kSpiritTracksArchive, scene.label, place))
        {
            continue;
        }

        snd::CommandList commands = FullMixCommands(scene.volume, kSpiritTracksMixer[scene.state]);
        if (scene.label == "SEQ_18")
        {
            const snd::CommandList mutes = Seq18Mutes(scene.state);
            commands.insert(commands.end(), mutes.begin(), mutes.end());
        }
        profile.arrangements.push_back({.suffix = std::string(scene.suffix), .commands = std::move(commands)});
    }
}

// Gives the _INTRO arrangement of each row of kSpiritTracksPieces that an intro handler plays the intro's mix and the
// fade after it (AddIntroFade), and the mutes of a train at a station where the handler is a train start
// (AddTrainStart).
void AddIntroFades(const Place& place, GameProfile& profile)
{
    for (const ManagerPiece& piece : kSpiritTracksPieces)
    {
        if (!Names("BKIE BKIP", kSpiritTracksArchive, piece.label, place))
        {
            continue;
        }

        for (const IntroHandler& handler : kIntroHandlers)
        {
            if (piece.played != handler.played)
            {
                continue;
            }

            for (ProfileArrangement& arrangement : profile.arrangements)
            {
                if (arrangement.suffix != "_INTRO")
                {
                    continue;
                }

                AddIntroFade(piece, handler, arrangement);
                if (handler.train_start)
                {
                    AddTrainStart(arrangement);
                }
            }
        }
    }
}

// The Legend of Zelda: Phantom Hourglass (USA, AZEE): at sea, overlay 15's music object (0x02185180, updated each frame
// through 0x02184A44) plays SEQ_101 on the first music handle, and SEQ_104 or SEQ_103 on the second. The sound
// manager's start refuses SEQ_101 (0x020D7060). The object is therefore the piece's one path. The object sets the
// tracks' volumes by a mode from the table at 0x02187264, in two groups for each handle (the table at 0x02187244:
// SEQ_101's tracks 0x033A and 0x04D5, sent in that order). A volume of 0 also mutes the group (0x0201F81C), with mode 3
// in runs of the USA release. Modes 0 and 1 set SEQ_101's tempo ratio to 250 (0x02185390, 0x021854FC).
//
// - Mode 2, while the ship sails (its speed at +0x78 is above 0, 0x0213A35C): both groups at 127. SEQ_101 plays as the
//   archive gives it.
// - Mode 1, while the ship stands: 0x033A at 64, and 0x04D5 at 0 and muted. Track 4 is in both groups and takes the
//   second's value. _STOPPED gives this mode.
// - Mode 0 (the start, and two modes of the touch screen) sets both groups to 48 at a ratio of 250. It has no
//   arrangement. Modes 3 and 4 (an enemy within 8.0) silence SEQ_101 and play SEQ_104 at 127 or 64: levels of SEQ_104.
// - Modes 5 and 6, in the fog (0x027E10A8): SEQ_101 stops, and SEQ_103 plays with tracks 0xFF7F at 127 or 64. Each
//   frame sets SEQ_103's track 7 to 0 (0x02185154 to 0x02185174) unless the Ghost Ship's distance sets it
//   (0x021855B8): 64 to 127 from 220.0 to 40.0. The base silences track 7, and _GHOST_SHIP plays it at 127.
//
// Changes within modes 0 to 4 ramp by steps of 2 or 6 in each update (0x02185758). An arrangement sets the mode from
// the start. Nothing else in the game calls the setters of a track's volume or mute, or of the tempo.
//
// Runs of the USA release ran the object's code on ntdlib's DS machine, at the title: moved into free memory, with
// its inputs set. The ship at rest gave _STOPPED's mix. A sailing ship gave SEQ_101 as the archive gives it, at a
// ratio of 256. The fog gave SEQ_103 with tracks 0xFF7F at 127 and track 7 at 0, and track 7 at 127 with the Ghost
// Ship's volume (0x021855B8) run at a distance of 0.
constexpr std::string_view kPhantomHourglassArchive = "SoundData/final_sound_data.sdat";

// Adds Phantom Hourglass's sea modes for the sequence at `place`: SEQ_101's _STOPPED, and SEQ_103's track 7 with its
// _GHOST_SHIP.
void AddSeaModes(const Place& place, GameProfile& profile)
{
    using snd::CommandId;
    constexpr uint32_t kTwoBytes = 2u << snd::kTrackParameterSizeShift;

    const auto fader = [](uint8_t volume)
    {
        return static_cast<uint32_t>(int32_t{snd::tables::kDecibel[volume]});
    };

    if (Names("AZEE", kPhantomHourglassArchive, "SEQ_101", place))
    {
        snd::CommandList stopped = {
            {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x033A, snd::kTrackFaderOffset, fader(64)}},
            {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x04D5, snd::kTrackFaderOffset, fader(0)}},
            {.id = CommandId::kMuteTrack, .args = {0, 0x04D5, snd::kMuteStop}},
            {.id = CommandId::kPlayerParameter, .args = {0, snd::kPlayerTempoRatioOffset, 250, 2}},
        };
        profile.arrangements.push_back({.suffix = "_STOPPED", .commands = std::move(stopped)});
    }
    else if (Names("AZEE", kPhantomHourglassArchive, "SEQ_103", place))
    {
        profile.commands.push_back(
            {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x0080, snd::kTrackFaderOffset, fader(0)}});
        profile.arrangements.push_back(
            {.suffix = "_GHOST_SHIP",
             .commands = {
                 {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x0080, snd::kTrackFaderOffset, fader(127)}}}});
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

    AddStreamLocks(place, profile);
    AddTrackStates(place, profile);
    AddVariables(place, profile);
    AddManagerPieces(place, profile);
    AddVariableStates(place, profile);
    AddSceneStates(place, profile);
    AddIntroFades(place, profile);
    AddSeaModes(place, profile);

    return profile;
}

} // namespace ntdlib::ntr

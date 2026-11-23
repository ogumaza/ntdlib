// SPDX-License-Identifier: MIT

// The effects that known 3DS games put on nn::snd's two aux buses. A game sets its effects up at run time, and its
// sound archive doesn't hold them. A tool that plays or converts a game's sounds without the game's code takes them
// from here. The effects themselves are ntdlib::ctr_fx's models and the DSP firmware's delay and reverb.
//
// Two tables list the games. The first finds a game by the program ID of its code. The second finds a sound archive by
// its SHA-1: a loose archive, or one in a game's add-on content, comes with no program ID of the program that plays it.
// Each entry cites the places in the game's code that set the effects up. Another region's release of a game has
// another program ID and often another archive. It has no entry until someone has read its code.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::ctr::snd
{

// The aux buses, A and B.
constexpr int kAuxBusCount = 2;

// An effect on an aux bus, and the library whose code it follows. The numbers of the kinds stay as they are: a file
// format may store them.
struct AuxEffect
{
    enum class Kind : uint32_t
    {
        kNwSndReverb = 1,     // nw::snd's FxReverb
        kNnSndReverb = 2,     // nn::snd's FxReverb, as games from 2013 on link it
        kNnSndReverb2011 = 3, // nn::snd's FxReverb, as Ocarina of Time 3D links it
        kDelay = 4,           // nw::snd's FxDelay, or nn::snd's: the two compute the same
        kFirmwareDelay = 5,   // the DSP firmware's delay, set up by nn::snd
        kNwSndReverb2011 = 6, // nw::snd's FxReverb, as StreetPass Mii Plaza links it (2011)
        kFirmwareReverb = 7,  // the reverb that nn::snd sets up in the DSP firmware
    };

    // Whether `kind` is one of the kinds above.
    static bool Known(uint32_t kind);

    // Whether the effect is a reverb, with the parameters in `reverb`, rather than a delay, with those in `delay`.
    bool IsReverb() const;

    // Whether the firmware runs the effect, rather than the ARM11.
    bool InFirmware() const;

    Kind kind = Kind::kNwSndReverb;
    ctr_fx::ReverbParam reverb;
    ctr_fx::DelayParam delay;
};

// The effects on each bus, in the order in which they process a frame. A game's nw::snd effects come first, in the
// order of nw::snd's list, then at most one of nn::snd's. The firmware's delay and reverb, at most one of each on each
// bus, run in the firmware wherever they stand in the list.
using AuxEffectLists = std::array<std::vector<AuxEffect>, kAuxBusCount>;

// The reverb that Pokémon X's field module starts on aux bus A: nw::snd's FxReverb. The game runs it while the player
// is in the field. Its entry in the table by program ID gives it for every sound of the game.
constexpr ctr_fx::ReverbParam kPokemonXFieldReverb = {.early_reflection_time = 0,
                                                      .fused_time = 4600,
                                                      .pre_delay_time = 1,
                                                      .coloration = 0.5f,
                                                      .damping = 0.91f,
                                                      .filter_sizes = {1120, 1440, 4960},
                                                      .early_gain = 0.3f,
                                                      .fused_gain = 0.73f};

// The effects that a game puts on the aux buses, from the table by program ID.
struct GameEffects
{
    const char* game; // the game's name, for a tool's messages

    // The releases whose code was read.
    std::vector<uint64_t> program_ids;

    // The SHA-1s of the sound archives of those releases that hold the game's music, in 40 lowercase hex digits, for a
    // tool that sees only the archive (EffectsWhilePlaying).
    std::vector<std::string_view> archive_sha1s;

    AuxEffectLists buses;
};

// The entry of the game whose code has the program ID `program_id`, or nullptr.
const GameEffects* FindGameEffects(uint64_t program_id);

// An effect that a known game runs on the aux buses for some of its sequences alone, by the game's number for it
// (SequenceStart::game_effect).
struct SequenceEffect
{
    uint8_t number;
    AuxEffectLists buses;
};

// A group of sounds that a known game plays with the effect `number` (SequenceEffect) in place of the archive's
// effects: the sounds whose labels start with `prefix`. It applies only where the sound's start (SequenceStart) gives
// the archive's effects.
struct LabelEffect
{
    std::string_view prefix;
    uint8_t number;
};

// The effects that the game of a known sound archive puts on the aux buses, from the table by SHA-1.
struct ArchiveEffects
{
    const char* game;  // the game's name, for a tool's messages
    const char* build; // the release whose archive the entry is, for the note on another build
    const char* label; // a sound that this archive holds and the others of the table don't
    AuxEffectLists buses;

    // The effects that the game runs for some sequences alone.
    std::vector<SequenceEffect> game_effects = {};

    // The sounds that play with one of `game_effects` by their labels.
    std::vector<LabelEffect> label_effects = {};
};

// The entry of the archive whose SHA-1 is `archive_sha1`, in 40 lowercase hex digits, or nullptr. The table by program
// ID doesn't count.
const ArchiveEffects* FindArchiveEffects(std::string_view archive_sha1);

// The effect that the game of `effects` plays the sound `label` with, where the sound's start gives `game_effect`
// (SequenceStart::game_effect): the number of the first LabelEffect whose prefix `label` starts with, when
// `game_effect` is SequenceStart::kArchiveEffects. Otherwise, and for a null `effects`, `game_effect`.
uint8_t GameEffectOf(const ArchiveEffects* effects, std::string_view label, uint8_t game_effect);

// The entry whose label `archive` holds, when no entry has the archive's SHA-1 `archive_sha1`: another build of a known
// archive. The tables of the effects and of the games' starts don't cover such a build. Otherwise nullptr.
const ArchiveEffects* FindOtherBuild(const sar::SoundArchive& archive, std::string_view archive_sha1);

// The effects that the game of the archive whose SHA-1 is `archive_sha1` runs on the aux buses while it plays the sound
// `label`, where the sound's start gives `game_effect` (SequenceStart::game_effect). For an entry of the table by
// SHA-1, GameEffectOf picks the effect: one of the entry's game_effects, or the archive's effects for
// SequenceStart::kArchiveEffects and for a number that the entry doesn't list. A game of the table by program ID whose
// archive_sha1s lists the archive gives its effects. nullptr for an archive that neither table lists.
const AuxEffectLists* EffectsWhilePlaying(std::string_view archive_sha1, std::string_view label, uint8_t game_effect);

} // namespace ntdlib::ctr::snd

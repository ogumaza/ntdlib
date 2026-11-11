// SPDX-License-Identifier: MIT

// The sound effects of a game on Jac, the C JAudio of Pikmin, as a ripper's game mode starts them, and the game's
// catalogue of effects in its main.dol. Jac plays a piece of music by its number in a sequence index in the game's
// code. The index's first two files play the effects.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dol.h"

namespace ntdlib::jaudio
{

// A part of a Jac game's piece in parts: its number, and the wave system and group that the game loads for it. The
// sound library plays that piece from its start-up. The game plays a part by writing the part's number to a port of the
// piece's root track. The game loads the part's wave group first.
struct JacPart
{
    uint8_t number = 0;
    uint8_t system = 0;
    uint8_t group = 0;
};

// How a Jac game starts a sound effect: by its number as an effect of the system or of the player, by an action of an
// event, or as the sound of Pikmin's formation. An event's number is its type << 12 | the action. The formation's
// number is 0. The formation's sound plays while the player moves the formation. A variant gives the move on ports 2
// and 3 of the formation's track.
enum class JacEffect : uint8_t
{
    kSystem = 1,
    kPlayer = 2,
    kEvent = 3,
    kFormation = 4,
};

// The files of a Jac game's sequence index that play its effects: the effects sequence plays the system's, the
// player's and the formation's, and the events' sequence the events'. A piece of music has a higher number.
inline constexpr uint32_t kJacEffectsSequence = 0;
inline constexpr uint32_t kJacEventsSequence = 1;
inline constexpr uint32_t kJacFirstPiece = 2;

// A Jac effect's sound ID: 0x80000000 | kind << 24 | number << 8 | the file that plays it. The number takes 16 bits.
constexpr uint32_t JacEffectId(JacEffect kind, uint32_t number)
{
    const uint32_t file = kind == JacEffect::kEvent ? kJacEventsSequence : kJacEffectsSequence;
    return 0x80000000u | static_cast<uint32_t>(kind) << 24 | (number & 0xffffu) << 8 | file;
}

// Whether a Jac sound's ID is an effect's.
constexpr bool IsJacEffect(uint32_t id)
{
    return (id & 0xffu) < kJacFirstPiece;
}

// A Jac game's catalogue of sound effects in its main.dol, read in place: `count` rows of 20 bytes at `table`. A row
// gives a number in its word at +4, a pointer to the effect's name at +8 and how the game starts the effect at +16:
// kSystemRow for an effect of the system, kPlayerRow for one of the player, or the type of the event whose action the
// number is. The table at `event_offsets` holds each event type's first entry in the table of actions that follows
// it. An action's entry gives the number of the effect that it plays, in its halfword at +4. The DOL must outlive the
// object.
class JacCatalogue
{
public:
    static constexpr int32_t kSystemRow = -2;
    static constexpr int32_t kPlayerRow = -1;
    static constexpr uint32_t kEventTypes = 8;

    // A row: its number, its effect's name and how the game starts the effect.
    struct Row
    {
        uint32_t number = 0;
        std::string name;
        int32_t kind = 0;
    };

    // Throws xsflib::FormatError if the catalogue or the table of event offsets runs past its section.
    JacCatalogue(const Dol& dol, uint32_t table, uint32_t count, uint32_t event_offsets);

    uint32_t Count() const
    {
        return count_;
    }

    // Row `i`, below Count(). Throws xsflib::FormatError if the effect's name has no end in its section.
    Row RowAt(uint32_t i) const;

    // Event type `type`'s first entry in the table of actions. `type` is below kEventTypes.
    uint32_t FirstEntry(uint32_t type) const;

    // The number of the effect that the action at `entry` plays, or nothing if the entry lies past the table's section.
    std::optional<uint16_t> EffectOf(std::size_t entry) const;

private:
    const Dol* dol_;
    std::span<const uint8_t> rows_;
    std::span<const uint8_t> offsets_;
    std::span<const uint8_t> actions_;
    uint32_t count_;
};

// The effects that a ripper lists for a Jac game: the game's catalogue (JacCatalogue, none if `table` is 0), the
// effects that the game's code starts itself, and the effects of the catalogue that the ripper leaves out. Both lists
// hold sound IDs (JacEffectId). An event of type `sounding_event` (0 for none) starts an effect as the game creates it.
// A rip plays that type's effects on an event of type `quiet_event` instead.
struct JacEffectRules
{
    uint32_t table = 0;
    uint32_t count = 0;
    uint32_t event_offsets = 0;
    std::span<const uint32_t> other_effects = {};
    std::span<const uint32_t> left_out = {};
    uint32_t sounding_event = 0;
    uint32_t quiet_event = 0;
};

// An effect that a ripper lists: its sound ID and its name.
struct JacEffectSound
{
    uint32_t id = 0;
    std::string name;
};

// The effects that a ripper lists by `rules`, from the catalogue in `dol`: the catalogue's in its order, then the
// other effects, each named by `name_of_other`. Actions of events of different types can play the same effect. The
// first row that plays an effect names it. The list leaves out `rules.left_out` and any ID that it has already.
// Throws xsflib::FormatError if the catalogue is damaged or has a row of unknown kind.
std::vector<JacEffectSound> ListJacEffects(const Dol& dol, const JacEffectRules& rules,
                                           const std::function<std::string(uint32_t id)>& name_of_other);

// Pikmin's effects, in the GameCube release (USA, disc version 1) and in the Wii release, New Play Control! Pikmin.
// The Wii release's code makes the same calls as the GameCube release's. The addresses are in the GameCube release's
// main.dol, then in the Wii release's.
// - The game's code plays four more effects of the system: 25 as the demo of a found ship part ends (0x8001a748), and
//   30, 31 and 36 in that demo (Jac_StartPartsFindDemo, 0x8001b120). The game's set-up of scene 0 plays one of four
//   voices of the player by the time (0x80019c58). Voice 0x800c is the one that the GameCube release plays in 123
//   cases of 128. The game passes 0x1800c. Jac_PlayOrimaSe reads only the low 16 bits. The game's code plays the sound
//   of the formation as the player moves the formation.
// - Effect 0 of the system stops the effects. The effects sequence plays nothing for 15. 18 plays part 8 of demobgm,
//   which a ripper lists as music. 32 plays a stream. The cases of Jac_PlaySystemSe (Wii 0x80019c00) for them match.
// - Jac_CreateEvent (Wii 0x8001ac00) starts the ship's idling (action 4) as it creates an event of type 7, the ship's.
inline constexpr uint32_t kPikminOtherEffects[] = {
    JacEffectId(JacEffect::kSystem, 25), JacEffectId(JacEffect::kSystem, 30),     JacEffectId(JacEffect::kSystem, 31),
    JacEffectId(JacEffect::kSystem, 36), JacEffectId(JacEffect::kPlayer, 0x800c), JacEffectId(JacEffect::kFormation, 0),
};
inline constexpr uint32_t kPikminLeftOutEffects[] = {
    JacEffectId(JacEffect::kSystem, 0),
    JacEffectId(JacEffect::kSystem, 15),
    JacEffectId(JacEffect::kSystem, 18),
    JacEffectId(JacEffect::kSystem, 32),
};
inline constexpr uint32_t kPikminSoundingEvent = 7;
inline constexpr uint32_t kPikminQuietEvent = 1;

} // namespace ntdlib::jaudio

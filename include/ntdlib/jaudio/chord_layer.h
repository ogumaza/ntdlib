// SPDX-License-Identifier: MIT

// Donkey Kong Jungle Beat's chord effects beside a piece of music: the records of the chord database that an object of
// a stage asks for while the piece plays (jungle_beat_states.h, JungleBeatChordLayer). Each record's player
// (chord_player.h) steps on the music's beats, over the chord that the piece's position gives in its chord file
// (chord_files.h). The addresses are in the main.dol of the GameCube release (USA).
//
// An object asks for its record once a game frame (0x80256fcc). The first ask starts the record through startChordSe
// (0x8024c4c8) once the chords are in. Later asks keep the player alive (0x8024c6c0): it stays busy after its last
// step, so the record plays once. The objects that start their effects once (0x80256f48) start them again only when
// neither a player nor the effect plays (0x8024cba4). The record's last step ends neither.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/jaudio/chord_files.h"
#include "ntdlib/jaudio/chord_keys.h"
#include "ntdlib/jaudio/chord_player.h"

namespace ntdlib::jaudio
{

// What a frame does to one of the layer's effects.
struct ChordLayerSound
{
    enum class Action : uint8_t
    {
        kStart, // stops the effect, then starts `sound` at `level` with `ports` written before its first tick
        kStop,  // stops the effect
    };

    Action action = Action::kStart;
    std::size_t effect = 0; // the record's index in the layer
    uint32_t sound = 0;
    float level = 1.0f;
    std::map<uint8_t, uint16_t> ports;
};

// The effect that a record of `sound` plays at stage `stage`: startChordSe plays JA_SE_IST_AUTO_COMP_E_PIANO_T
// (0x50063) for JA_SE_IST_AUTO_COMP_ALTO_SAX (0x50042) and JA_SE_IST_AUTO_COMP_TRUMPET (0x50045) at stage 0x2c, and
// JA_SE_IST_AUTO_COMP_E_PIANO (0x50062) at stages 0x35 and 0x41.
uint32_t ChordEffectAtStage(uint32_t sound, uint32_t stage);

class ChordLayer
{
public:
    // The records `records` of the chord database `data`, over the chord file `chords`, at stage `stage`. `data` has to
    // outlive the layer. Throws FormatError for a record that the database doesn't hold.
    ChordLayer(std::span<const uint8_t> data, ChordFile chords, uint32_t stage, std::span<const uint32_t> records);

    // A note with a voice above 0 of the piece's track 0 or of its child 0, which the rhythm's parser reads
    // (0x80247194).
    void Note(uint8_t key, uint8_t velocity)
    {
        rhythm_.Note(key, velocity);
    }

    // A game frame, after the audio frame: the objects' asks, then the rhythm's frame process. `position` is port 0xe
    // of the child that gives the piece's position (0x8024b57c), or nothing before the child writes it. The records
    // start in the first frame with a position.
    std::vector<ChordLayerSound> Frame(std::optional<uint16_t> position);

    std::size_t Effects() const
    {
        return effects_.size();
    }

private:
    struct Effect
    {
        uint32_t record = 0;
        ChordPlayer player;
        uint32_t sound = 0;
        uint32_t shared = 0; // the record's third word: the offset of the byte that its keys go to, or 0
        bool started = false;
        int list = -1;                              // the timer whose list the player waits in
        std::optional<uint16_t> last_step_position; // the position at the effect's last step (+0x3c of the player)
    };

    ChordMoment MomentAt(std::optional<uint16_t> position, const Effect& effect);
    void Apply(std::size_t index, std::optional<uint16_t> position, ChordMoment& moment,
               const std::vector<ChordStep>& steps, std::vector<ChordLayerSound>& sounds);

    std::span<const uint8_t> data_;
    ChordDatabase database_;
    ChordFile chords_;
    std::vector<Effect> effects_;
    ChordRhythm rhythm_;
    ChordRandom random_;
    uint8_t last_root_ = 60;
    std::map<uint32_t, uint8_t> shared_keys_;
    std::optional<uint16_t> last_position_;
};

} // namespace ntdlib::jaudio

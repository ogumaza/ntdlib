// SPDX-License-Identifier: MIT

// Donkey Kong Jungle Beat's chord layers as companions of their pieces (SequenceSound::companion): the chord effects
// that ntdlib/jaudio/chord_layer.h starts beside the music.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "ntdlib/jaudio/chord_layer.h"
#include "ntdlib/jaudio/jungle_beat_states.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/sequence_export/jaudio/performance.h"

namespace ntdlib::sequence_export::jaudio
{

// The game's chord files and the effects that its chord layers play.
struct ChordLayerData
{
    std::vector<uint8_t> database;                  // ChordSeDataBase.csd
    std::vector<uint8_t> archive;                   // JaiChord.arc
    std::vector<ntdlib::jaudio::ArchiveFile> files; // the archive's files, which point into `archive`
    std::map<uint32_t, SequenceSound> sounds;       // the chord effects, by sound ID
};

class ChordCompanion : public Companion
{
public:
    // The companion of `layer`. Throws FormatError if the piece has no chord file or the database lacks a record.
    ChordCompanion(std::shared_ptr<const ChordLayerData> data, const ntdlib::jaudio::JungleBeatChordLayer& layer);

    std::unique_ptr<Companion> Clone() const override;
    void Note(uint8_t key, uint8_t velocity) override;
    std::vector<CompanionSound> Frame(const std::function<std::optional<uint16_t>(int, int)>& port) override;

    // The sound IDs that the layer's records can start, at its stage.
    static std::vector<uint32_t> Sounds(const ntdlib::jaudio::ChordDatabase& database,
                                        const ntdlib::jaudio::JungleBeatChordLayer& layer);

private:
    std::shared_ptr<const ChordLayerData> data_;
    int position_child_ = 0;
    ntdlib::jaudio::ChordLayer layer_;
};

} // namespace ntdlib::sequence_export::jaudio

// SPDX-License-Identifier: MIT

#include "sequence_export/jaudio/chord_companion.h"

#include <algorithm>
#include <string>

#include "xsflib/binary.h"

namespace ntdlib::sequence_export::jaudio
{

namespace
{

// The pieces whose position is at port 0xe of child 1 of track 0, not child 0 (0x8024b57c).
constexpr uint32_t kPositionInChild1[] = {0x01000006, 0x01000008, 0x01000014, 0x01000015};

ntdlib::jaudio::ChordFile ChordFileOf(const ChordLayerData& data, uint32_t piece)
{
    const char* name = ntdlib::jaudio::JungleBeatChordFileOf(piece);
    const auto file =
        name ? std::ranges::find(data.files, std::string(name), &ntdlib::jaudio::ArchiveFile::name) : data.files.end();
    if (file == data.files.end())
    {
        throw xsflib::FormatError("JaiChord.arc has no chord file for the piece");
    }

    return ntdlib::jaudio::ReadChordFile(file->data);
}

std::vector<uint32_t> RecordsOf(const ntdlib::jaudio::JungleBeatChordLayer& layer)
{
    std::vector<uint32_t> records{layer.record};
    if (layer.second != 0)
    {
        records.push_back(layer.second);
    }
    return records;
}

} // namespace

ChordCompanion::ChordCompanion(std::shared_ptr<const ChordLayerData> data,
                               const ntdlib::jaudio::JungleBeatChordLayer& layer)
    : data_(std::move(data)),
      position_child_(std::ranges::find(kPositionInChild1, layer.piece) != std::end(kPositionInChild1) ? 1 : 0),
      layer_(data_->database, ChordFileOf(*data_, layer.piece), layer.stage, RecordsOf(layer))
{
}

std::unique_ptr<Companion> ChordCompanion::Clone() const
{
    return std::make_unique<ChordCompanion>(*this);
}

void ChordCompanion::Note(uint8_t key, uint8_t velocity)
{
    layer_.Note(key, velocity);
}

std::vector<CompanionSound> ChordCompanion::Frame(const std::function<std::optional<uint16_t>(int, int)>& port)
{
    constexpr int kPositionPort = 0xe;
    std::vector<CompanionSound> sounds;
    for (ntdlib::jaudio::ChordLayerSound& step : layer_.Frame(port(position_child_, kPositionPort)))
    {
        const auto sound = data_->sounds.find(step.sound);
        if (step.action == ntdlib::jaudio::ChordLayerSound::Action::kStart && sound == data_->sounds.end())
        {
            continue;
        }

        sounds.push_back({.action = step.action == ntdlib::jaudio::ChordLayerSound::Action::kStart
                                        ? CompanionSound::Action::kStart
                                        : CompanionSound::Action::kStop,
                          .voice = step.effect,
                          .sound = sound == data_->sounds.end() ? nullptr : &sound->second,
                          .level = step.level,
                          .ports = std::move(step.ports)});
    }

    return sounds;
}

std::vector<uint32_t> ChordCompanion::Sounds(const ntdlib::jaudio::ChordDatabase& database,
                                             const ntdlib::jaudio::JungleBeatChordLayer& layer)
{
    std::vector<uint32_t> sounds;
    for (const uint32_t record : RecordsOf(layer))
    {
        if (record < database.records.size())
        {
            sounds.push_back(ntdlib::jaudio::ChordEffectAtStage(database.records[record].sound, layer.stage));
        }
    }
    return sounds;
}

} // namespace ntdlib::sequence_export::jaudio

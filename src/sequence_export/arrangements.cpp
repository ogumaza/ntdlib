// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/arrangements.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_pieces.h"

namespace ntdlib::sequence_export
{

namespace
{

// ConvertSoundOrPiece for an archive of either format.
template <typename Archive>
Conversion ConvertSoundOrPieceOf(const Archive& archive, const ArrangedSound& at, std::span<const SoundPiece> pieces,
                                 const PerformOptions& options, const Settings& settings, const std::string& program)
{
    const SoundPiece* piece = at.arrangement ? nullptr : PieceStartingAt(pieces, at.sound);
    if (piece)
    {
        return ConvertTogether(archive, piece->sounds, piece->name, options, settings, program);
    }

    return ConvertSequence(archive, static_cast<uint32_t>(at.sound), options, settings, program);
}

} // namespace

std::vector<Sequence> WithArrangements(const std::vector<Sequence>& sequences, std::size_t sounds,
                                       const ArrangementsOf& arrangements_of, bool picked_with_sequence)
{
    std::vector<Sequence> listed;
    for (const Sequence& sequence : sequences)
    {
        listed.push_back(sequence);
        const std::vector<Arrangement> arrangements = arrangements_of(sequence.index);
        for (std::size_t k = 0; k < arrangements.size(); k++)
        {
            Sequence arranged{(k + 1) * sounds + sequence.index, sequence.name + arrangements[k].suffix};
            if (picked_with_sequence)
            {
                arranged.labels.push_back(sequence.name);
                arranged.labels.insert(arranged.labels.end(), sequence.labels.begin(), sequence.labels.end());
            }

            listed.push_back(std::move(arranged));
        }
    }

    return listed;
}

ArrangedSound ArrangedSoundAt(std::size_t index, std::size_t sounds)
{
    if (index < sounds || sounds == 0)
    {
        return {index, std::nullopt};
    }

    return {index % sounds, index / sounds - 1};
}

void SetUpArrangement(const ArrangedSound& at, const ArrangementsOf& arrangements_of, PerformOptions& options)
{
    if (!at.arrangement)
    {
        return;
    }

    const std::vector<Arrangement> arrangements = arrangements_of(at.sound);
    if (*at.arrangement >= arrangements.size())
    {
        throw std::runtime_error("sound " + std::to_string(at.sound) + " has no arrangement " +
                                 std::to_string(*at.arrangement));
    }

    if (arrangements[*at.arrangement].set_up)
    {
        arrangements[*at.arrangement].set_up(options);
    }
}

Conversion ConvertSoundOrPiece(const sar::SoundArchive& archive, const ArrangedSound& at,
                               std::span<const SoundPiece> pieces, const PerformOptions& options,
                               const Settings& settings, const std::string& program)
{
    return ConvertSoundOrPieceOf(archive, at, pieces, options, settings, program);
}

Conversion ConvertSoundOrPiece(const rsar::SoundArchive& archive, const ArrangedSound& at,
                               std::span<const SoundPiece> pieces, const PerformOptions& options,
                               const Settings& settings, const std::string& program)
{
    return ConvertSoundOrPieceOf(archive, at, pieces, options, settings, program);
}

} // namespace ntdlib::sequence_export

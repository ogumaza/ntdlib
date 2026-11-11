// SPDX-License-Identifier: MIT

// A sound's arrangements, for a converter of sound archives on the command-line frame (archive_converter.h). An
// arrangement is another state that a game plays a sound in: other tracks, other values of the variables, or other
// settings of the performance. The sound converts once more for each of its arrangements. WithArrangements lists
// arrangement k of the sound at index s at the index (k + 1) * n + s. n is the archive's count of sounds. The
// converter's convert hook turns the index back into the sound and the arrangement (ArrangedSoundAt).

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export
{

// An arrangement of a sound. Its files take the sound's name followed by `suffix`. `set_up` makes the arrangement's
// changes to the options of the performance.
struct Arrangement
{
    std::string suffix;
    std::function<void(PerformOptions&)> set_up;
};

// The arrangements of the sound at an index, in the order of the listing.
using ArrangementsOf = std::function<std::vector<Arrangement>(std::size_t sound)>;

// `sequences` of an archive of `sounds` sounds, each followed by its arrangements (`arrangements_of`). A piece of
// several sequences takes the arrangements of its first sound. With `picked_with_sequence`, --only picks an arrangement
// wherever it picks the arrangement's sequence: the arrangement takes the sequence's name and labels as its labels
// (Sequence::labels).
std::vector<Sequence> WithArrangements(const std::vector<Sequence>& sequences, std::size_t sounds,
                                       const ArrangementsOf& arrangements_of, bool picked_with_sequence = false);

// What an index from WithArrangements converts: the sound, and the arrangement's place among the sound's
// arrangements. The arrangement is nothing for the sound itself.
struct ArrangedSound
{
    std::size_t sound = 0;
    std::optional<std::size_t> arrangement;
};

// The sound and the arrangement at `index` in an archive of `sounds` sounds.
ArrangedSound ArrangedSoundAt(std::size_t index, std::size_t sounds);

// Makes the changes of the arrangement `at` to `options` (Arrangement::set_up). Changes nothing for a sound itself.
// Throws std::runtime_error if the sound has no such arrangement. The listing and the conversion then disagree.
void SetUpArrangement(const ArrangedSound& at, const ArrangementsOf& arrangements_of, PerformOptions& options);

} // namespace ntdlib::sequence_export

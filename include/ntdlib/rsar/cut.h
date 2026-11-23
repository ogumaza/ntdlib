// SPDX-License-Identifier: MIT

// Copies of an RSAR that keep only the data that some sounds play: for a model of NW4R SND that reads the archive
// (KeepSequenceData), and for the game's code that loads the archive's groups (KeepGroups). MissingData lists the data
// that a sound requires and the archive doesn't contain, and SkipIncomplete takes a rip's jobs with such a sound out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"

namespace ntdlib::rsar
{

// A copy of the archive that contains only the data that the sequences at `indices` play. That data is each sequence's
// file, and its bank's file and wave data, in the items where SoundArchive::File finds them. The header, the SYMB block
// and the INFO block keep their sizes. The FILE block contains only those items, group by group. Items of a group with
// the same bytes, such as banks that share a wave archive, share one copy. The INFO block's groups and items give their
// new places. Every other item starts at the copy's end. SoundArchive::File then skips it. Each kept file is read back
// from the copy with `options` and compared with the archive's. Throws xsflib::FormatError if one differs, or if the
// groups' entries don't come before the FILE block. Only a hand-made archive could cause either.
std::vector<uint8_t> KeepSequenceData(const SoundArchive& archive, const std::vector<uint32_t>& indices,
                                      const ReadOptions& options);

// A copy of the archive for game code that loads the groups that `loaded` marks, one flag for each of
// SoundArchive::Groups. The game reads the archive's header and its INFO block (DvdSoundArchive::LoadFileHeader and
// LoadHeader), then each loaded group's files and wave data (SoundArchiveLoader::LoadGroup). NW4R SND's stream rounds
// the last read of a group's data up to a multiple of 32 bytes. The copy keeps each of those ranges, rounded out to
// multiples of 32 bytes. The rest of the FILE block's data is zeros. The copy keeps the archive's size and layout. The
// game then reads the same bytes in the same places. The stream of a group of no bytes would read the rest of the
// archive. If the game loads such a group, the copy keeps the whole archive.
std::vector<uint8_t> KeepGroups(const SoundArchive& archive, const std::vector<bool>& loaded);

// The data that sequence `index` requires and the archive doesn't contain. The sequence or bank may be in another file,
// or cut off by truncation. Empty when nothing is missing.
std::vector<std::string> MissingData(const SoundArchive& archive, uint32_t index);

// Takes out of `jobs` the jobs of a rip whose sequences' data isn't all in the archive (MissingData). Such a job would
// play with missing instruments or channels. `sounds(job)` gives the indices of a job's sequences, and `job.label`
// names the job. Each job taken out gets a line on stderr: "skipped LABEL: not in this archive: " and what is missing.
// A job of several sequences, such as a piece, names each sequence whose data is missing, with its data in brackets.
// Returns the number of jobs taken out.
template <class Job, class Sounds>
std::size_t SkipIncomplete(std::vector<Job>& jobs, const SoundArchive& archive, const Sounds& sounds)
{
    const auto incomplete = [&archive, &sounds](const Job& job)
    {
        const std::vector<uint32_t> indices = sounds(job);
        std::string what;
        for (const uint32_t sound : indices)
        {
            const std::vector<std::string> missing = MissingData(archive, sound);
            std::string list;
            for (std::size_t i = 0; i < missing.size(); i++)
            {
                list += (i ? ", " : "") + missing[i];
            }

            // A piece names each sequence whose data is missing.
            if (!list.empty() && indices.size() > 1 && sound < archive.Sounds().size())
            {
                list = archive.Sounds()[sound].name + " (" + list + ")";
            }

            if (!list.empty())
            {
                what += (what.empty() ? "" : "; ") + list;
            }
        }

        if (what.empty())
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: not in this archive: %s\n", job.label.c_str(), what.c_str());

        return true;
    };
    return std::erase_if(jobs, incomplete);
}

} // namespace ntdlib::rsar

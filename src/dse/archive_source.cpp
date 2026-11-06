// SPDX-License-Identifier: MIT

#include "ntdlib/dse/archive_source.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/dse/sets.h"

namespace ntdlib::dse
{

std::optional<ArchiveSelection> SelectionOf(std::span<const NamedFile> files, const SequenceRef& sequence, Build build)
{
    const std::optional<SequenceSet> set = FindSequenceSet(files, sequence);
    if (!set)
    {
        return std::nullopt;
    }

    ArchiveSelection selection{.sequence = sequence, .build = build};
    for (const std::size_t bank : set->banks)
    {
        selection.bank_paths.push_back(files[bank].path);
    }
    if (set->main_bank)
    {
        selection.main_bank_path = files[*set->main_bank].path;
    }
    return selection;
}

SequenceFiles LoadedFiles::Files() const
{
    SequenceFiles files;
    files.song = song ? &*song : nullptr;
    files.effects = effects ? &*effects : nullptr;
    files.effect = effect;
    for (const std::unique_ptr<WaveBank>& bank : banks)
    {
        files.banks.push_back(bank.get());
    }
    files.main_bank = main_bank ? &*main_bank : nullptr;
    return files;
}

} // namespace ntdlib::dse

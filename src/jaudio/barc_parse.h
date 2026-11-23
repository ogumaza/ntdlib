// SPDX-License-Identifier: MIT

// The reader of a sequence index (barc.h) that ReadBarcSequenceFiles and the start-up archive's reader share.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/rarc.h"

namespace ntdlib::jaudio::detail
{

// Where an index's header holds its count of sequences, and where its entries start. kBarc is the GameCube's layout.
// kWideBarc is the layout of the Switch's release of Pikmin.
struct BarcLayout
{
    std::size_t count_at = 12;
    std::size_t entries_at = 0x20;
};

inline constexpr BarcLayout kBarc{12, 0x20};
inline constexpr BarcLayout kWideBarc{16, 0x24};

// Reads the entries of the sequence index at `at` with `r`, as ReadBarcEntries does. Throws "<what> is damaged" for an
// index without its tag or with more than 0x10000 entries.
std::vector<BarcEntry> ParseBarcEntries(const xsflib::Reader& r, std::size_t at, BarcLayout layout,
                                        const std::string& what);

// Reads the sequence index at `at` with `r`, as ReadBarcSequenceFiles does. The messages name the index `what`: "<what>
// is damaged", "<what> names a sequence outside the sequence archive", "<what> shares the data of a sequence past its
// end" and "<what> shares data in a loop".
std::vector<ArchiveFile> ParseBarc(const xsflib::Reader& r, std::size_t at, BarcLayout layout,
                                   std::span<const uint8_t> archive, const std::string& what);

} // namespace ntdlib::jaudio::detail

// SPDX-License-Identifier: MIT

// The sound archives inside a game's CPK archives (GameOptions::cpk_sound_archives).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"

namespace ntdlib::disc::detail
{

// Reads `size` bytes at `offset` in the file at `path` among a game's files. The range lies inside the file. Throws
// Error if the read fails.
using ReadFileRange = std::function<std::vector<uint8_t>(const std::string& path, uint64_t offset, std::size_t size)>;

// Adds the sound archives (.brsar) inside the CPKs (.cpk) among `out.files` to `out.files`, and wraps `out.read_file`
// to read them (see GameOptions::cpk_sound_archives). `read_range` reads the CPKs. A CPK that isn't one, or whose
// tables are damaged, adds nothing.
void AddCpkSoundArchives(GameFiles& out, const ReadFileRange& read_range);

} // namespace ntdlib::disc::detail

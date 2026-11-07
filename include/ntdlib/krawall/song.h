// SPDX-License-Identifier: MIT

// A text listing of a module: its header, its orders and every row of its patterns.

#pragma once

#include <string>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"

namespace ntdlib::krawall
{

using ntdlib::agb::Rom;

// Writes a listing of module `song` to `path`. Returns false and sets `error` if the module can't be read or the file
// can't be written.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace ntdlib::krawall

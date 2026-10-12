// SPDX-License-Identifier: MIT

// The reader settings of the jaudio module's files.

#pragma once

#include "xsflib/binary.h"

namespace ntdlib::jaudio::detail
{

// The GameCube's formats are big-endian. A string's offset has to lie inside the data.
inline constexpr xsflib::ReaderOptions kReaderOptions{.check_string_offset = true};

} // namespace ntdlib::jaudio::detail

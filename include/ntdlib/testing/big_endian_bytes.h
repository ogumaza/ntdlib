// SPDX-License-Identifier: MIT

// A builder of big-endian files for tests, such as the GameCube's: values appended in turn, and stores over the bytes
// already added.

#pragma once

#include "ntdlib/testing/byte_writer.h"

namespace ntdlib::testing
{

// Big-endian data under construction.
using BigEndianBytes = ByteWriter;

} // namespace ntdlib::testing

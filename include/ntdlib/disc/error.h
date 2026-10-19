// SPDX-License-Identifier: MIT

// The exception of ntdlib::disc and ntdlib::disc_containers. The image readers, the loaders of games and titles, and
// the decompressors throw it.

#pragma once

#include <stdexcept>

namespace ntdlib::disc
{

// A damaged or unsupported file, or a read that failed.
class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

} // namespace ntdlib::disc

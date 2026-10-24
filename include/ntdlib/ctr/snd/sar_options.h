// SPDX-License-Identifier: MIT

// How archive mode's model reads the 3DS's files with ntdlib::sar: messages for data cut short, and a stop at the
// first problem.

#pragma once

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"

namespace ntdlib::ctr
{

inline const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kCtr,
                                              xsflib::ReaderOptions{xsflib::BoundsMessage::kCutShort}, false,
                                              ntdlib::sar::Rules::kStopAtFirstProblem};

} // namespace ntdlib::ctr

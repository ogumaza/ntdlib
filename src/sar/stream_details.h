// SPDX-License-Identifier: MIT

// A stream sound's details in the sound table of an archive of the 3DS, the Wii U or the Switch, as NintendoWare's
// tools lay them out (ntdlib/sar/tables.h).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar::detail
{

// The first version whose stream sounds hold a pitch and sends.
inline constexpr uint32_t kSendsVersion = 0x00020100;

// The first version whose stream sounds all hold a reference to nothing.
inline constexpr uint32_t kNothingVersion = 0x00020200;

// The fields of a stream sound's details before its table of tracks, as the tools write them in an archive of
// `version`, with zeros where the tracks that play, the count of channels, the references, the pitch and the sends go.
std::vector<uint8_t> StreamHeader(uint32_t version, bool big_endian);

// A stream sound's details for `stream` in an archive of `flavor`: `header`, the fields before the table of tracks,
// then the table, each track and the sound's sends, as the tools lay them out (tables.h). `sends` gives whether the
// version holds the pitch and sends. The references in `header` take their offsets. Sends take 8 bytes on the Wii U
// and the Switch, and 4 on the 3DS.
std::vector<uint8_t> StreamDetailsOf(std::span<const uint8_t> header, const StreamSoundInfo& stream, bool sends,
                                     Flavor flavor);

} // namespace ntdlib::sar::detail

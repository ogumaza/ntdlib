// SPDX-License-Identifier: MIT

// The ROM sets of Seta's Aleck64 arcade board, a board built on the Nintendo 64. Every chip of a set is stored with
// each pair of bytes reversed. Put together in order, a set's chips make the image of a cartridge.

#pragma once

#include <span>
#include <string_view>

#include "xsflib/rip/rom_set.h"

namespace ntdlib::n64
{

// The chips that make the image whose SHA-1 is `image_sha1`, in lower-case hex. Empty for an image of no known set.
std::span<const xsflib::rip::RomChip> Aleck64Chips(std::string_view image_sha1);

} // namespace ntdlib::n64

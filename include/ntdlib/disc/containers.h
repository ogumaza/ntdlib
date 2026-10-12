// SPDX-License-Identifier: MIT

// The container formats of ntdlib::disc_containers: GCZ, CISO, WIA and RVZ. ntdlib::disc_containers requires zlib,
// Zstandard, liblzma and bzip2. A program that reads these formats links ntdlib::disc_containers and sets OpenContainer
// in its options: ImageOptions::open_container, or the open_container of GameOptions::image.

#pragma once

#include <filesystem>
#include <memory>

#include "ntdlib/disc/image.h"

namespace ntdlib::disc
{

// Opens an image in one of the formats that ContainerFormat names, or returns null for an image in another format.
// Throws Error.
std::unique_ptr<Image> OpenContainer(const std::filesystem::path& path, Console console);

} // namespace ntdlib::disc

// SPDX-License-Identifier: MIT

// A title for the Wii's NAND, such as a WiiWare game, from a folder that holds the title's TMD and its decrypted
// contents (GameOptions::titles). A WAD unpacker that decrypts the contents writes such a folder. A Wii's NAND keeps
// each installed title's TMD and contents in a folder too: title/<the title ID's high half>/<its low half>/content. A
// WAD holds the contents encrypted. The loader rejects a WAD. WiiBrew documents what the loader reads: the TMD ("Title
// metadata"), U8 archives ("U8 archive"), the banner ("Opening.bnr") and the boot content ("NAND Boot Program").

#pragma once

#include <filesystem>

#include "ntdlib/disc/game.h"

namespace ntdlib::disc::detail
{

// True if `folder` holds files of a title: a TMD (.tmd) or contents (.app).
bool HoldsTitle(const std::filesystem::path& folder);

// Loads the title in `folder` as a game's files (see GameOptions::titles). Throws Error.
void LoadTitle(const std::filesystem::path& folder, GameFiles& out, const GameOptions& options);

// Throws Error, with a message that says what to load instead, if the file at `path` is a WAD, or a title's TMD or one
// of its contents.
void RejectTitleFile(const std::filesystem::path& path, const GameOptions& options);

} // namespace ntdlib::disc::detail

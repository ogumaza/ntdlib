// SPDX-License-Identifier: MIT

// A game being converted: an image, and the engine that plays its music.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/n64/ead.h"
#include "ntdlib/sequence_export/n64/games.h"
#include "ntdlib/sequence_export/n64/image.h"
#include "ntdlib/sequence_export/n64/instruments.h"
#include "ntdlib/sequence_export/n64/libmus.h"
#include "ntdlib/sequence_export/n64/libultra.h"
#include "ntdlib/sequence_export/n64/sbn.h"

namespace ntdlib::sequence_export::n64
{

class Input
{
public:
    // Reads the image at `path`, or the ROM set of an arcade game: a ZIP archive or a folder of its chips. A ZIP
    // archive without a profile's chips may hold a cartridge image instead: its one entry named .z64, .v64 or .n64, in
    // any case. Bytes that start no image, a set without a profile's chips, or an image with no engine that ntdlib
    // converts, throw xsflib::FormatError. `program` names the program in the message for a ZIP archive that it can't
    // read.
    static Input Open(const std::filesystem::path& path, std::string_view program);

    std::vector<ntdlib::sequence_export::Sequence> Sequences() const;
    Conversion Convert(std::size_t index, const PerformOptions& options);

private:
    explicit Input(std::unique_ptr<Image> image);

    std::unique_ptr<Image> image_;
    const GameProfile* profile_ = nullptr;
    std::unique_ptr<LibultraGame> libultra_;
    std::unique_ptr<EadGame> ead_;
    std::unique_ptr<LibmusGame> libmus_;
    std::unique_ptr<SbnGame> sbn_;
};

} // namespace ntdlib::sequence_export::n64

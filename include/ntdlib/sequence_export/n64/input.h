// SPDX-License-Identifier: MIT

// A game being converted: an image, and the engine that plays its music.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
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
    // Reads the image at `path`. Bytes that start no image, or an image with no engine that the converter reads, throw
    // xsflib::FormatError.
    static Input Open(const std::filesystem::path& path);

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

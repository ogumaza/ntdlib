// SPDX-License-Identifier: MIT

// A DOL, the executable format of GameCube and Wii games (main.dol). A DOL has up to 7 text and 11 data sections, each
// loaded at an address, plus a BSS range and an entry point.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib
{

class Dol
{
public:
    struct Section
    {
        uint32_t address = 0;
        std::span<const uint8_t> data;
    };

    // Reads a DOL's header. Returns nothing if `file` isn't a DOL. A section that the header places outside `file`
    // makes it no DOL. With `skip_damaged_sections`, the result leaves such a section out instead. The sections point
    // into `file`. The caller must keep `file` alive while it uses the result.
    static std::optional<Dol> Parse(std::span<const uint8_t> file, bool skip_damaged_sections = false);

    // The size of the DOL whose header is `header`, the file's first 0x100 bytes: the end of the section that ends
    // last, and at least the header. The header gives it before the rest of the file is read. An empty section's offset
    // counts too.
    static uint64_t Size(std::span<const uint8_t> header);

    const std::vector<Section>& Sections() const
    {
        return sections_;
    }

    uint32_t Entry() const
    {
        return entry_;
    }

private:
    std::vector<Section> sections_;
    uint32_t entry_ = 0;
};

} // namespace ntdlib

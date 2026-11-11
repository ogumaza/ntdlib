// SPDX-License-Identifier: MIT

// A DOL, the executable format of GameCube and Wii games (main.dol). A DOL has up to 7 text and 11 data sections, each
// loaded at an address, plus a BSS range and an entry point. FindMainDol finds the main.dol of an extracted disc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    // Reads a game's main.dol as Parse does. Throws FormatError ("main.dol isn't a DOL") if `file` isn't a DOL.
    static Dol ParseMain(std::span<const uint8_t> file);

    // The size of the DOL whose header is `header`, the file's first 0x100 bytes: the end of the section that ends
    // last, and at least the header. The header gives it before the rest of the file is read. An empty section's offset
    // counts too.
    static uint64_t Size(std::span<const uint8_t> header);

    const std::vector<Section>& Sections() const
    {
        return sections_;
    }

    // The bytes from the load address `address` to the end of the section that holds it. Empty if no section holds the
    // address.
    std::span<const uint8_t> At(uint32_t address) const;

    // The bytes from the load address `address` to the end of the section that holds it, as At gives them. Throws
    // FormatError ("main.dol has no data at <what>") if no section holds the address. `what` names what the caller
    // reads there, such as "the song table".
    std::span<const uint8_t> From(uint32_t address, std::string_view what) const;

    // The `size` bytes at load address `address`. Throws FormatError, as From does, if no section holds them all.
    std::span<const uint8_t> Bytes(uint32_t address, std::size_t size, std::string_view what) const;

    // The big-endian word at load address `address` (see Bytes).
    uint32_t Word(uint32_t address, std::string_view what) const;

    // The text at load address `address`, up to its zero byte, the end of its section or `longest` bytes, whichever
    // comes first. Throws FormatError, as From does, if no section holds the address.
    std::string Text(uint32_t address, std::size_t longest, std::string_view what) const;

    uint32_t Entry() const
    {
        return entry_;
    }

private:
    std::vector<Section> sections_;
    uint32_t entry_ = 0;
};

// The main.dol of the extracted disc that holds `file`: sys/main.dol in a folder whose files folder holds `file`.
// Dolphin and wit extract a disc that way. Nothing if there's none.
std::optional<std::filesystem::path> FindMainDol(const std::filesystem::path& file);

} // namespace ntdlib

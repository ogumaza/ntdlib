// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/input.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/rip/rom_set.h"

#include "ntdlib/n64/aleck64.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

// The extensions of a cartridge image's file, in any case.
constexpr std::array<std::string_view, 3> kImageExtensions = {".z64", ".v64", ".n64"};

// Puts together the image of the ROM set at `path` from the chips of the profile that it holds. A ZIP archive that
// holds no game's chips can hold a cartridge image instead: its one entry named .z64, .v64 or .n64.
std::vector<uint8_t> ReadSetImage(const std::filesystem::path& path, std::string_view program)
{
    std::set<uint64_t> sizes;
    std::vector<xsflib::rip::RomSetGame> games;
    for (const GameProfile& profile : Profiles())
    {
        const std::span<const xsflib::rip::RomChip> chips = ntdlib::n64::Aleck64Chips(profile.sha1);
        for (const xsflib::rip::RomChip& chip : chips)
        {
            sizes.insert(chip.size);
        }

        games.push_back({profile.title, chips});
    }

    std::vector<xsflib::rip::SetFile> files;
    if (const std::optional<std::string> problem = xsflib::rip::ReadRomSet(path, sizes, program, files))
    {
        throw xsflib::FormatError(*problem);
    }

    xsflib::rip::RomSetImage image = xsflib::rip::BuildRomSetImage(games, files);
    if (image.game)
    {
        return std::move(image.rom);
    }

    // A ZIP archive with a cartridge image and no game's chips.
    std::error_code error;
    xsflib::rip::SetFile* lone = xsflib::rip::OnlyFileWithExtension(files, kImageExtensions);
    if (lone != nullptr && !std::filesystem::is_directory(path, error))
    {
        return std::move(lone->data);
    }

    throw xsflib::FormatError(image.problem);
}

} // namespace

Input::Input(std::unique_ptr<Image> image) : image_(std::move(image))
{
}

Input Input::Open(const std::filesystem::path& path, std::string_view program)
{
    std::optional<std::vector<uint8_t>> file;
    if (xsflib::rip::IsRomSet(path))
    {
        file = ReadSetImage(path, program);
    }
    else
    {
        file = ntdlib::sequence_export::ReadFile(path);
    }
    if (!file)
    {
        throw std::runtime_error("the file can't be read");
    }

    Input input(std::make_unique<Image>(std::move(*file)));
    input.profile_ = FindProfile(input.image_->Sha1());
    if (input.profile_ != nullptr && input.profile_->streamed_music)
    {
        throw xsflib::FormatError("the game streams its music as waves, with no sequences to convert");
    }

    if (input.profile_ != nullptr && input.profile_->ead != nullptr)
    {
        input.ead_ = EadGame::Open(input.image_->Bytes(), *input.profile_->ead);
    }
    else if (input.profile_ != nullptr && input.profile_->libmus != nullptr)
    {
        input.libmus_ = LibmusGame::Open(input.image_->Bytes(), *input.profile_->libmus);
    }
    else if (input.profile_ != nullptr && input.profile_->sbn != nullptr)
    {
        input.sbn_ = SbnGame::Open(input.image_->Bytes(), *input.profile_->sbn);
    }
    else
    {
        const LibultraProfile* libultra = input.profile_ != nullptr ? &input.profile_->libultra : nullptr;
        input.libultra_ = LibultraGame::Open(input.image_->Bytes(), libultra);
    }

    if (!input.libultra_ && !input.ead_ && !input.libmus_ && !input.sbn_)
    {
        throw xsflib::FormatError("the converter doesn't know this game's sound engine");
    }

    return input;
}

std::vector<ntdlib::sequence_export::Sequence> Input::Sequences() const
{
    if (libmus_)
    {
        return libmus_->Sequences();
    }

    if (sbn_)
    {
        return sbn_->Sequences();
    }

    return ead_ ? ead_->Sequences() : libultra_->Sequences();
}

Conversion Input::Convert(std::size_t index, const PerformOptions& options)
{
    if (libmus_)
    {
        return libmus_->Convert(index, options);
    }

    if (sbn_)
    {
        return sbn_->Convert(index, options);
    }

    return ead_ ? ead_->Convert(index, options) : libultra_->Convert(index, options);
}

} // namespace ntdlib::sequence_export::n64

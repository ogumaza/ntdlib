// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/input.h"

#include <utility>

#include "xsflib/binary.h"

namespace ntdlib::sequence_export::n64
{

Input::Input(std::unique_ptr<Image> image) : image_(std::move(image))
{
}

Input Input::Open(const std::filesystem::path& path)
{
    std::optional<std::vector<uint8_t>> file = ntdlib::sequence_export::ReadFile(path);
    if (!file)
    {
        throw std::runtime_error("the file can't be read");
    }

    Input input(std::make_unique<Image>(std::move(*file)));
    input.profile_ = FindProfile(input.image_->Sha1());
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

// SPDX-License-Identifier: MIT

// The image formats that store a disc without compression: plain images (ISO, NKit), WBFS and WDF. A WBFS file is a
// WBFS partition that holds one disc. The page on WDF at https://wit.wiimm.de describes WDF. containers.cpp, in
// ntdlib::disc_containers, reads GCZ, CISO, WIA and RVZ.

#include "ntdlib/disc/image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"

#include "disc/image_parts.h"

namespace ntdlib::disc
{
namespace detail
{

std::vector<fs::path> Pieces(const fs::path& path, Console console)
{
    std::vector<fs::path> pieces = {path};
    std::error_code error;
    if (console == Console::kWii && xsflib::AsciiLower(path.extension().string()) == ".wbfs")
    {
        for (int i = 1; i <= 9; ++i)
        {
            fs::path piece = path;
            piece.replace_extension(".wbf" + std::to_string(i));
            if (!fs::is_regular_file(piece, error))
            {
                break;
            }

            pieces.push_back(piece);
        }

        return pieces;
    }

    // Wiimm's tools number the continuations with one, two or three digits at least.
    for (int digits = 1; digits <= 3; ++digits)
    {
        const auto continuation = [&](int index)
        {
            std::string number = std::to_string(index);
            number.insert(0, std::max<size_t>(number.size(), static_cast<size_t>(digits)) - number.size(), '0');
            fs::path piece = path;
            piece += "." + number;

            return piece;
        };

        for (int i = 1; fs::is_regular_file(continuation(i), error); ++i)
        {
            pieces.push_back(continuation(i));
        }
        if (pieces.size() > 1)
        {
            break;
        }
    }

    return pieces;
}

} // namespace detail

namespace
{

using detail::BlockImage;
using detail::kSector;

// An ISO image, including NKit images: the disc byte for byte. NKit and scrubbed images may end early. The missing part
// reads as zeros.
class PlainImage : public Image
{
public:
    explicit PlainImage(std::unique_ptr<Source> source) : source_(std::move(source))
    {
    }

    std::string Format() const override
    {
        return "ISO";
    }

    std::optional<uint64_t> StoredSize() const override
    {
        return source_->Size();
    }

    void Read(uint64_t offset, uint8_t* out, size_t size) override
    {
        source_->ReadPadded(offset, out, size);
    }

private:
    std::unique_ptr<Source> source_;
};

// A WBFS file: a WBFS partition, usually with one disc. The head gives the partition's sector size and block size. The
// head also has a byte for each disc slot. The slots' disc infos follow from the second sector, in slot order. The
// reader takes the first used slot's info. The info holds a copy of the disc header, then one big-endian number for
// each block of the disc. Each number gives the WBFS block that stores the disc block. A number of 0 means the block
// isn't stored.
class WbfsImage : public BlockImage
{
public:
    explicit WbfsImage(std::unique_ptr<Source> source) : source_(std::move(source))
    {
        uint8_t head[12];
        source_->Read(0, head, sizeof(head));
        if (head[8] < 9 || head[8] > 12 || head[9] < 15 || head[9] > 30)
        {
            throw Error("the WBFS header is invalid");
        }

        const uint64_t sector_size = uint64_t{1} << head[8];
        block_size_ = uint64_t{1} << head[9];
        const std::vector<uint8_t> slots = source_->Read(12, static_cast<size_t>(sector_size - 12));

        // A disc's info has room for every block of a dual-layer disc. The info for each slot takes a whole number of
        // sectors.
        const uint64_t blocks_per_disc = kWiiSectorsPerDisc / (block_size_ / kSector);
        const uint64_t info_size = (0x100 + blocks_per_disc * 2 + sector_size - 1) / sector_size * sector_size;
        const auto slot = std::find_if(slots.begin(), slots.end(), [](uint8_t used) { return used != 0; });
        if (slot == slots.end())
        {
            throw Error("the WBFS file holds no disc");
        }

        const uint64_t info = sector_size + static_cast<uint64_t>(slot - slots.begin()) * info_size;
        const std::vector<uint8_t> table = source_->Read(info + 0x100, static_cast<size_t>(blocks_per_disc * 2));
        for (uint64_t i = 0; i < blocks_per_disc; ++i)
        {
            blocks_.push_back(xsflib::LoadBe16(table.data() + i * 2));
        }
    }

    std::string Format() const override
    {
        return "WBFS";
    }

protected:
    bool FetchBlock(uint64_t index, std::vector<uint8_t>& out) override
    {
        if (index >= blocks_.size() || !blocks_[index])
        {
            return false;
        }

        out = source_->Read(blocks_[index] * block_size_, static_cast<size_t>(block_size_));

        return true;
    }

private:
    static constexpr uint64_t kWiiSectorsPerDisc = 143432 * 2;

    std::unique_ptr<Source> source_;
    std::vector<uint16_t> blocks_;
};

// Wiimm's WDF, versions 1 and 2: a big-endian header and a table of extents. Each extent gives a disc offset, a file
// offset and a length. The rest of the disc is zeros.
class WdfImage : public Image
{
public:
    explicit WdfImage(std::unique_ptr<Source> source) : source_(std::move(source))
    {
        uint8_t header[0x38];
        source_->Read(0, header, sizeof(header));
        const uint32_t version = xsflib::LoadBe32(header + 8);
        if (version != 1 && version != 2)
        {
            throw Error("WDF version " + std::to_string(version) + " isn't supported");
        }

        // The table follows an 8-byte magic. In version 1, each entry starts with an unused split-file index.
        const uint32_t count = xsflib::LoadBe32(header + 0x2C);
        const uint64_t table = xsflib::LoadBe64(header + 0x30) + 8;
        const size_t entry = version == 1 ? 28 : 24;
        if (table > source_->Size() || count > (source_->Size() - table) / entry)
        {
            throw Error("the WDF chunk table is invalid");
        }

        const std::vector<uint8_t> raw = source_->Read(table, size_t{count} * entry);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* p = raw.data() + i * entry + (version == 1 ? 4 : 0);
            extents_.push_back({xsflib::LoadBe64(p), xsflib::LoadBe64(p + 8), xsflib::LoadBe64(p + 16)});
        }

        std::sort(extents_.begin(), extents_.end(),
                  [](const Extent& a, const Extent& b) { return a.disc_offset < b.disc_offset; });
    }

    std::string Format() const override
    {
        return "WDF";
    }

    void Read(uint64_t offset, uint8_t* out, size_t size) override
    {
        std::fill_n(out, size, uint8_t{0});
        for (const Extent& extent : extents_)
        {
            const uint64_t start = std::max(offset, extent.disc_offset);
            const uint64_t end = std::min(offset + size, extent.disc_offset + extent.size);
            if (start < end)
            {
                source_->Read(extent.file_offset + start - extent.disc_offset, out + (start - offset),
                              static_cast<size_t>(end - start));
            }
        }
    }

private:
    struct Extent
    {
        uint64_t disc_offset;
        uint64_t file_offset;
        uint64_t size;
    };

    std::unique_ptr<Source> source_;
    std::vector<Extent> extents_;
};

} // namespace

std::unique_ptr<PartitionData> Image::OpenPartitionData(uint64_t /*data_offset*/)
{
    return nullptr;
}

std::optional<uint64_t> Image::StoredSize() const
{
    return std::nullopt;
}

std::string ContainerFormat(std::span<const uint8_t> header)
{
    const auto starts = [&](const char* magic)
    {
        return header.size() >= 4 && std::equal(header.begin(), header.begin() + 4, magic);
    };

    if (header.size() >= 4 && xsflib::LoadLe32(header.data()) == 0xB10BC001)
    {
        return "GCZ";
    }
    if (starts("WIA\x01"))
    {
        return "WIA";
    }
    if (starts("RVZ\x01"))
    {
        return "RVZ";
    }
    if (starts("CISO"))
    {
        return "CISO";
    }

    return "";
}

std::unique_ptr<Image> OpenImage(const std::filesystem::path& path, Console console, const ImageOptions& options)
{
    auto source = std::make_unique<Source>(detail::Pieces(path, console), "image");
    uint8_t magic[8] = {};
    const size_t stored = static_cast<size_t>(std::min<uint64_t>(sizeof(magic), source->Size()));
    source->Read(0, magic, stored);
    if (const std::string format = ContainerFormat({magic, stored}); !format.empty())
    {
        if (options.open_container)
        {
            if (std::unique_ptr<Image> image = options.open_container(path, console))
            {
                return image;
            }
        }
        if (options.refuse_container)
        {
            throw Error(options.refuse_container(format));
        }
    }

    const std::string text(reinterpret_cast<const char*>(magic), sizeof(magic));
    if (console == Console::kWii && text.starts_with(detail::kWbfsMagic))
    {
        return std::make_unique<WbfsImage>(std::move(source));
    }
    if (text == detail::kWdfMagic)
    {
        return std::make_unique<WdfImage>(std::move(source));
    }

    return std::make_unique<PlainImage>(std::move(source));
}

} // namespace ntdlib::disc

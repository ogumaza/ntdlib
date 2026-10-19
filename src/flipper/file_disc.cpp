// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/file_disc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/header.h"
#include "xsflib/bytes.h"

namespace ntdlib::flipper
{
namespace
{

constexpr uint64_t kFstOffset = 0x2440; // where the apploader would be

// The files of a disc: each file's path and data.
using Files = std::vector<std::pair<std::string, std::span<const uint8_t>>>;

// A directory of the file system, with its subdirectories and files by name.
struct Directory
{
    std::map<std::string, Directory> directories;
    std::map<std::string, std::size_t> files; // index into the files given
};

// The table's entries and their names, in the order of a walk of the tree. Each entry is 12 bytes: the kind and the
// name's offset, then a file's data offset in units of `offset_unit` and its size, or a directory's parent and the
// index after its last entry.
class FstWriter
{
public:
    FstWriter(const Files& files, const std::vector<uint64_t>& offsets, uint32_t offset_unit)
        : files_(files), offsets_(offsets), offset_unit_(offset_unit)
    {
    }

    void Write(const Directory& directory, const std::string& name, uint32_t parent)
    {
        const auto index = static_cast<uint32_t>(entries_.size() / 12);
        Entry(true, name, parent, 0);
        for (const auto& [file_name, file] : directory.files)
        {
            Entry(false, file_name, static_cast<uint32_t>(offsets_[file] / offset_unit_),
                  static_cast<uint32_t>(files_[file].second.size()));
        }

        for (const auto& [directory_name, child] : directory.directories)
        {
            Write(child, directory_name, index);
        }

        // The directory's last word: the index after its entries.
        xsflib::StoreBe32(&entries_[index * 12 + 8], static_cast<uint32_t>(entries_.size() / 12));
    }

    std::vector<uint8_t> Table() const
    {
        std::vector<uint8_t> table = entries_;
        table.insert(table.end(), names_.begin(), names_.end());

        return table;
    }

private:
    void Entry(bool directory, const std::string& name, uint32_t first, uint32_t second)
    {
        uint8_t entry[12];
        xsflib::StoreBe32(&entry[0], (directory ? 0x01000000u : 0) | static_cast<uint32_t>(names_.size()));
        xsflib::StoreBe32(&entry[4], first);
        xsflib::StoreBe32(&entry[8], second);
        entries_.insert(entries_.end(), entry, entry + 12);
        names_.insert(names_.end(), name.begin(), name.end());
        names_.push_back(0);
    }

    const Files& files_;
    const std::vector<uint64_t>& offsets_;
    uint32_t offset_unit_;
    std::vector<uint8_t> entries_;
    std::vector<uint8_t> names_;
};

} // namespace

FileDisc::FileDisc(std::vector<uint8_t> header, const Files& files, uint32_t offset_unit) : header_(std::move(header))
{
    header_.resize(disc::kHeaderSize);

    // The tree of the files' paths.
    Directory root;
    for (std::size_t i = 0; i < files.size(); i++)
    {
        Directory* directory = &root;
        const std::string& path = files[i].first;
        std::size_t start = 0;
        for (std::size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1)
        {
            directory = &directory->directories[path.substr(start, slash - start)];
        }
        directory->files[path.substr(start)] = i;
    }

    // The table's size doesn't depend on where the files go. A first pass with no offsets measures it. The first file
    // then starts at the next 32 KiB boundary after the table. Each other file starts at the next 32-byte boundary
    // after the file before it.
    std::vector<uint64_t> offsets(files.size(), 0);
    FstWriter measure(files, offsets, offset_unit);
    measure.Write(root, "", 0);
    uint64_t at = (kFstOffset + measure.Table().size() + 0x7fff) & ~uint64_t{0x7fff};
    for (std::size_t i = 0; i < files.size(); i++)
    {
        offsets[i] = at;
        files_.push_back({at, files[i].second});
        at = (at + files[i].second.size() + 0x1f) & ~uint64_t{0x1f};
    }

    FstWriter writer(files, offsets, offset_unit);
    writer.Write(root, "", 0);
    fst_ = writer.Table();
    const auto fst_size = static_cast<uint32_t>((fst_.size() + offset_unit - 1) / offset_unit);
    xsflib::StoreBe32(&header_[disc::kHeaderFstOffset], static_cast<uint32_t>(kFstOffset / offset_unit));
    xsflib::StoreBe32(&header_[disc::kHeaderFstSize], fst_size);
    xsflib::StoreBe32(&header_[disc::kHeaderFstMaxSize], fst_size);
}

bool FileDisc::Read(uint64_t offset, void* out, std::size_t size)
{
    // The header, the table and the files, with zeros between them. The data ends with the last file's last 32-byte
    // block. A disc without files ends with the table.
    auto* bytes = static_cast<uint8_t*>(out);
    const uint64_t end = files_.empty() ? kFstOffset + fst_.size()
                                        : (files_.back().offset + files_.back().data.size() + 0x1f) & ~uint64_t{0x1f};
    if (offset + size > end)
    {
        return false;
    }

    std::memset(bytes, 0, size);

    const auto copy = [&](uint64_t start, std::span<const uint8_t> data)
    {
        const uint64_t from = std::max(offset, start);
        const uint64_t to = std::min(offset + size, start + data.size());
        if (from < to)
        {
            std::memcpy(bytes + (from - offset), data.data() + (from - start), to - from);
        }
    };

    copy(0, header_);
    copy(kFstOffset, fst_);
    for (const Placed& file : files_)
    {
        if (file.offset >= offset + size)
        {
            break;
        }

        copy(file.offset, file.data);
    }

    return true;
}

} // namespace ntdlib::flipper

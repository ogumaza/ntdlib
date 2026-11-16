// SPDX-License-Identifier: MIT

#include "ntdlib/dse/files.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"

#include "gfsa.h"
#include "ntdlib/disc/cx.h"
#include "ntdlib/disc/error.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/sets.h"

namespace ntdlib::dse
{

namespace
{

constexpr std::size_t kHeaderSize = 0x40;
constexpr std::size_t kNameAt = 0x20;
constexpr std::size_t kNameSize = 16;

// The SDK's LZ77 data: a type byte of 0x10 before the decompressed size, and at most this much once decompressed.
constexpr uint8_t kLz77 = 0x10;
constexpr std::size_t kMostDecompressed = 16 << 20;

struct Kind
{
    const char* magic;
    const char* extension;
};

constexpr Kind kKinds[] = {{"smdl", ".smd"}, {"swdl", ".swd"}, {"sedl", ".sed"}};

// The kind of the DSE header at `at`, or nothing.
std::optional<Kind> KindAt(std::span<const uint8_t> bytes, std::size_t at)
{
    if (bytes.size() < kHeaderSize || at > bytes.size() - kHeaderSize)
    {
        return std::nullopt;
    }

    for (const Kind& kind : kKinds)
    {
        if (std::memcmp(&bytes[at], kind.magic, 4) == 0)
        {
            return kind;
        }
    }

    return std::nullopt;
}

// The size of the DSE file at `at`, when its header is one of version 0x415 and its size fits in `bytes`.
std::optional<std::size_t> FileSizeAt(std::span<const uint8_t> bytes, std::size_t at)
{
    if (!KindAt(bytes, at) || xsflib::LoadLe32(&bytes[at + 4]) != 0 || xsflib::LoadLe16(&bytes[at + 0x0C]) != kVersion)
    {
        return std::nullopt;
    }

    const std::size_t size = xsflib::LoadLe32(&bytes[at + 8]);
    if (size < kHeaderSize || size > bytes.size() - at)
    {
        return std::nullopt;
    }

    return size;
}

// The place of the next DSE header of version 0x415 from `from` on, or the end of `bytes`. Each magic word starts
// with 's'.
std::size_t NextFile(std::span<const uint8_t> bytes, std::size_t from)
{
    for (std::size_t at = from; at + kHeaderSize <= bytes.size(); at++)
    {
        const void* s = std::memchr(&bytes[at], 's', bytes.size() - kHeaderSize + 1 - at);
        if (s == nullptr)
        {
            break;
        }

        at = static_cast<std::size_t>(static_cast<const uint8_t*>(s) - bytes.data());
        if (FileSizeAt(bytes, at))
        {
            return at;
        }
    }

    return bytes.size();
}

// The name at 0x20 of the file at `at` of `bytes`, as FindFiles gives it. The field gives no encoding, so a byte
// outside printable ASCII can't become valid UTF-8 and is replaced, like the bytes that Windows forbids in a file name.
std::string NameAt(std::span<const uint8_t> bytes, std::size_t at, const Kind& kind)
{
    std::string name;
    for (std::size_t i = 0; i < kNameSize; i++)
    {
        const uint8_t byte = bytes[at + kNameAt + i];
        if (byte == 0 || byte == 0xFF)
        {
            break;
        }

        const bool fits = byte > 0x20 && byte < 0x7F && std::strchr("/\\:*?\"<>|", byte) == nullptr;
        name.push_back(fits ? static_cast<char>(byte) : '_');
    }

    if (name.empty())
    {
        char offset[24];
        std::snprintf(offset, sizeof(offset), "0x%zX", at);
        name = offset;
    }
    // The names are often cut short at 16 bytes, inside the extension.
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos)
    {
        name += kind.extension;
    }
    else if (const std::string cut = xsflib::AsciiLower(name.substr(dot));
             cut.size() < 4 && std::string_view(kind.extension).starts_with(cut))
    {
        name = name.substr(0, dot) + kind.extension;
    }

    return name;
}

// `name`, or else `name` with _2, _3 and so on before its extension: the first that the pack hasn't given yet. A file
// of the pack can hold a name such as P32_2.SMD itself. `given` holds the names given so far, in lower case. `counts`
// holds the last number of each name.
std::string Numbered(const std::string& name, std::set<std::string>& given, std::map<std::string, int>& counts)
{
    const std::size_t dot = name.rfind('.');
    int& count = counts[xsflib::AsciiLower(name)];
    std::string numbered = name;
    while (!given.insert(xsflib::AsciiLower(numbered)).second)
    {
        count = std::max(count, 1) + 1;
        numbered = name.substr(0, dot) + "_" + std::to_string(count) + name.substr(dot);
    }

    return numbered;
}

} // namespace

std::vector<NamedFile> FindFiles(std::span<const NamedFile> files)
{
    std::vector<NamedFile> found;
    for (const NamedFile& file : files)
    {
        const std::span<const uint8_t> bytes = file.bytes;
        const auto starts_with = [&bytes](const Kind& kind)
        {
            return bytes.size() >= 4 && std::memcmp(bytes.data(), kind.magic, 4) == 0;
        };
        if (std::ranges::any_of(kKinds, starts_with))
        {
            const std::optional<std::size_t> size = FileSizeAt(bytes, 0);
            if (!size || NextFile(bytes, *size) == bytes.size())
            {
                found.push_back(file);
                continue;
            }
        }

        std::set<std::string> given;
        std::map<std::string, int> counts;
        for (std::size_t at = NextFile(bytes, 0); at < bytes.size(); at = NextFile(bytes, at))
        {
            const std::size_t size = *FileSizeAt(bytes, at);
            const std::string name = Numbered(NameAt(bytes, at, *KindAt(bytes, at)), given, counts);
            found.push_back({file.path + "/" + name, bytes.subspan(at, size)});
            at += size;
        }
    }

    return found;
}

std::vector<NamedFile> Decompressed(std::span<const NamedFile> files, std::vector<std::vector<uint8_t>>& storage)
{
    std::vector<NamedFile> result;
    for (const NamedFile& file : files)
    {
        // An archive's stored files would give FindFiles their DSE files twice.
        if (const std::optional<std::vector<GfsaMember>> members = GfsaMembers(file.bytes))
        {
            for (const GfsaMember& member : *members)
            {
                std::optional<std::vector<uint8_t>> bytes = DecompressLevel5(member.bytes, kMostDecompressed);
                if (bytes && NextFile(*bytes, 0) < bytes->size())
                {
                    storage.push_back(std::move(*bytes));
                    result.push_back({file.path + "/" + member.path, storage.back()});
                }
            }
            continue;
        }
        result.push_back(file);
        if (file.bytes.size() < 4 || file.bytes[0] != kLz77 || (xsflib::LoadLe32(file.bytes.data()) >> 8) < kHeaderSize)
        {
            continue;
        }

        std::vector<uint8_t> bytes;
        try
        {
            bytes = disc::DecompressCx(file.bytes, kMostDecompressed);
        }
        catch (const disc::Error&)
        {
            continue;
        }
        if (NextFile(bytes, 0) < bytes.size())
        {
            storage.push_back(std::move(bytes));
            result.back().bytes = storage.back();
        }
    }

    return result;
}

} // namespace ntdlib::dse

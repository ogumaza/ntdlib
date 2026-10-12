// SPDX-License-Identifier: MIT

// GameCube and Wii disc images in the common container formats: ISO (including NKit), GCZ, CISO, WBFS, WDF, WIA and
// RVZ, whole or split into pieces. An Image provides the disc's bytes. WIA and RVZ store a Wii partition's data
// decrypted and without its hashes. They store it separately from the rest of the disc. PartitionData provides that
// data. ntdlib::disc reads ISO, WBFS and WDF. ntdlib::disc_containers reads GCZ, CISO, WIA and RVZ, with zlib,
// Zstandard, liblzma and bzip2 (containers.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ntdlib/disc/source.h"

namespace ntdlib::disc
{

// The console whose discs a program reads. Wii discs come in more containers than GameCube discs. WBFS holds only Wii
// discs. Only a Wii disc has partitions for WIA and RVZ to store apart.
enum class Console
{
    kGameCube,
    kWii,
};

// A Wii partition's data as the game reads it: decrypted and without the hashes. That leaves 0x7C00 bytes for every
// 0x8000 bytes on the disc.
class PartitionData
{
public:
    virtual ~PartitionData() = default;

    // Reads `size` bytes at `offset` into `out`. Throws Error.
    virtual void Read(uint64_t offset, uint8_t* out, size_t size) = 0;
};

// Random access to a disc image's bytes.
class Image
{
public:
    virtual ~Image() = default;

    // The container's name for messages: "ISO", "GCZ", "CISO", "WBFS", "WDF", "WIA" or "RVZ".
    virtual std::string Format() const = 0;

    // Reads `size` bytes of the disc at `offset` into `out`. Bytes that the image doesn't store read as zeros. These
    // include scrubbed areas and anything past the end of the image. Throws Error.
    virtual void Read(uint64_t offset, uint8_t* out, size_t size) = 0;

    // Returns the data of the Wii partition whose data starts at disc offset `data_offset` if the container stores that
    // data apart from the rest of the disc. WIA and RVZ do this. For other containers it returns null. The partition's
    // data is then on the disc.
    virtual std::unique_ptr<PartitionData> OpenPartitionData(uint64_t data_offset);

    // Returns the number of the disc's bytes that a plain image stores. Reads past them return zeros. The other formats
    // return nothing.
    virtual std::optional<uint64_t> StoredSize() const;
};

// Returns the container format of an image whose first bytes are `header`: "GCZ", "CISO", "WIA" or "RVZ", the formats
// that disc_containers reads, or an empty string for the others.
std::string ContainerFormat(std::span<const uint8_t> header);

// How OpenImage treats an image in one of disc_containers' formats (ContainerFormat).
struct ImageOptions
{
    // Opens the image: disc_containers' OpenContainer. Without it, or if it returns null, the image reads as a plain
    // image. With `refuse_container`, OpenImage throws Error with that function's message instead.
    std::function<std::unique_ptr<Image>(const std::filesystem::path& path, Console console)> open_container{};

    // Returns the message for an image in container `format` that `open_container` doesn't open. A program that reads
    // only plain images rejects the other formats this way.
    std::function<std::string(const std::string& format)> refuse_container{};
};

// Opens an image file in any of the formats above, with `options` for disc_containers' formats. The format is detected
// from the file's contents. The name doesn't matter. A split image is opened by its first piece. WBFS continues in
// .wbf1 to .wbf9. The other formats append .1, .01 or .001 and onwards to the name. Wiimm's tools split images this
// way. For `Console::kGameCube`, a WBFS file opens as a plain image, and a WIA or RVZ file's Wii partitions are
// ignored. Throws Error.
std::unique_ptr<Image> OpenImage(const std::filesystem::path& path, Console console, const ImageOptions& options = {});

} // namespace ntdlib::disc

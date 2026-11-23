// SPDX-License-Identifier: MIT

// Tests of the compact writer of DS cartridge images (ntdlib/ntr/compact_image.h), on images that testing/nds_files.h
// builds.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/testing/nds_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
namespace ntr = ntdlib::ntr;

using Bytes = std::vector<uint8_t>;

Bytes Of(std::span<const uint8_t> bytes)
{
    return {bytes.begin(), bytes.end()};
}

nds_files::Image Spec()
{
    nds_files::Image spec;
    spec.files = {
        {"sound/sound_data.sdat", Bytes(0x300, 1)}, {"data/a.bin", Bytes(0x10, 2)}, {"data/b.bin", Bytes(0x20, 3)}};

    return spec;
}

// An image with a footer after its ARM9 binary and a banner.
Bytes ImageWithFooter(const nds_files::Image& spec, std::vector<uint32_t>& ids)
{
    auto [image, built_ids] = nds_files::BuildImage(spec);
    ids = built_ids;
    const uint32_t end = 0x4000 + static_cast<uint32_t>(spec.arm9.size());
    xsflib::StoreLe32(&image[end], 0xDEC00621);
    xsflib::StoreLe32(&image[end + 4], 0x10);
    xsflib::StoreLe32(&image[end + 8], 0);
    xsflib::StoreLe32(&image[0x68], xsflib::LoadLe32(&image[0x40]));

    return image;
}

// A new ARM9 binary, a new file, a file and the ARM9 overlay's file kept as they are, and two files left out.
void TestLayout()
{
    const nds_files::Image spec = Spec();
    std::vector<uint32_t> ids;
    const Bytes image = ImageWithFooter(spec, ids);
    const ntr::Rom rom = ntr::Rom::Parse(image);
    ntr::CompactParts parts{.arm9 = Bytes(0x180, 0xA9)};
    parts.files[ids[0]] = Bytes(0x40, 5);
    parts.files[ids[2]] = Of(rom.FileData(ids[2]));
    parts.files[0] = Of(rom.FileData(0));

    const Bytes compact = ntr::WriteCompactImage(rom, parts);

    const ntr::Rom cut = ntr::Rom::Parse(compact);
    const ntr::RomHeader& header = cut.Header();
    XSFLIB_CHECK(header.arm9.rom_offset == 0x4000 && Of(cut.Arm9()) == parts.arm9);
    XSFLIB_CHECK(Of(std::span(compact).subspan(0x4180, 12)) == Of(std::span(image).subspan(0x4100, 12)));
    XSFLIB_CHECK(Of(cut.Arm7()) == Of(rom.Arm7()) && header.arm7.rom_offset % 0x200 == 0);
    XSFLIB_CHECK(cut.Arm9Overlays() == rom.Arm9Overlays() && cut.Arm7Overlays() == rom.Arm7Overlays());
    XSFLIB_CHECK(Of(cut.FileData(ids[0])) == Bytes(0x40, 5));
    XSFLIB_CHECK(Of(cut.FileData(ids[2])) == spec.files[2].second);
    XSFLIB_CHECK(Of(cut.FileData(0)) == spec.arm9_overlay);
    XSFLIB_CHECK(cut.FileData(ids[1]).empty() && cut.FileData(1).empty());
    XSFLIB_CHECK(cut.Find("data/b.bin") != nullptr && cut.Find("data/b.bin")->id == ids[2]);
    XSFLIB_CHECK(cut.Files()[0].start < cut.Files()[ids[0]].start);
    XSFLIB_CHECK(cut.Files()[ids[0]].start < cut.Files()[ids[2]].start);
    XSFLIB_CHECK(header.used_size == compact.size() && header.banner_offset == 0);
    XSFLIB_CHECK(compact[0x14] == 0);
    XSFLIB_CHECK(xsflib::LoadLe16(&compact[0x15E]) == ntr::Crc16(std::span(compact).first(0x15E)));
}

// A DSi-only image keeps its layout. A new ARM9 binary, shorter than the old, goes at the old one's offset. A kept file
// with new bytes of its size, and a kept file as it is. The other files contain zeros, and every other byte stays.
void TestDsiImage()
{
    nds_files::Image spec = Spec();
    spec.unit_code = 3;
    const auto [image, ids] = nds_files::BuildImage(spec);
    const ntr::Rom rom = ntr::Rom::Parse(image);
    const uint32_t sdat = ids[0];
    const uint32_t a = ids[1];
    const uint32_t b = ids[2];

    ntr::CompactParts parts;
    parts.arm9 = Bytes(0x80, 0xA9);
    parts.files[sdat] = Of(rom.FileData(sdat));
    parts.files[a] = Bytes(0x10, 0xAA);
    const Bytes out = ntr::WriteCompactImage(rom, parts);

    Bytes expected = image;
    const ntr::RomHeader& header = rom.Header();
    std::ranges::copy(parts.arm9, expected.begin() + header.arm9.rom_offset);
    std::ranges::copy(parts.files[a], expected.begin() + rom.Files()[a].start);
    for (const uint32_t id : {b, 0u, 1u})
    {
        std::fill(expected.begin() + rom.Files()[id].start, expected.begin() + rom.Files()[id].end, uint8_t{0});
    }
    XSFLIB_CHECK(out == expected);
    XSFLIB_CHECK(out[header.arm9.rom_offset + 0x80] == 0x99);
    XSFLIB_CHECK(Of(ntr::Rom::Parse(out).FileData(sdat)) == Bytes(0x300, 1));
    XSFLIB_CHECK(Of(ntr::Rom::Parse(out).FileData(b)) == Bytes(0x20, 0));

    // An ARM9 binary larger than the image's, and a kept file of another size.
    ntr::CompactParts larger;
    larger.arm9 = Bytes(0x101, 0);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { ntr::WriteCompactImage(rom, larger); }) ==
                 "the new ARM9 binary is larger than the image's");
    ntr::CompactParts resized;
    resized.files[a] = Bytes(0x11, 0);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { ntr::WriteCompactImage(rom, resized); }) ==
                 "file " + std::to_string(a) + " changes its size in a DSi image");
}

// In a DSi-only image with a digest, a file that isn't kept keeps its bytes in a sector that a kept file reaches. The
// image's files: the overlay's two files at 0x4C00 and 0x4E00, the sound archive at 0x5000 to 0x5300, a at 0x5400 and
// b at 0x5600. With sectors of 0x400 bytes from 0x4000, a shares its sector with b. From 0x4200, the archive's first
// sector starts at 0x4E00 with the overlay's second file, and b has a sector of its own.
void TestDsiSectors()
{
    nds_files::Image spec = Spec();
    spec.unit_code = 3;
    const auto [image, ids] = nds_files::BuildImage(spec);
    const uint32_t sdat = ids[0];
    const uint32_t a = ids[1];
    const uint32_t b = ids[2];
    const ntr::Rom plain = ntr::Rom::Parse(image);

    struct Case
    {
        uint32_t start;
        Bytes overlay_first;
        Bytes overlay_second;
        Bytes b_bytes;
    };
    const Case kCases[] = {
        {0x4000, Bytes(0x40, 0), Bytes(0x20, 0), Bytes(0x20, 3)},
        {0x4200, Bytes(0x40, 0), Of(plain.FileData(1)), Bytes(0x20, 0)},
    };
    for (const Case& c : kCases)
    {
        Bytes with_digest = image;
        xsflib::StoreLe32(&with_digest[0x1E0], c.start);
        xsflib::StoreLe32(&with_digest[0x200], 0x400);
        const ntr::Rom rom = ntr::Rom::Parse(with_digest);
        XSFLIB_CHECK(ntr::DigestSectorsOf(rom).start == c.start && ntr::DigestSectorsOf(rom).size == 0x400);

        ntr::CompactParts parts;
        parts.arm9 = Of(rom.Arm9());
        parts.files[sdat] = Of(rom.FileData(sdat));
        parts.files[a] = Of(rom.FileData(a));
        const Bytes written = ntr::WriteCompactImage(rom, parts);
        const ntr::Rom out = ntr::Rom::Parse(written);
        XSFLIB_CHECK(Of(out.FileData(0)) == c.overlay_first);
        XSFLIB_CHECK(Of(out.FileData(1)) == c.overlay_second);
        XSFLIB_CHECK(Of(out.FileData(sdat)) == Bytes(0x300, 1));
        XSFLIB_CHECK(Of(out.FileData(a)) == Bytes(0x10, 2));
        XSFLIB_CHECK(Of(out.FileData(b)) == c.b_bytes);
    }

    // A range widens to whole sectors from the start. It stays as it is before the start and without a digest.
    const ntr::DigestSectors sectors{.start = 0x4200, .size = 0x400};
    XSFLIB_CHECK((sectors.Around(0x4F00, 0x5201) == std::pair<uint32_t, uint32_t>{0x4E00, 0x5600}));
    XSFLIB_CHECK((sectors.Around(0x4000, 0x4100) == std::pair<uint32_t, uint32_t>{0x4000, 0x4100}));
    XSFLIB_CHECK((ntr::DigestSectors{}.Around(0x5001, 0x5002) == std::pair<uint32_t, uint32_t>{0x5001, 0x5002}));
    // A DS image has no digest, whatever its header's bytes at 0x200.
    Bytes ds_image = nds_files::BuildImage(Spec()).first;
    xsflib::StoreLe32(&ds_image[0x200], 0x400);
    XSFLIB_CHECK(ntr::DigestSectorsOf(ntr::Rom::Parse(ds_image)).size == 0);
}

// A partial image, a DSi-enhanced image, and a file past the FAT.
void TestRefusals()
{
    nds_files::Image dsi = Spec();
    dsi.unit_code = 2;
    const auto [dsi_image, dsi_ids] = nds_files::BuildImage(dsi);
    const auto [image, ids] = nds_files::BuildImage(Spec());
    const ntr::Rom rom = ntr::Rom::Parse(image);
    ntr::CompactParts past;
    past.files[99] = {};

    const Bytes cut(image.begin(), image.end() - 0x10);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { ntr::WriteCompactImage(ntr::Rom::ParsePartial(cut), {}); }) ==
                 "the image is partial");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { ntr::WriteCompactImage(ntr::Rom::Parse(dsi_image), {}); }) ==
                 "the image is for the DSi");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { ntr::WriteCompactImage(rom, past); }) ==
                 "file 99 lies past the FAT");
}

} // namespace

int main()
{
    TestLayout();
    TestDsiImage();
    TestDsiSectors();
    TestRefusals();

    return xsflib::testing::Summary("DS compact image");
}

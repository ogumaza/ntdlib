// SPDX-License-Identifier: MIT

// Tests of the compact writer of DS cartridge images (ntdlib/ntr/compact_image.h), on images that testing/nds_files.h
// builds.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
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

// A partial image, an image for the DSi, and a file past the FAT.
void TestRefusals()
{
    nds_files::Image dsi = Spec();
    dsi.unit_code = 3;
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
    TestRefusals();

    return xsflib::testing::Summary("DS compact image");
}

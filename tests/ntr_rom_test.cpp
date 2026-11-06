// SPDX-License-Identifier: MIT

// Tests of the reader of the DS's cartridge images (ntdlib/ntr/rom.h), on images that testing/nds_files.h builds.

#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
using ntdlib::ntr::Rom;

nds_files::Image Spec()
{
    nds_files::Image spec;
    spec.files = {{"sound/sound_data.sdat", {1, 2, 3}},
                  {"readme.txt", {4, 5}},
                  {"sound/bgm/song.bin", {6}},
                  {"data/a/b/c.bin", std::vector<uint8_t>(700, 7)}};

    return spec;
}

void TestHeader()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());

    const Rom rom = Rom::Parse(image);

    XSFLIB_CHECK(rom.Header().title == "TEST");
    XSFLIB_CHECK(rom.Header().game_code == "ATST");
    XSFLIB_CHECK(rom.Header().maker_code == "01");
    XSFLIB_CHECK(rom.Header().arm9.ram_address == 0x02000000);
    XSFLIB_CHECK(rom.Arm9().size() == 0x100 && rom.Arm9()[0] == 0x99);
    XSFLIB_CHECK(rom.Arm7().size() == 0x80 && rom.Arm7()[0] == 0x77);
    XSFLIB_CHECK(rom.Arm9i().empty() && rom.Arm7i().empty());
    XSFLIB_CHECK(!rom.Header().arm9i);
}

void TestOverlays()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());

    const Rom rom = Rom::Parse(image);

    XSFLIB_CHECK(rom.Arm9Overlays().size() == 1 && rom.Arm7Overlays().size() == 1);
    XSFLIB_CHECK(rom.Arm9Overlays()[0].file_id == 0 && rom.Arm9Overlays()[0].ram_address == 0x02100000);
    XSFLIB_CHECK(rom.Arm7Overlays()[0].file_id == 1 && rom.Arm7Overlays()[0].ram_size == 0x20);
    XSFLIB_CHECK(rom.Files()[0].path.empty() && rom.FileData(0).size() == 0x40);
}

void TestFiles()
{
    const nds_files::Image spec = Spec();
    const auto [image, ids] = nds_files::BuildImage(spec);

    const Rom rom = Rom::Parse(image);

    XSFLIB_CHECK(rom.Files().size() == 6);
    for (std::size_t i = 0; i < spec.files.size(); i++)
    {
        const ntdlib::ntr::RomFile* file = rom.Find(spec.files[i].first);
        XSFLIB_CHECK(file != nullptr && file->id == ids[i]);
        if (file)
        {
            const auto data = rom.FileData(file->id);
            XSFLIB_CHECK(std::vector<uint8_t>(data.begin(), data.end()) == spec.files[i].second);
        }
    }
    XSFLIB_CHECK(rom.Find("sound") == nullptr);
    XSFLIB_CHECK(rom.Find("Sound/sound_data.sdat") == nullptr);
}

void TestDsiBinaries()
{
    nds_files::Image spec = Spec();
    spec.unit_code = 2;
    auto [image, ids] = nds_files::BuildImage(spec);
    xsflib::StoreLe32(&image[0x1C0], 0x200);
    xsflib::StoreLe32(&image[0x1C8], 0x02400000);
    xsflib::StoreLe32(&image[0x1CC], 0x10);
    xsflib::StoreLe32(&image[0x1D0], 0x300);
    xsflib::StoreLe32(&image[0x1D8], 0x02E80000);
    xsflib::StoreLe32(&image[0x1DC], 0x20);

    const Rom rom = Rom::Parse(image);

    XSFLIB_CHECK(rom.Header().arm9i && rom.Header().arm9i->ram_address == 0x02400000);
    XSFLIB_CHECK(rom.Arm9i().size() == 0x10 && rom.Arm9i().data() == image.data() + 0x200);
    XSFLIB_CHECK(rom.Arm7i().size() == 0x20 && rom.Arm7i().data() == image.data() + 0x300);
}

void TestRefusals()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());
    std::vector<uint8_t> short_fat = image;
    xsflib::StoreLe32(&short_fat[xsflib::LoadLe32(&image[0x48]) + 2 * 8 + 4], 0xFFFFFF);
    std::vector<uint8_t> looped = image;
    const uint32_t fnt = xsflib::LoadLe32(&image[0x40]);
    // The root's first entry is a file, then the subdirectories. Point the first subdirectory back at the root.
    const uint32_t root = xsflib::LoadLe32(&image[fnt]);
    const uint32_t name = image[fnt + root];
    looped[fnt + root + 1 + name + 1 + (image[fnt + root + 1 + name] & 0x7F)] = 0;

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(std::vector<uint8_t>(0x100)); }) ==
                 "the image is shorter than a cartridge header");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(short_fat); }) ==
                 "file 2 lies past the end of the image");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(looped); }) ==
                 "the file name table names a directory twice or out of range");
}

} // namespace

int main()
{
    TestHeader();
    TestOverlays();
    TestFiles();
    TestDsiBinaries();
    TestRefusals();

    return xsflib::testing::Summary("DS ROM");
}

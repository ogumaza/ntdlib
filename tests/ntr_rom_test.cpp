// SPDX-License-Identifier: MIT

// Tests of the reader of the DS's cartridge images (ntdlib/ntr/rom.h), on images that testing/nds_files.h builds.

#include <algorithm>
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

// The files that a range of the image overlaps: one file, two files across the gap between them, the overlay's file,
// and none.
void TestFilesIn()
{
    nds_files::Image spec = Spec();
    spec.files.push_back({"empty.bin", {}});
    const auto [image, ids] = nds_files::BuildImage(spec);
    const Rom rom = Rom::Parse(image);
    const ntdlib::ntr::RomFile& sdat = rom.Files()[ids[0]];
    const ntdlib::ntr::RomFile& readme = rom.Files()[ids[1]];
    const ntdlib::ntr::RomFile& empty = rom.Files()[ids[4]];
    const ntdlib::ntr::RomFile& overlay = rom.Files()[0];

    const std::vector<uint32_t> one = rom.FilesIn(sdat.start + 1, sdat.start + 2);
    const std::vector<uint32_t> two = rom.FilesIn(std::min(sdat.start, readme.start), std::max(sdat.end, readme.end));
    const std::vector<uint32_t> first = rom.FilesIn(overlay.start, overlay.start + 0x200);
    const std::vector<uint32_t> after = rom.FilesIn(sdat.end, sdat.end + 1);
    const std::vector<uint32_t> at_empty = rom.FilesIn(empty.start, empty.start + 1);

    XSFLIB_CHECK(one == std::vector<uint32_t>{ids[0]});
    XSFLIB_CHECK(two.size() == 2 && two[0] == std::min(ids[0], ids[1]) && two[1] == std::max(ids[0], ids[1]));
    XSFLIB_CHECK(first == std::vector<uint32_t>{0});
    XSFLIB_CHECK(after.empty());
    XSFLIB_CHECK(at_empty.empty());
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

// A trimmed image may end in or before a DSi binary. The image parses without that binary.
void TestTrimmedDsiImage()
{
    nds_files::Image spec = Spec();
    spec.unit_code = 2;
    auto [image, ids] = nds_files::BuildImage(spec);
    const auto size = static_cast<uint32_t>(image.size());
    xsflib::StoreLe32(&image[0x1C0], size - 0x10);
    xsflib::StoreLe32(&image[0x1CC], 0x10);
    xsflib::StoreLe32(&image[0x1D0], size - 0x10);
    xsflib::StoreLe32(&image[0x1DC], 0x20);

    const Rom rom = Rom::Parse(image);

    XSFLIB_CHECK(rom.Arm9i().size() == 0x10 && rom.Arm9i().data() == image.data() + size - 0x10);
    XSFLIB_CHECK(!rom.Header().arm7i && rom.Arm7i().empty());
    XSFLIB_CHECK(rom.Find("sound/sound_data.sdat") != nullptr);
}

// A partial image, such as the image of a 2SF set, may end before the files, the tables or the binaries. Parse refuses
// it. ParsePartial keeps what the image contains, and the sizes as the header states them.
void TestPartialImage()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());
    const Rom whole = Rom::Parse(image);
    XSFLIB_CHECK(!whole.Partial());
    const ntdlib::ntr::RomHeader& h = whole.Header();
    const ntdlib::ntr::RomFile* last = &whole.Files()[0];
    for (const ntdlib::ntr::RomFile& file : whole.Files())
    {
        last = file.end > last->end ? &file : last;
    }
    XSFLIB_CHECK(last->path == "data/a/b/c.bin" && last->end == image.size());

    // The image ends 0x10 bytes into its last file.
    const std::vector<uint8_t> cut(image.begin(), image.begin() + last->start + 0x10);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(cut); }) ==
                 "file " + std::to_string(last->id) + " lies past the end of the image");
    const Rom rom = Rom::ParsePartial(cut);
    XSFLIB_CHECK(rom.Partial());
    XSFLIB_CHECK(rom.Header().used_size == h.used_size && rom.Header().arm9 == h.arm9);
    XSFLIB_CHECK(rom.Arm9().size() == 0x100 && rom.Arm7().size() == 0x80);
    XSFLIB_CHECK(rom.Arm9Overlays() == whole.Arm9Overlays() && rom.Arm7Overlays() == whole.Arm7Overlays());
    XSFLIB_CHECK(rom.Files().size() == whole.Files().size());
    const ntdlib::ntr::RomFile* cut_file = rom.Find("data/a/b/c.bin");
    XSFLIB_CHECK(cut_file != nullptr && cut_file->start == last->start && cut_file->end == last->end);
    if (cut_file != nullptr)
    {
        const auto data = rom.FileData(cut_file->id);
        XSFLIB_CHECK(std::vector<uint8_t>(data.begin(), data.end()) == std::vector<uint8_t>(0x10, 7));
    }
    const ntdlib::ntr::RomFile* readme = rom.Find("readme.txt");
    XSFLIB_CHECK(readme != nullptr && rom.FileData(readme->id).size() == 2);

    // The image ends inside the FAT: the files stop there and stay unnamed.
    const std::vector<uint8_t> in_fat(image.begin(), image.begin() + h.fat_offset + 2 * 8 + 4);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(in_fat); }) ==
                 "the file allocation table lies past the end of the image");
    const Rom fat_cut = Rom::ParsePartial(in_fat);
    XSFLIB_CHECK(fat_cut.Files().size() == 2);
    XSFLIB_CHECK(fat_cut.Files()[0].path.empty() && fat_cut.Files()[1].path.empty());
    XSFLIB_CHECK(fat_cut.FileData(0).empty() && fat_cut.Find("readme.txt") == nullptr);

    // The image ends inside the ARM9 binary. The rest of that binary, the ARM7 binary and every table are past the end.
    const std::vector<uint8_t> in_arm9(image.begin(), image.begin() + h.arm9.rom_offset + 0x80);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(in_arm9); }) ==
                 "the ARM9 binary lies past the end of the image");
    const Rom start = Rom::ParsePartial(in_arm9);
    XSFLIB_CHECK(start.Header().arm9.size == 0x100 && start.Header().arm7 == h.arm7);
    XSFLIB_CHECK(start.Arm9().size() == 0x80 && start.Arm9().data() == in_arm9.data() + h.arm9.rom_offset);
    XSFLIB_CHECK(start.Arm7().empty());
    XSFLIB_CHECK(start.Arm9Overlays().empty() && start.Arm7Overlays().empty() && start.Files().empty());

    // A file that ends before it starts is an error in a partial image too, and so is an image without a whole header.
    std::vector<uint8_t> backwards = cut;
    xsflib::StoreLe32(&backwards[h.fat_offset + 4], 0);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::ParsePartial(backwards); }) ==
                 "file 0 lies past the end of the image");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::ParsePartial(std::vector<uint8_t>(0x100)); }) ==
                 "the image is shorter than a cartridge header");
}

// A partial image whose FNT doesn't parse leaves the files unnamed. Parse refuses the same FNT in a whole image.
void TestPartialImageWithBadNames()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());
    const uint32_t fnt = xsflib::LoadLe32(&image[0x40]);

    // A zeroed root: the directory count is 0.
    std::vector<uint8_t> zero_root = image;
    std::fill_n(zero_root.begin() + fnt, 8, 0);
    // A zeroed entry of a subdirectory: its list starts at the table's start, in the directory entries.
    std::vector<uint8_t> zero_directory = image;
    std::fill_n(zero_directory.begin() + fnt + 8, 8, 0);

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { Rom::Parse(zero_root); }) ==
                 "the file name table's directory count is out of range");
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { Rom::Parse(zero_directory); }).empty());
    for (const std::vector<uint8_t>* bad : {&zero_root, &zero_directory})
    {
        const Rom rom = Rom::ParsePartial(*bad);
        XSFLIB_CHECK(rom.Files().size() == 6);
        bool unnamed = true;
        for (const ntdlib::ntr::RomFile& file : rom.Files())
        {
            unnamed = unnamed && file.path.empty();
        }
        XSFLIB_CHECK(unnamed);
        XSFLIB_CHECK(rom.FileData(ids[1]).size() == 2);
    }
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
    TestFilesIn();
    TestDsiBinaries();
    TestTrimmedDsiImage();
    TestRefusals();
    TestPartialImage();
    TestPartialImageWithBadNames();

    return xsflib::testing::Summary("DS ROM");
}

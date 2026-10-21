// SPDX-License-Identifier: MIT

// Tests of ntdlib::disc without disc_containers: the messages and checks that GameOptions sets for a program that reads
// only plain images, the file system table's order, the files of an extracted disc, a disc whose header leaves out the
// magic word, a file read in parts, reads split at blocks' bounds, and ContainerFormat, DiscConsole and ImageFormat.
// The images and folders are made up here, in a temporary folder.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/testing/check.h"

#include "disc_files.h"
#include "ntdlib/disc/error.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/disc/source.h"
#include "temp_folder.h"

namespace fs = std::filesystem;
namespace disc = ntdlib::disc;
using disc_files::AppleDouble;
using disc_files::StoreBe32;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

namespace
{

// A file system table's entry: a folder's flag and name offset, then its two words.
void Entry(std::vector<uint8_t>& fst, size_t index, bool folder, uint32_t name, uint32_t a, uint32_t b)
{
    StoreBe32(fst, index * 12, (folder ? 1u << 24 : 0u) | name);
    StoreBe32(fst, index * 12 + 4, a);
    StoreBe32(fst, index * 12 + 8, b);
}

// A GameCube image: the disc header, a file system table at 0x2800 that lists "z.aw" and "a.baa" in that order, and the
// files' data from 0x3000. main.dol is an empty header at 0x2500. `entries` overrides the table's count, and `baa_size`
// the size of "a.baa".
std::vector<uint8_t> GameCubeImage(uint32_t entries = 3, uint32_t baa_size = 16)
{
    std::vector<uint8_t> fst(3 * 12);
    const std::string names = std::string("z.aw") + '\0' + "a.baa" + '\0';
    Entry(fst, 0, true, 0, 0, entries);
    Entry(fst, 1, false, 0, 0x3000, 16);
    Entry(fst, 2, false, 5, 0x3010, baa_size);
    fst.insert(fst.end(), names.begin(), names.end());

    std::vector<uint8_t> image(0x3020, 0);
    image[0] = 'G';
    StoreBe32(image, 0x1C, 0xC2339F3D);
    StoreBe32(image, 0x420, 0x2500); // main.dol with a header of zeros
    StoreBe32(image, 0x424, 0x2800);
    StoreBe32(image, 0x428, static_cast<uint32_t>(fst.size()));
    std::copy(fst.begin(), fst.end(), image.begin() + 0x2800);
    for (size_t i = 0x3000; i < 0x3020; i++)
    {
        image[i] = static_cast<uint8_t>(i);
    }

    return image;
}

// A small main.dol: a header, then one text section of 0x20 bytes, loaded at 0x80003100. `entry` is the entry point.
std::vector<uint8_t> SmallDol(uint32_t entry = 0x80003100)
{
    std::vector<uint8_t> dol(0x120, 0);
    StoreBe32(dol, 0x00, 0x100);      // the section's offset in the file
    StoreBe32(dol, 0x48, 0x80003100); // its load address
    StoreBe32(dol, 0x90, 0x20);       // its size
    StoreBe32(dol, 0xE0, entry);

    return dol;
}

// GameCubeImage without the magic word in the disc header, and with SmallDol at 0x2500. Some discs that never went on
// sale leave the word out.
std::vector<uint8_t> UnmarkedImage(uint32_t entries = 3)
{
    std::vector<uint8_t> image = GameCubeImage(entries);
    StoreBe32(image, 0x1C, 0);
    const std::vector<uint8_t> dol = SmallDol();
    std::copy(dol.begin(), dol.end(), image.begin() + 0x2500);

    return image;
}

// The settings of a program that reads plain images only: its messages, the checks of a plain image's size, and no
// system files.
disc::GameOptions PlainOnly()
{
    disc::GameOptions options{disc::Console::kGameCube, "xyz2midi"};
    options.image.refuse_container = [](const std::string& format)
    {
        return "it's " + std::string(format == "RVZ" ? "an " : "a ") + format + " disc image";
    };
    options.not_this_console = "not a start-up archive or a GameCube disc image";
    options.other_console = "it's a Wii disc";
    options.damaged_file_system = "the disc image's file table is damaged";
    options.short_header = "not a GameCube disc image";
    options.file_past_end = [](const std::string& path)
    {
        return path + " runs past the end of the disc image";
    };
    options.system_files = false;

    return options;
}

// LoadGame's message for `path` with `options`, and every file's read, or "read" if every read succeeds.
std::string Outcome(const fs::path& path, const disc::GameOptions& options)
{
    disc::GameFiles game;
    if (const std::optional<std::string> error = disc::LoadGame(path, game, options))
    {
        return *error;
    }

    const auto read_every_file = [&]
    {
        for (const disc::GameFile& file : game.files)
        {
            std::vector<uint8_t> data;
            game.read_file(file.path, data);
        }
    };
    const std::string error = xsflib::testing::ErrorOf<disc::Error>(read_every_file);
    return error.empty() ? "read" : error;
}

void TestPlainImage()
{
    const TempFolder folder("disc_options_test");
    const fs::path path = folder.Write("good.iso", GameCubeImage());

    disc::GameFiles game;
    const std::optional<std::string> error = disc::LoadGame(path, game, PlainOnly());

    Check(!error, "a plain image loads: " + error.value_or(""));
    Check(game.files.size() == 2 && game.files[0].path == "a.baa" && game.files[1].path == "z.aw",
          "the files are sorted by path");
    Check(game.files.size() == 2 && game.files[0].index == 2 && game.files[1].index == 1,
          "each file keeps its entry's index in the table");
    Check(game.dol.empty() && game.bi2.empty() && game.apploader.empty() && game.boot.size() == 0x440,
          "without system files, only the header and the file system are read");

    std::vector<uint8_t> baa;
    Check(game.read_file && game.read_file("a.baa", baa) && baa.size() == 16 && baa[0] == 0x10, "a file reads");
}

void TestMessages()
{
    const TempFolder folder("disc_options_test");
    std::vector<uint8_t> short_image = GameCubeImage();
    short_image.resize(0x100);
    std::vector<uint8_t> wii(0x440, 0);
    StoreBe32(wii, 0x18, 0x5D1C9EA3);
    std::vector<uint8_t> fst_past_end = GameCubeImage();
    StoreBe32(fst_past_end, 0x424, 0x100000);
    std::vector<uint8_t> noise(0x2000);
    for (size_t i = 0; i < noise.size(); i++)
    {
        noise[i] = static_cast<uint8_t>(i * 7);
    }

    const disc::GameOptions options = PlainOnly();

    Check(Outcome(folder.Write("past_end.iso", GameCubeImage(3, 0x10000)), options) ==
              "a.baa runs past the end of the disc image",
          "a file past the image's end");
    Check(Outcome(folder.Write("count.iso", GameCubeImage(1000)), options) == "the disc image's file table is damaged",
          "a damaged table's count");
    Check(Outcome(folder.Write("fst_past_end.iso", fst_past_end), options) == "the disc image's file table is damaged",
          "a table past the image's end");
    Check(Outcome(folder.Write("short.iso", short_image), options) == "not a GameCube disc image",
          "an image shorter than the disc header");
    Check(Outcome(folder.Write("wii.iso", wii), options) == "it's a Wii disc", "a Wii disc");
    Check(Outcome(folder.Write("noise.bin", noise), options) == "not a start-up archive or a GameCube disc image",
          "data of neither console");

    for (const char* magic : {"RVZ\x01", "WIA\x01", "CISO"})
    {
        std::vector<uint8_t> container(0x100, 0);
        std::copy(magic, magic + 4, container.begin());
        const std::string format = std::string(magic, magic[3] == 1 ? 3 : 4);
        Check(Outcome(folder.Write(format + ".bin", container), options) ==
                  "it's " + std::string(format == "RVZ" ? "an " : "a ") + format + " disc image",
              "a rejected " + format + " image");
    }

    std::vector<uint8_t> gcz(0x100, 0);
    StoreBe32(gcz, 0, 0x01C00BB1);
    Check(Outcome(folder.Write("gcz.bin", gcz), options) == "it's a GCZ disc image", "a rejected GCZ image");
}

void TestDefaults()
{
    const TempFolder folder("disc_options_test");
    std::vector<uint8_t> rvz(0x100, 0);
    std::copy_n("RVZ\x01", 4, rvz.begin());
    const disc::GameOptions kOptions{disc::Console::kGameCube, "xyz"};

    disc::GameFiles game;
    const std::optional<std::string> error =
        disc::LoadGame(folder.Write("past_end.iso", GameCubeImage(3, 0x10000)), game, kOptions);
    std::vector<uint8_t> baa;
    const bool read = !error && game.read_file("a.baa", baa);

    Check(read && baa.size() == 0x10000 && baa[0x20] == 0, "by default, a read past the image's end gives zeros");
    Check(Outcome(folder.Write("rvz.bin", rvz), kOptions) == "it isn't a GameCube disc image",
          "without an opener, a container reads as a plain image");
    Check(Outcome(folder.Write("count.iso", GameCubeImage(1000)), kOptions) == "the disc's file system is damaged",
          "the default message for a damaged table");

    disc::GameOptions null_opener{disc::Console::kGameCube, "xyz"};
    null_opener.image.open_container = [](const fs::path&, disc::Console) -> std::unique_ptr<disc::Image>
    {
        return nullptr;
    };

    Check(Outcome(folder.Write("rvz_again.bin", rvz), null_opener) == "it isn't a GameCube disc image",
          "an opener that returns null leaves the image to read as a plain image");
}

// An extracted disc's files are those in its files/ folder, sorted by path. The walk passes over an entry whose type
// can't be read: broken symbolic links here. The list leaves out AppleDouble files. macOS writes one, named "._" and
// the file's name, beside each file that it copies to a FAT or exFAT drive. On a system that doesn't let the test
// make the links, such as Windows without developer mode, the test checks the rest.
void TestExtractedDisc()
{
    const TempFolder folder("disc_options_test");
    const fs::path root = folder.Path() / "disc";
    fs::create_directories(root / "sys");
    fs::create_directories(root / "files" / "Audio");
    std::vector<uint8_t> boot(0x440, 0);
    StoreBe32(boot, 0x1C, 0xC2339F3D);
    folder.Write("disc/sys/boot.bin", boot);
    folder.Write("disc/sys/main.dol", {0});
    folder.Write("disc/sys/fst.bin", {0});

    for (const char* name : {"a.bin", "m.bin", "z.bin", "Audio/b.bin"})
    {
        folder.Write(std::string("disc/files/") + name, {1, 2, 3});
    }

    for (const char* name : {"._a.bin", "._m.bin", "Audio/._b.bin"})
    {
        folder.Write(std::string("disc/files/") + name, AppleDouble());
    }

    for (const char* name : {"b_link.bin", "y_link.bin", "Audio/a_link.bin"})
    {
        std::error_code ignored;
        fs::create_symlink(root / "missing.bin", root / "files" / name, ignored);
    }

    disc::GameFiles game;
    const std::optional<std::string> error = disc::LoadGame(root, game, {disc::Console::kGameCube, "xyz"});

    std::vector<std::string> paths;
    for (const disc::GameFile& file : game.files)
    {
        paths.push_back(file.path);
    }

    Check(!error, "an extracted disc loads: " + error.value_or(""));
    Check(paths == std::vector<std::string>{"Audio/b.bin", "a.bin", "m.bin", "z.bin"},
          "an extracted disc's files, past the broken links and without the AppleDouble files");
}

// A GameCube disc whose header leaves out the magic word loads if the rest of it is intact, from an image or from a
// folder. Data without the magic word is otherwise refused as before, with the message for data of neither console.
// The other console's options refuse even an intact disc.
void TestUnmarkedDisc()
{
    const TempFolder folder("disc_options_test");
    const disc::GameOptions options = PlainOnly();
    const disc::GameOptions kDefaults{disc::Console::kGameCube, "xyz"};
    const fs::path unmarked = folder.Write("unmarked.iso", UnmarkedImage());

    disc::GameFiles game;
    std::optional<std::string> error = disc::LoadGame(unmarked, game, options);
    Check(!error && game.files.size() == 2 && game.GameId() == std::string("G") + std::string(5, '\0'),
          "an unmarked image loads: " + error.value_or(""));
    Check(Outcome(unmarked, options) == "read", "an unmarked image's files read");
    error = disc::LoadGame(unmarked, game, kDefaults);
    Check(!error && game.dol == SmallDol() && game.region == 0u,
          "an unmarked image's system files read: " + error.value_or(""));

    // Each image below breaks one check. Otherwise it is UnmarkedImage.
    std::vector<uint8_t> empty_dol = GameCubeImage();
    StoreBe32(empty_dol, 0x1C, 0);
    std::vector<uint8_t> entry_outside = UnmarkedImage();
    StoreBe32(entry_outside, 0x2500 + 0xE0, 0x80003120);
    std::vector<uint8_t> dol_past_end = UnmarkedImage();
    StoreBe32(dol_past_end, 0x2500 + 0x90, 0x10000);
    std::vector<uint8_t> dol_in_system_area = UnmarkedImage();
    std::copy_n(dol_in_system_area.begin() + 0x2500, 0x120, dol_in_system_area.begin() + 0x1000);
    StoreBe32(dol_in_system_area, 0x420, 0x1000);
    std::vector<uint8_t> fst_past_end = UnmarkedImage();
    StoreBe32(fst_past_end, 0x424, 0x100000);
    std::vector<uint8_t> fst_in_system_area = UnmarkedImage();
    std::copy_n(fst_in_system_area.begin() + 0x2800, 0x2F, fst_in_system_area.begin() + 0x1000);
    StoreBe32(fst_in_system_area, 0x424, 0x1000);
    std::vector<uint8_t> cut = UnmarkedImage();
    cut.resize(0x2810);
    std::vector<uint8_t> short_image = UnmarkedImage();
    short_image.resize(0x100);

    const std::string kNeither = "not a start-up archive or a GameCube disc image";
    Check(Outcome(folder.Write("count.iso", UnmarkedImage(1000)), options) == kNeither, "an unmarked damaged table");
    Check(Outcome(folder.Write("empty_dol.iso", empty_dol), options) == kNeither, "an unmarked image without a DOL");
    Check(Outcome(folder.Write("entry.iso", entry_outside), options) == kNeither,
          "an unmarked image whose entry point lies outside main.dol");
    Check(Outcome(folder.Write("dol_past_end.iso", dol_past_end), options) == kNeither,
          "an unmarked image whose main.dol runs past its end");
    Check(Outcome(folder.Write("dol_in_system_area.iso", dol_in_system_area), options) == kNeither,
          "an unmarked image whose main.dol lies in the system area");
    Check(Outcome(folder.Write("fst_past_end.iso", fst_past_end), options) == kNeither,
          "an unmarked image whose table lies past its end");
    Check(Outcome(folder.Write("fst_in_system_area.iso", fst_in_system_area), options) == kNeither,
          "an unmarked image whose table lies in the system area");
    Check(Outcome(folder.Write("cut.iso", cut), options) == kNeither, "an unmarked image cut short in its table");
    Check(Outcome(folder.Write("short.iso", short_image), options) == kNeither,
          "an unmarked image shorter than the disc header");
    Check(Outcome(unmarked, {disc::Console::kWii, "xyz"}) == "it isn't a Wii disc image",
          "the Wii's options refuse an unmarked image");

    // An extracted disc gives its region in bi2.bin, with the magic word or without it.
    const fs::path root = folder.Path() / "disc";
    fs::create_directories(root / "sys");
    fs::create_directories(root / "files");
    std::vector<uint8_t> boot(0x440, 0);
    std::vector<uint8_t> bi2(0x2000, 0);
    StoreBe32(bi2, 0x18, 1);
    const std::vector<uint8_t> image = UnmarkedImage();
    folder.Write("disc/sys/boot.bin", boot);
    folder.Write("disc/sys/bi2.bin", bi2);
    folder.Write("disc/sys/main.dol", SmallDol());
    folder.Write("disc/sys/fst.bin", std::vector<uint8_t>(image.begin() + 0x2800, image.begin() + 0x282F));
    folder.Write("disc/files/a.bin", {1, 2, 3});

    error = disc::LoadGame(root, game, kDefaults);
    Check(!error && game.files.size() == 1 && game.region == 1u,
          "an unmarked extracted disc loads: " + error.value_or(""));
    Check(Outcome(root, {disc::Console::kWii, "xyz"}) == "sys/boot.bin isn't a Wii disc's header",
          "the Wii's options refuse an unmarked extracted disc");

    folder.Write("disc/sys/main.dol", SmallDol(0x80003120));
    Check(Outcome(root, kDefaults) == "sys/boot.bin isn't a GameCube disc's header",
          "an unmarked extracted disc whose entry point lies outside main.dol");

    folder.Write("disc/sys/main.dol", SmallDol());
    folder.Write("disc/sys/fst.bin", {0});
    Check(Outcome(root, kDefaults) == "sys/boot.bin isn't a GameCube disc's header",
          "an unmarked extracted disc with a damaged table");
}

// A file in parts reads as one. A read past the end is rejected, or padded with zeros. The messages name the whole by
// the noun given.
void TestSource()
{
    const TempFolder folder("disc_options_test");
    const fs::path a = folder.Write("a.bin", {1, 2, 3});
    const fs::path b = folder.Write("b.bin", {4, 5});
    const fs::path c = folder.Write("c.bin", {6, 7, 8, 9});
    disc::Source source({a, b, c}, "dump");

    XSFLIB_CHECK(source.Size() == 9);
    XSFLIB_CHECK((source.Read(2, 5) == std::vector<uint8_t>{3, 4, 5, 6, 7}));
    XSFLIB_CHECK(xsflib::testing::ErrorOf([&] { source.Read(7, 3); }) == "the dump is truncated");

    std::vector<uint8_t> padded(5, 0xFF);
    source.ReadPadded(6, padded.data(), padded.size());
    XSFLIB_CHECK((padded == std::vector<uint8_t>{7, 8, 9, 0, 0}));

    padded.assign(2, 0xFF);
    source.ReadPadded(20, padded.data(), padded.size());
    XSFLIB_CHECK((padded == std::vector<uint8_t>{0, 0}));

    XSFLIB_CHECK(xsflib::testing::ErrorOf([&] { disc::Source({a.parent_path() / "missing.bin"}, "dump"); }) ==
                 "can't open missing.bin");
}

// A read of 10 bytes at 6, in blocks of 4: the last 2 bytes of block 1, then blocks 2 and 3.
void TestReadByBlocks()
{
    std::vector<uint8_t> out(10);
    std::vector<std::vector<uint64_t>> pieces;
    const auto record = [&](uint64_t index, size_t within, uint8_t* piece, size_t n)
    {
        pieces.push_back({index, within, static_cast<uint64_t>(piece - out.data()), n});
    };

    disc::ReadByBlocks(6, out.data(), out.size(), 4, record);

    XSFLIB_CHECK((pieces == std::vector<std::vector<uint64_t>>{{1, 2, 0, 2}, {2, 0, 2, 4}, {3, 0, 6, 4}}));
}

void TestFormats()
{
    const std::vector<uint8_t> kRvz = {'R', 'V', 'Z', 1, 0, 0, 0, 0};
    const std::vector<uint8_t> kGcz = {0x01, 0xC0, 0x0B, 0xB1};
    const std::vector<uint8_t> kWdf = {'W', 'I', 'I', 1, 'D', 'I', 'S', 'C'};
    const std::vector<uint8_t> kCiso = {'C', 'I', 'S', 'O', 0, 0, 0, 0};
    const std::vector<uint8_t> kWbfs = {'W', 'B', 'F', 'S', 0, 0, 0, 0};
    std::vector<uint8_t> gamecube(0x20, 0);
    StoreBe32(gamecube, 0x1C, 0xC2339F3D);
    std::vector<uint8_t> wii(0x20, 0);
    StoreBe32(wii, 0x18, 0x5D1C9EA3);

    XSFLIB_CHECK(disc::ContainerFormat(kRvz) == "RVZ" && disc::ContainerFormat(kGcz) == "GCZ");
    XSFLIB_CHECK(disc::ContainerFormat(kWdf).empty() && disc::ContainerFormat({}).empty());
    XSFLIB_CHECK(disc::DiscConsole(gamecube) == disc::Console::kGameCube &&
                 disc::DiscConsole(wii) == disc::Console::kWii);
    XSFLIB_CHECK(!disc::DiscConsole(kRvz));

    XSFLIB_CHECK(disc::ImageFormat(gamecube) == "ISO" && disc::ImageFormat(wii) == "ISO");
    XSFLIB_CHECK(disc::ImageFormat(kRvz) == "RVZ" && disc::ImageFormat(kGcz) == "GCZ" &&
                 disc::ImageFormat(kCiso) == "CISO");
    XSFLIB_CHECK(disc::ImageFormat(kWbfs) == "WBFS" && disc::ImageFormat(kWdf) == "WDF");
    XSFLIB_CHECK(!disc::ImageFormat({}) && !disc::ImageFormat(std::vector<uint8_t>(0x20, 0)));
}

} // namespace

int main()
{
    TestPlainImage();
    TestMessages();
    TestDefaults();
    TestExtractedDisc();
    TestUnmarkedDisc();
    TestSource();
    TestReadByBlocks();
    TestFormats();

    return xsflib::testing::Summary("disc options");
}

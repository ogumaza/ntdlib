// SPDX-License-Identifier: MIT

// Tests of the text that ntdlib::disc decodes to UTF-8: the title in a disc's header, by the disc's region, and the
// names in a disc's file system. The expected text comes from Microsoft's tables of code pages 932 and 1252, as Unicode
// publishes them (CP932.TXT and CP1252.TXT). The images and extracted discs are made up here, in a temporary folder.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "temp_folder.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace fs = std::filesystem;
namespace disc = ntdlib::disc;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

namespace
{

using Bytes = std::vector<uint8_t>;

// How the tests load games: as a program named xyz that reads plain images of each console.
const disc::GameOptions kGameCube{disc::Console::kGameCube, "xyz"};
const disc::GameOptions kWii{disc::Console::kWii, "xyz"};

// Where a test disc keeps its parts. A Wii disc keeps them in its game partition's data.
constexpr uint32_t kBi2 = 0x440;
constexpr uint32_t kDol = 0x2500;
constexpr uint32_t kFst = 0x2800;
constexpr uint32_t kFiles = 0x3000;

// The parts of a Wii disc around its game partition.
constexpr uint32_t kPartitionTable = 0x40000;
constexpr uint32_t kRegionSetting = 0x4E000;
constexpr uint32_t kPartition = 0x50000;
constexpr uint32_t kPartitionData = 0x70000;

// The two bytes of HIRAGANA LETTER A in Shift-JIS (0x82A0), and what they decode to: U+3042 as Shift-JIS, and U+201A
// U+00A0 as Windows-1252.
constexpr char kTwoBytes[] = "\x82\xA0";
constexpr char kAsShiftJis[] = "\xE3\x81\x82";
constexpr char kAsWindows1252[] = "\xE2\x80\x9A\xC2\xA0";

// A file system table entry: a directory's flag, the name as the disc stores it, and two words. A file's words are its
// offset and size. A directory's words are its parent and the index after its last entry.
struct Entry
{
    bool directory;
    std::string name;
    uint32_t first;
    uint32_t second;
};

// The test discs' file system: a folder named in double-byte katakana with a file named in kanji, a file named in
// half-width katakana, a file whose name contains bytes that don't decode, and a file named in the codes that code page
// 932 adds to JIS X 0208 or maps otherwise. Each file holds 16 bytes. The paths that LoadGame lists follow, in the
// order of `GameFiles::files`.
const std::vector<Entry> kEntries = {
    {true, "", 0, 6},
    {true, "\x83\x54\x83\x45\x83\x93\x83\x68", 0, 3},
    {false, "\x89\xB9\x8A\x79.aw", kFiles, 16},
    {false, "\xBB\xB3\xDD\xC4\xDE.bin", kFiles + 16, 16},
    {false, "\x85\x40\xFD.bin", kFiles + 32, 16},
    {false, "\x87\x55\x81\x60\xFA\x40\xED\x40\x81\x5F\xF0\x40.bin", kFiles + 48, 16},
};

// The size of the files' data.
constexpr uint32_t kFilesSize = 64;

const std::vector<std::string> kPaths = {
    // 0x85 0x40 isn't in the table. 0x40 stays '@'. 0xFD starts no character.
    "%85@%FD.bin",
    // NEC's row 13 (0x8755, U+2161), FULLWIDTH TILDE (0x8160, U+FF5E), an IBM extension (0xFA40, U+2170), NEC's
    // selection of the IBM extensions (0xED40, U+7E8A) and FULLWIDTH REVERSE SOLIDUS (0x815F, U+FF3C). JIS0208.TXT
    // maps 0x8160 to U+301C and 0x815F to a backslash. The user area's 0xF040 maps to nothing.
    "\xE2\x85\xA1\xEF\xBD\x9E\xE2\x85\xB0\xE7\xBA\x8A\xEF\xBC\xBC%F0@.bin",
    // U+30B5 U+30A6 U+30F3 U+30C9, then U+97F3 U+697D.
    "\xE3\x82\xB5\xE3\x82\xA6\xE3\x83\xB3\xE3\x83\x89/\xE9\x9F\xB3\xE6\xA5\xBD.aw",
    // U+FF7B U+FF73 U+FF9D U+FF84 U+FF9E.
    "\xEF\xBD\xBB\xEF\xBD\xB3\xEF\xBE\x9D\xEF\xBE\x84\xEF\xBE\x9E.bin",
};

void WriteFile(const fs::path& path, const Bytes& data)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// The file system table of kEntries. A Wii disc's table gives the files' offsets divided by 4 (`shift` 2).
Bytes FileTable(int shift)
{
    Bytes table(kEntries.size() * 12, 0);
    std::string names;
    for (size_t i = 0; i < kEntries.size(); ++i)
    {
        const Entry& entry = kEntries[i];
        uint8_t* bytes = table.data() + i * 12;
        xsflib::StoreBe32(bytes, (entry.directory ? 1u << 24 : 0u) | static_cast<uint32_t>(names.size()));
        xsflib::StoreBe32(bytes + 4, entry.directory ? entry.first : entry.first >> shift);
        xsflib::StoreBe32(bytes + 8, entry.second);
        names += entry.name;
        names += '\0';
    }

    table.insert(table.end(), names.begin(), names.end());
    table.resize((table.size() + 3) & ~size_t{3}, 0);

    return table;
}

// What a GameCube disc holds from its start, or a Wii disc's game partition: the header with the game's ID, the
// console's magic word and the title, then bi2.bin with `bi2_region` at 0x18, an empty main.dol, the file system table
// and the files. A Wii partition's header gives the offsets and the table's size divided by 4.
Bytes GameData(disc::Console console, const std::string& id, const std::string& title, uint32_t bi2_region)
{
    const bool wii = console == disc::Console::kWii;
    const int shift = wii ? 2 : 0;
    const Bytes fst = FileTable(shift);
    Bytes data(kFiles + kFilesSize, 0);
    std::copy(id.begin(), id.end(), data.begin());
    xsflib::StoreBe32(data.data() + (wii ? 0x18 : 0x1C), wii ? 0x5D1C9EA3 : 0xC2339F3D);
    std::copy(title.begin(), title.end(), data.begin() + 0x20);
    xsflib::StoreBe32(data.data() + 0x420, kDol >> shift);
    xsflib::StoreBe32(data.data() + 0x424, kFst >> shift);
    xsflib::StoreBe32(data.data() + 0x428, static_cast<uint32_t>(fst.size()) >> shift);
    xsflib::StoreBe32(data.data() + kBi2 + 0x18, bi2_region);
    std::copy(fst.begin(), fst.end(), data.begin() + kFst);
    for (uint32_t i = 0; i < kFilesSize; ++i)
    {
        data[kFiles + i] = static_cast<uint8_t>(i);
    }

    return data;
}

// A GameCube disc image with `region` in its region field.
Bytes GameCubeImage(const std::string& id, const std::string& title, uint32_t region)
{
    return GameData(disc::Console::kGameCube, id, title, region);
}

// A Wii disc image with `region` in its region field, outside the partition. Its game partition is stored without
// hashes or encryption. The partition's bi2.bin gives `bi2_region` at 0x18.
Bytes WiiImage(const std::string& id, const std::string& title, uint32_t region, uint32_t bi2_region)
{
    const Bytes data = GameData(disc::Console::kWii, id, title, bi2_region);
    Bytes image(kPartitionData + data.size(), 0);
    std::copy_n(data.begin(), 0x60, image.begin());
    image[0x60] = 1; // no hashes
    image[0x61] = 1; // no encryption

    // The partition table's one group, with the game partition. Its header gives the TMD's size and offset, and the
    // data's offset and size, all but the size divided by 4.
    xsflib::StoreBe32(image.data() + kPartitionTable, 1);
    xsflib::StoreBe32(image.data() + kPartitionTable + 4, (kPartitionTable + 0x20) >> 2);
    xsflib::StoreBe32(image.data() + kPartitionTable + 0x20, kPartition >> 2);
    xsflib::StoreBe32(image.data() + kRegionSetting, region);
    xsflib::StoreBe32(image.data() + kPartition + 0x2A4, 0x40);
    xsflib::StoreBe32(image.data() + kPartition + 0x2A8, 0x2C0 >> 2);
    xsflib::StoreBe32(image.data() + kPartition + 0x2B8, (kPartitionData - kPartition) >> 2);
    xsflib::StoreBe32(image.data() + kPartition + 0x2BC, static_cast<uint32_t>(data.size() >> 2));
    std::copy(data.begin(), data.end(), image.begin() + kPartitionData);

    return image;
}

// Writes an extracted disc into `root`: sys/boot.bin, an empty sys/main.dol and sys/fst.bin. sys/bi2.bin, with
// `bi2_region` at 0x18, is written only if there's a `bi2_region`. files/ stays empty.
void WriteExtractedDisc(const fs::path& root, disc::Console console, const std::string& id, const std::string& title,
                        std::optional<uint32_t> bi2_region)
{
    const Bytes data = GameData(console, id, title, bi2_region.value_or(0));
    WriteFile(root / "sys" / "boot.bin", Bytes(data.begin(), data.begin() + kBi2));
    WriteFile(root / "sys" / "main.dol", Bytes(0x100, 0));
    WriteFile(root / "sys" / "fst.bin", FileTable(console == disc::Console::kWii ? 2 : 0));
    if (bi2_region)
    {
        WriteFile(root / "sys" / "bi2.bin", Bytes(data.begin() + kBi2, data.begin() + kBi2 + 0x2000));
    }
}

// The game that LoadGame reads from `path`, or nothing after a failed check.
std::optional<disc::GameFiles> Load(const std::string& what, const fs::path& path, const disc::GameOptions& options)
{
    disc::GameFiles game;
    const std::optional<std::string> error = disc::LoadGame(path, game, options);
    Check(!error, what + ": " + error.value_or(""));
    if (error)
    {
        return std::nullopt;
    }

    return game;
}

// Checks the title and the region field that LoadGame reads from `path`.
void CheckTitle(const std::string& what, const fs::path& path, const disc::GameOptions& options,
                const std::string& title, std::optional<uint32_t> region)
{
    const std::optional<disc::GameFiles> game = Load(what, path, options);
    if (!game)
    {
        return;
    }

    Check(game->Title() == title, what + ": the title, got \"" + game->Title() + "\"");
    Check(game->region == region, what + ": the region field");
}

// Titles of Japanese discs: kanji, half-width katakana, the two codes where Unicode's table and Windows' code page 932
// differ, codes at the ends of the table's rows, and bytes that don't decode.
void TestJapaneseTitles()
{
    const TempFolder folder("disc_text_test");
    int images = 0;
    const auto check =
        [&folder, &images](const std::string& what, const std::string& title, const std::string& expected)
    {
        const std::string name = "japan_" + std::to_string(++images) + ".iso";
        CheckTitle(what, folder.Write(name, GameCubeImage("GTSJZZ", title, 0)), kGameCube, expected, 0);
    };

    // U+8A66 U+9A13, then U+FF83 U+FF7D U+FF84.
    check("kanji and half-width katakana", "\x8E\x8E\x8C\xB1\xC3\xBD\xC4",
          "\xE8\xA9\xA6\xE9\xA8\x93\xEF\xBE\x83\xEF\xBD\xBD\xEF\xBE\x84");

    // 0x815F is U+FF3C (FULLWIDTH REVERSE SOLIDUS) and 0x8160 is U+FF5E (FULLWIDTH TILDE) in code page 932. Unicode's
    // JIS0208.TXT gives U+005C and U+301C (WAVE DASH).
    check("0x815F and 0x8160",
          "A\x81\x5F"
          "B\x81\x60",
          "A\xEF\xBC\xBC"
          "B\xEF\xBD\x9E");

    // 0x8140 is U+3000, 0x817E U+00D7, 0x8180 U+00F7, 0x889F U+4E9C, 0x9FFC U+6ECC, 0xE040 U+6F3E and 0xEAA4 U+7199,
    // the last character of JIS X 0208. 0xEAA5 isn't in the table. 0xFC4B is U+9ED1, the table's last character. 0xFC4C
    // isn't in the table, and its second byte stays 'L'.
    check("the ends of the rows", "\x81\x40\x81\x7E\x81\x80\x88\x9F\x9F\xFC\xE0\x40\xEA\xA4\xEA\xA5\xFC\x4B\xFC\x4C",
          "\xE3\x80\x80\xC3\x97\xC3\xB7\xE4\xBA\x9C\xE6\xBB\x8C\xE6\xBC\xBE\xE7\x86\x99%EA%A5\xE9\xBB\x91%FCL");

    // 0x8540 and 0x859F aren't in the table. An ASCII second byte decodes as itself. 0x80, 0xA0 and 0xFD start no
    // character. 0xF0's codes are beyond the table. A lead byte at the end has no second byte.
    check("bytes that don't decode", "\x85\x40\x85\x9F\x80\xA0\xFD\xF0\x40\x83", "%85@%85%9F%80%A0%FD%F0@%83");
}

// Titles of discs from outside Japan, in Windows-1252: Latin-1, the characters at 0x80 to 0x9F, and the five codes that
// Microsoft's table leaves undefined.
void TestOtherTitles()
{
    const TempFolder folder("disc_text_test");

    // U+00E9, U+00C4 and U+00AE.
    CheckTitle("a European disc", folder.Write("europe.iso", GameCubeImage("GTSPZZ", "Caf\xE9 \xC4pfel\xAE", 2)),
               kGameCube, "Caf\xC3\xA9 \xC3\x84pfel\xC2\xAE", 2);

    // U+20AC U+0160 U+2122 U+0178, then 0x81, 0x8D, 0x8F, 0x90 and 0x9D.
    CheckTitle("an American disc",
               folder.Write("america.iso", GameCubeImage("GTSEZZ", "\x80\x8A\x99\x9F\x81\x8D\x8F\x90\x9D", 1)),
               kGameCube, "\xE2\x82\xAC\xC5\xA0\xE2\x84\xA2\xC5\xB8%81%8D%8F%90%9D", 1);
}

// The region field, not the game ID's region letter, selects the title's encoding. On the Wii, the field is outside the
// partition. The partition's bi2.bin doesn't count. A program that reads no system files still gets the field.
void TestRegionField()
{
    const TempFolder folder("disc_text_test");
    disc::GameOptions header_only = kGameCube;
    header_only.system_files = false;

    CheckTitle("a GameCube disc of region 0 with the letter E",
               folder.Write("gamecube_0.iso", GameCubeImage("GTSEZZ", kTwoBytes, 0)), kGameCube, kAsShiftJis, 0);
    CheckTitle("a GameCube disc of region 1 with the letter J",
               folder.Write("gamecube_1.iso", GameCubeImage("GTSJZZ", kTwoBytes, 1)), kGameCube, kAsWindows1252, 1);
    CheckTitle("a GameCube disc of region 0 without system files",
               folder.Write("header_only.iso", GameCubeImage("GTSEZZ", kTwoBytes, 0)), header_only, kAsShiftJis, 0);
    CheckTitle("a Wii disc of region 0 with the letter E",
               folder.Write("wii_0.iso", WiiImage("RTSEZZ", kTwoBytes, 0, 2)), kWii, kAsShiftJis, 0);
    CheckTitle("a Wii disc of region 2 with the letter J",
               folder.Write("wii_2.iso", WiiImage("RTSJZZ", kTwoBytes, 2, 0)), kWii, kAsWindows1252, 2);
}

// Extracted discs: a GameCube disc's bi2.bin gives the region field. Without bi2.bin, and on any extracted Wii disc,
// the game ID's region letter selects the encoding: J is Japan.
void TestExtractedDiscs()
{
    const TempFolder folder("disc_text_test");
    const fs::path& root = folder.Path();
    WriteExtractedDisc(root / "gamecube_bi2", disc::Console::kGameCube, "GTSEZZ", kTwoBytes, 0);
    WriteExtractedDisc(root / "gamecube_j", disc::Console::kGameCube, "GTSJZZ", kTwoBytes, std::nullopt);
    WriteExtractedDisc(root / "gamecube_e", disc::Console::kGameCube, "GTSEZZ", kTwoBytes, std::nullopt);
    WriteExtractedDisc(root / "wii_j", disc::Console::kWii, "RTSJZZ", kTwoBytes, 2);
    WriteExtractedDisc(root / "wii_p", disc::Console::kWii, "RTSPZZ", kTwoBytes, 0);

    CheckTitle("an extracted GameCube disc of region 0 with the letter E", root / "gamecube_bi2", kGameCube,
               kAsShiftJis, 0);
    CheckTitle("an extracted GameCube disc without bi2.bin, with the letter J", root / "gamecube_j", kGameCube,
               kAsShiftJis, std::nullopt);
    CheckTitle("an extracted GameCube disc without bi2.bin, with the letter E", root / "gamecube_e", kGameCube,
               kAsWindows1252, std::nullopt);
    CheckTitle("an extracted Wii disc with the letter J", root / "wii_j", kWii, kAsShiftJis, std::nullopt);
    CheckTitle("an extracted Wii disc with the letter P", root / "wii_p", kWii, kAsWindows1252, std::nullopt);
}

// Checks that LoadGame lists kPaths from `path`, and reads each file by its path.
void CheckNames(const std::string& what, const fs::path& path, const disc::GameOptions& options)
{
    const std::optional<disc::GameFiles> game = Load(what, path, options);
    if (!game)
    {
        return;
    }

    Check(game->files.size() == kPaths.size(), what + ": the number of files");
    for (size_t i = 0; i < game->files.size() && i < kPaths.size(); ++i)
    {
        Bytes data;
        Check(game->files[i].path == kPaths[i], what + ": a path, got \"" + game->files[i].path + "\"");
        Check(game->read_file(kPaths[i], data) && data.size() == 16, what + ": reading " + kPaths[i]);
    }
}

// The names in the file system are Shift-JIS on a disc of any region.
void TestNames()
{
    const TempFolder folder("disc_text_test");

    CheckNames("a European GameCube disc", folder.Write("names.iso", GameCubeImage("GTSPZZ", "TEST", 2)), kGameCube);
    CheckNames("an American Wii disc", folder.Write("names_wii.iso", WiiImage("RTSEZZ", "TEST", 1, 1)), kWii);
}

} // namespace

int main()
{
    TestJapaneseTitles();
    TestOtherTitles();
    TestRegionField();
    TestExtractedDiscs();
    TestNames();

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("disc text tests passed\n");

    return 0;
}

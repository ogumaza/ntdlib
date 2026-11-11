// SPDX-License-Identifier: MIT

// Checks the DOL reader on a DOL built here: the sections and the entry point that Parse reads from the header, the
// size that Size gives, the bytes that At gives for a load address, the reads that throw for a missing address (From,
// Bytes, Word and Text), and ParseMain's error for a file that isn't a DOL. Also checks FindMainDol on an extracted
// disc made up here.

#include "ntdlib/dol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "ntdlib/testing/dol_files.h"

namespace
{

using ntdlib::testing::SetDolSection;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// A DOL with three sections after its 0x100-byte header. Text section 0 holds 0x20 bytes at file offset 0x100, loaded
// at 0x80003100. Text section 1 holds 0x10 bytes at file offset 0x130, loaded straight after text section 0. Data
// section 0 (section 7) holds 0x10 bytes at file offset 0x120, loaded at 0x80004000. Each byte holds its file offset's
// low 8 bits. The entry point is 0x80003100.
std::vector<uint8_t> TestDol()
{
    std::vector<uint8_t> file(0x140);
    for (std::size_t i = 0x100; i < file.size(); i++)
    {
        file[i] = static_cast<uint8_t>(i);
    }

    // The three sections, then the entry point at 0xe0.
    SetDolSection(file, 0, 0x100, 0x80003100, 0x20);
    SetDolSection(file, 1, 0x130, 0x80003120, 0x10);
    SetDolSection(file, 7, 0x120, 0x80004000, 0x10);
    xsflib::StoreBe32(&file[0xe0], 0x80003100);

    return file;
}

// Whether `bytes` are the `size` bytes of the test DOL from file offset `offset`.
bool Holds(std::span<const uint8_t> bytes, std::size_t offset, std::size_t size)
{
    const std::vector<uint8_t> file = TestDol();
    return bytes.size() == size &&
           std::equal(bytes.begin(), bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
}

void TestParse()
{
    const std::vector<uint8_t> file = TestDol();

    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);

    Check(dol && dol->Sections().size() == 3 && dol->Entry() == 0x80003100,
          "Parse reads three sections and the entry point");
    Check(dol && dol->Sections()[0].address == 0x80003100 && Holds(dol->Sections()[0].data, 0x100, 0x20) &&
              dol->Sections()[1].address == 0x80003120 && Holds(dol->Sections()[1].data, 0x130, 0x10) &&
              dol->Sections()[2].address == 0x80004000 && Holds(dol->Sections()[2].data, 0x120, 0x10),
          "Parse gives each section's address and data, in the header's order");
    Check(ntdlib::Dol::Size(file) == 0x140, "Size gives the end of the section that ends last");
}

void TestAt()
{
    const std::vector<uint8_t> file = TestDol();
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!dol)
    {
        Check(false, "the test DOL parses");
        return;
    }

    Check(Holds(dol->At(0x80003100), 0x100, 0x20) && Holds(dol->At(0x80003110), 0x110, 0x10) &&
              Holds(dol->At(0x8000311f), 0x11f, 1),
          "At gives the bytes to the end of a text section");
    Check(Holds(dol->At(0x80003120), 0x130, 0x10), "At gives the next section's bytes at a section's end");
    Check(Holds(dol->At(0x80004008), 0x128, 8), "At gives the bytes to the end of a data section");
    Check(dol->At(0x800030ff).empty(), "At gives nothing before the first section");
    Check(dol->At(0x80003130).empty() && dol->At(0x80004010).empty(), "At gives nothing past a section's end");
    Check(dol->At(0).empty(), "At gives nothing at 0");
}

void TestReads()
{
    std::vector<uint8_t> file = TestDol();
    file[0x104] = 0;
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!dol)
    {
        Check(false, "the test DOL parses");
        return;
    }

    Check(Holds(dol->From(0x80004008, "a table"), 0x128, 8), "From gives the bytes to the end of the section");
    Check(Holds(dol->Bytes(0x80003110, 0x10, "a table"), 0x110, 0x10), "Bytes gives the bytes up to a section's end");
    Check(dol->Word(0x80003108, "a table") == 0x08090a0b, "Word reads a big-endian word");
    Check(dol->Text(0x80003101, 64, "a name") == std::string("\x01\x02\x03"), "Text stops at the zero byte");
    Check(dol->Text(0x80003105, 4, "a name") == std::string("\x05\x06\x07\x08"), "Text stops at `longest` bytes");
    Check(dol->Text(0x8000312e, 64, "a name") == std::string("\x3e\x3f"), "Text stops at the section's end");

    const std::string missing = "main.dol has no data at a table";
    Check(ErrorOf<xsflib::FormatError>([&] { dol->From(0x80004010, "a table"); }) == missing,
          "From throws FormatError for an address that no section holds");
    Check(ErrorOf<xsflib::FormatError>([&] { dol->Bytes(0x80003110, 0x11, "a table"); }) == missing,
          "Bytes throws FormatError for bytes that run past a section's end");
    Check(ErrorOf<xsflib::FormatError>([&] { dol->Word(0x8000311e, "a table"); }) == missing,
          "Word throws FormatError for a word that runs past a section's end");
    Check(ErrorOf<xsflib::FormatError>([&] { dol->Text(0, 64, "a table"); }) == missing,
          "Text throws FormatError for an address that no section holds");
}

void TestParseMain()
{
    const std::vector<uint8_t> file = TestDol();

    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(file);

    Check(dol.Sections().size() == 3 && dol.Entry() == 0x80003100, "ParseMain reads a DOL as Parse does");

    const std::string message =
        ErrorOf<xsflib::FormatError>([&] { ntdlib::Dol::ParseMain(std::span(file).first(0xff)); });
    Check(message == "main.dol isn't a DOL", "ParseMain throws FormatError for a file shorter than a DOL's header");
}

void TestFindMainDol()
{
    // Dolphin and wit extract a disc into a sys folder and a files folder. A file anywhere in files finds sys/main.dol.
    // A file outside a files folder finds nothing.
    const xsflib::testing::TempFolder temp("ntdlib_dol_test");
    const std::filesystem::path& root = temp.Path();
    std::filesystem::create_directories(root / "files" / "Sound");
    std::filesystem::create_directories(root / "sys");
    const std::vector<uint8_t> kByte = {0};
    for (const std::filesystem::path& file :
         {root / "sys" / "main.dol", root / "files" / "Sound" / "a.brsar", root / "a.brsar"})
    {
        Check(!xsflib::WriteFile(file, kByte), "writes " + file.filename().string());
    }

    const std::optional<std::filesystem::path> inside = ntdlib::FindMainDol(root / "files" / "Sound" / "a.brsar");
    const std::optional<std::filesystem::path> outside = ntdlib::FindMainDol(root / "a.brsar");

    Check(inside && inside->filename() == "main.dol" && inside->parent_path().filename() == "sys",
          "a file in the files folder finds sys/main.dol");
    Check(!outside, "a file outside a files folder finds nothing");
}

// A section that starts in the header, or that runs past the file's end, is damaged. Parse then gives nothing, unless
// it skips damaged sections. Those are left out.
void TestDamagedSections()
{
    std::vector<uint8_t> in_header = TestDol();
    SetDolSection(in_header, 2, 0x80, 0x80005000, 0x10);
    std::vector<uint8_t> past_end = TestDol();
    SetDolSection(past_end, 2, 0x138, 0x80005000, 0x10);

    for (const std::vector<uint8_t>& file : {in_header, past_end})
    {
        const std::optional<ntdlib::Dol> skipped = ntdlib::Dol::Parse(file, true);
        Check(!ntdlib::Dol::Parse(file).has_value(), "a damaged section fails the DOL");
        Check(skipped && skipped->Sections().size() == 3 && skipped->Entry() == 0x80003100,
              "a damaged section is left out on request");
    }
}

} // namespace

int main()
{
    TestParse();
    TestAt();
    TestReads();
    TestParseMain();
    TestFindMainDol();
    TestDamagedSections();

    return xsflib::testing::Summary("dol");
}

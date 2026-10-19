// SPDX-License-Identifier: MIT

// Checks the DOL reader on a DOL built here: the sections and the entry point that Parse reads from the header, the
// size that Size gives, the bytes that At gives for a load address, and ParseMain's error for a file that isn't a DOL.

#include "ntdlib/dol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/testing/dol_files.h"

namespace
{

using ntdlib::testing::SetDolSection;
using xsflib::testing::Check;

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

void TestParseMain()
{
    const std::vector<uint8_t> file = TestDol();

    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(file);

    Check(dol.Sections().size() == 3 && dol.Entry() == 0x80003100, "ParseMain reads a DOL as Parse does");

    std::string message;
    try
    {
        ntdlib::Dol::ParseMain(std::span(file).first(0xff));
    }
    catch (const xsflib::FormatError& e)
    {
        message = e.what();
    }
    Check(message == "main.dol isn't a DOL", "ParseMain throws FormatError for a file shorter than a DOL's header");
}

} // namespace

int main()
{
    TestParse();
    TestAt();
    TestParseMain();

    return xsflib::testing::Summary("dol");
}

// SPDX-License-Identifier: MIT

// Tests the reader of Bezel Engine's data sheets and the sound levels (ntdlib/bezel) on sheets built here: records of
// both games' sizes, a level left to the default, a random change to the level, a level below 0, the choice among the
// records that play a sound, a table in .bss, names in .bss, the sheets that the reader leaves out, and the search for
// a game's sheets. The levels of the records follow a run of WarioWare: Get It Together!'s and WarioWare: Move It!'s
// code on their sheets.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "ntdlib/bezel/sheet.h"
#include "ntdlib/bezel/sound_levels.h"
#include "temp_folder.h"

namespace
{

namespace bezel = ntdlib::bezel;
namespace fs = std::filesystem;

using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

constexpr float kUnset = -999999.0f;
constexpr std::size_t kGetItTogether = 0x98;
constexpr std::size_t kMoveIt = 0xB0;

// A record of a sound sheet. An empty `sound` has no pointer.
struct Record
{
    std::string key;
    std::string sound;
    float level = kUnset;
    float low = kUnset;
    float high = kUnset;
};

struct List
{
    std::string symbol;
    std::vector<Record> records;
};

void Pad(std::vector<uint8_t>& out)
{
    out.resize((out.size() + 7) / 8 * 8);
}

void PutFloat(std::vector<uint8_t>& out, std::size_t offset, float value)
{
    xsflib::StoreLe32(&out[offset], std::bit_cast<uint32_t>(value));
}

// A sound sheet with records of `size` bytes: a little-endian AArch64 ELF relocatable object. Section 1 holds
// s_DefaultVoiceData and the lists, section 2 the names, sections 3 and 4 the symbols and their names, section 5 a
// relocation for each pointer to a name, against section 2's symbol. Section 6 is a .bss with the symbols in `bss` and
// their counts, at places that run past the file.
std::vector<uint8_t> SoundSheet(std::size_t size, const std::vector<List>& lists,
                                const std::vector<std::string>& bss = {})
{
    std::vector<uint8_t> data(size);
    std::vector<uint8_t> names(1);
    std::vector<uint8_t> relocations;
    std::vector<uint8_t> strings(1);
    std::vector<uint8_t> symbols(2 * 24);
    symbols[24 + 4] = 3; // STT_SECTION
    xsflib::StoreLe16(&symbols[24 + 6], 2);

    const auto add_symbol =
        [&symbols, &strings](const std::string& name, uint16_t section, uint64_t value, uint64_t bytes)
    {
        const std::size_t at = symbols.size();
        symbols.resize(at + 24);
        xsflib::StoreLe32(&symbols[at], static_cast<uint32_t>(strings.size()));
        xsflib::StoreLe16(&symbols[at + 6], section);
        xsflib::StoreLe64(&symbols[at + 8], value);
        xsflib::StoreLe64(&symbols[at + 16], bytes);
        strings.insert(strings.end(), name.begin(), name.end());
        strings.push_back(0);
    };
    const auto add_pointer = [&relocations, &names](std::size_t offset, const std::string& text)
    {
        const std::size_t at = relocations.size();
        relocations.resize(at + 24);
        xsflib::StoreLe64(&relocations[at], offset);
        xsflib::StoreLe64(&relocations[at + 8], (uint64_t{1} << 32) | 257); // symbol 1, R_AARCH64_ABS64
        xsflib::StoreLe64(&relocations[at + 16], names.size());
        names.insert(names.end(), text.begin(), text.end());
        names.push_back(0);
    };

    add_symbol("s_DefaultVoiceData", 1, 0, size);
    for (const List& list : lists)
    {
        const std::size_t start = data.size();
        data.resize(start + size * list.records.size());
        for (std::size_t i = 0; i < list.records.size(); ++i)
        {
            const Record& record = list.records[i];
            const std::size_t at = start + size * i;
            add_pointer(at, record.key);
            if (!record.sound.empty())
            {
                add_pointer(at + 0x10, record.sound);
            }
            PutFloat(data, at + 0x18, record.level);
            PutFloat(data, at + 0x4C, record.low);
            PutFloat(data, at + 0x50, record.high);
        }
        add_symbol(list.symbol, 1, start, size * list.records.size());

        const std::size_t count = data.size();
        data.resize(count + 8);
        xsflib::StoreLe32(&data[count], static_cast<uint32_t>(list.records.size()));
        add_symbol(list.symbol + "Count", 1, count, 4);
    }
    for (const std::string& symbol : bss)
    {
        add_symbol(symbol, 6, 0, 0x8000000);
        add_symbol(symbol + "Count", 6, 0x8000000, 4);
    }

    std::vector<uint8_t> out(0x40);
    out[0] = 0x7F;
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = 2;
    out[5] = 1;
    xsflib::StoreLe16(&out[0x12], 183);
    xsflib::StoreLe16(&out[0x3A], 0x40);
    xsflib::StoreLe16(&out[0x3C], 7);

    struct Section
    {
        uint32_t type;
        const std::vector<uint8_t>* bytes;
        uint32_t link;
        uint32_t info;
    };
    const std::vector<uint8_t> none;
    const Section sections[] = {{1, &data, 0, 0},    {1, &names, 0, 0},       {2, &symbols, 4, 2},
                                {3, &strings, 0, 0}, {4, &relocations, 3, 1}, {8, &none, 0, 0}};
    std::vector<uint8_t> headers(0x40);
    for (const Section& section : sections)
    {
        Pad(out);
        const std::size_t header = headers.size();
        headers.resize(header + 0x40);
        xsflib::StoreLe32(&headers[header + 4], section.type);
        xsflib::StoreLe64(&headers[header + 0x18], out.size());
        xsflib::StoreLe64(&headers[header + 0x20], section.type == 8 ? 0x10000000 : section.bytes->size());
        xsflib::StoreLe32(&headers[header + 0x28], section.link);
        xsflib::StoreLe32(&headers[header + 0x2C], section.info);
        out.insert(out.end(), section.bytes->begin(), section.bytes->end());
    }
    Pad(out);
    xsflib::StoreLe64(&out[0x28], out.size());
    out.insert(out.end(), headers.begin(), headers.end());

    return out;
}

bezel::Sheet Parsed(std::vector<uint8_t> bytes)
{
    bezel::Sheet sheet;
    const std::optional<std::string> error = bezel::Sheet::Parse(std::move(bytes), sheet);
    Check(!error, "the sheet parses: " + error.value_or(""));

    return sheet;
}

bezel::SoundLevels LevelsOf(std::size_t size, const std::vector<List>& lists)
{
    bezel::SoundLevels levels;
    levels.AddSheet(Parsed(SoundSheet(size, lists)));

    return levels;
}

void TestSheet()
{
    const bezel::Sheet sheet = Parsed(SoundSheet(kGetItTogether, {{"s_VoiceDataTable", {{"BGM_A", "", 0.5f}}},
                                                                  {"s_voiceData_B", {{"B_1", "BGM_B", 0.5f}}},
                                                                  {"s_voiceData_C", {{"C_1", "BGM_C", 0.5f}}}}));
    Check(sheet.Symbol("s_VoiceDataTable").size() == kGetItTogether, "a symbol's bytes come from its size");
    Check(sheet.Symbol("s_Nothing").empty(), "a symbol that isn't there has no bytes");
    Check(sheet.StringAt("s_voiceData_B", 0x10) == "BGM_B", "a pointer gives its string");
    Check(!sheet.StringAt("s_voiceData_B", 0x20), "a place without a pointer gives none");
    const std::vector<std::string> expected = {"s_voiceData_B", "s_voiceData_BCount", "s_voiceData_C",
                                               "s_voiceData_CCount"};
    Check(sheet.SymbolsStartingWith("s_voiceData_") == expected, "the symbols that start with a prefix, in order");

    bezel::Sheet other;
    Check(bezel::Sheet::Parse({1, 2, 3}, other).has_value(), "a file that isn't an ELF object is refused");
}

// The records' levels, each named after its sound, in both games' sizes. 0.3 is BGM_P_Pipe_Success's level in Get It
// Together!, and 2.37 BGM_BossDance's in Move It!.
void TestLevels()
{
    const bezel::SoundLevels git = LevelsOf(kGetItTogether, {{"s_VoiceDataTable",
                                                              {{"BGM_P_Pipe_Success", "", 0.3f},
                                                               {"BGM_Named_Twice", "BGM_Named_Twice", 0.71f},
                                                               {"BGM_Default", ""},
                                                               {"BGM_Change", "", 0.5f, -0.25f, 0.75f},
                                                               {"BGM_Change_On_Default", "", kUnset, -0.5f, -0.25f},
                                                               {"BGM_Below_0", "", 0.25f, -0.75f, -0.25f},
                                                               {"BGM_Silence", "Silence", 0.0f}}}});
    Check(git.LevelOf("BGM_P_Pipe_Success") == 0.3f, "a record named after its sound gives the level");
    Check(git.LevelOf("BGM_Named_Twice") == 0.71f, "a record that names itself as its sound gives the level");
    Check(git.LevelOf("BGM_Default") == 1.0f, "a level left to the default is 1");
    Check(git.LevelOf("BGM_Change") == 0.75f, "the middle of the change's bounds adds to the level");
    Check(git.LevelOf("BGM_Change_On_Default") == 0.625f, "a change adds to the default too");
    Check(git.LevelOf("BGM_Below_0") == 0.0f, "a level below 0 is 0");
    Check(git.LevelOf("Silence") == 0.0f, "a level of 0 stays");
    Check(git.LevelOf("BGM_No_Record") == 1.0f, "a sound without a record plays at 1");
    Check(git.Count() == 7, "seven sounds have records");

    const bezel::SoundLevels move_it = LevelsOf(kMoveIt, {{"s_VoiceDataList", {{"BGM_BossDance", "", 2.37f}}},
                                                          {"s_VoiceDataList_0", {{"BGM_Other_List", "", 0.56f}}}});
    Check(move_it.LevelOf("BGM_BossDance") == 2.37f, "Move It!'s size and table");
    Check(move_it.LevelOf("BGM_Other_List") == 0.56f, "Move It!'s other lists");
}

// A sound can be played by records with other names. Those of BGM_P_Success3, with levels 0.7, 0.85, 0.89 and 1 in Get
// It Together!, are the model here.
void TestChoiceAmongRecords()
{
    const std::vector<Record> records = {
        {"Most_1", "BGM_Most", 0.7f}, {"Most_2", "BGM_Most", 0.85f}, {"Most_3", "BGM_Most", 0.85f},
        {"Most_4", "BGM_Most", 1.0f}, {"Tie_1", "BGM_Tie", 0.4f},    {"Tie_2", "BGM_Tie", 0.7f},
        {"Over_1", "BGM_Over", 0.5f}, {"Over_2", "BGM_Over", 0.5f},  {"BGM_Over", "", 0.25f}};
    std::vector<Record> reversed = records;
    std::reverse(reversed.begin(), reversed.end());
    for (const std::vector<Record>& order : {records, reversed})
    {
        const bezel::SoundLevels levels = LevelsOf(kGetItTogether, {{"s_VoiceDataTable", order}});
        Check(levels.LevelOf("BGM_Most") == 0.85f, "the level that most of the records give");
        Check(levels.LevelOf("BGM_Tie") == 0.7f, "the higher level on a tie");
        Check(levels.LevelOf("BGM_Over") == 0.25f, "the record named after the sound comes first");
    }
}

// A table and a count in .bss have no bytes in the file, whatever bytes follow the section's place in it.
void TestBss()
{
    bezel::SoundLevels levels;
    const bezel::Sheet sheet = Parsed(SoundSheet(kGetItTogether, {}, {"s_VoiceDataTable"}));
    Check(sheet.Symbol("s_VoiceDataTable").empty(), "a symbol in .bss has no bytes");
    Check(sheet.Symbol("s_VoiceDataTableCount").empty(), "a count in .bss has no bytes");
    levels.AddSheet(sheet);
    Check(levels.Count() == 0, "a table in .bss adds no records");
}

// A symbol table whose names lie in a section without bytes in the file, such as .bss, gives no symbols.
void TestNamesInBss()
{
    std::vector<uint8_t> bytes = SoundSheet(kGetItTogether, {{"s_VoiceDataTable", {{"BGM_A", "", 0.5f}}}});
    const uint64_t symbol_table = xsflib::LoadLe64(&bytes[0x28]) + 3 * 0x40;
    xsflib::StoreLe32(&bytes[symbol_table + 0x28], 6);

    const bezel::Sheet sheet = Parsed(std::move(bytes));

    Check(sheet.Symbol("s_VoiceDataTable").empty(), "a symbol has no bytes without its name");
    Check(sheet.SymbolsStartingWith("s_").empty(), "the table gives no names");
}

void TestSheetNames()
{
    Check(bezel::IsSoundSheet("SoundVoice_BGM.elf"), "the music's sheet");
    Check(bezel::IsSoundSheet("SoundVoice_SE_Petit_A-E.elf"), "a sound effects' sheet");
    Check(!bezel::IsSoundSheet("SoundVoice_Voice_enUS.elf"), "a voice sheet is left out");
    Check(!bezel::IsSoundSheet("SoundEffectList.elf"), "another sheet is left out");
    Check(!bezel::IsSoundSheet("SoundVoice_BGM.txt"), "another file is left out");
}

void TestFolders()
{
    const TempFolder folder("bezel_test");
    const fs::path archive = folder.Path() / "romfs/Archive/Sound.nx.bea/Audio/Sound.bfsar";
    const std::string sheets_name = "romfs/Archive/Data~SoundData.nx.bea/Data/SheetBin";
    const fs::path sheets = folder.Path() / sheets_name;
    fs::create_directories(archive.parent_path());
    Check(!bezel::FindSoundSheets(archive), "no sheets to find");

    fs::create_directories(sheets);
    const auto write = [&folder, &sheets_name](const std::string& name, const std::vector<uint8_t>& bytes)
    {
        folder.Write(sheets_name + "/" + name, bytes);
    };
    write("SoundEffectList.elf", {});
    Check(!bezel::FindSoundSheets(archive), "a folder without a sound sheet doesn't count");

    write("SoundVoice_BGM.elf", SoundSheet(kGetItTogether, {{"s_VoiceDataTable", {{"BGM_A", "", 0.5f}}}}));
    write("SoundVoice_SE_Common.elf", SoundSheet(kGetItTogether, {{"s_VoiceDataTable", {{"SE_A", "", 0.25f}}}}));
    write("SoundVoice_SE_Damaged.elf", {1, 2, 3});
    write("SoundVoice_Voice_enUS.elf", SoundSheet(kGetItTogether, {{"s_VoiceDataTable", {{"BGM_A", "", 0.125f}}}}));

    const std::optional<fs::path> found = bezel::FindSoundSheets(archive);
    Check(found && fs::equivalent(*found, sheets), "the search goes up to the folder of the game's unpacked archives");

    const fs::path beside = folder.Path() / "romfs/Archive/Data~SoundData.nx.bea/Data/Other/Sound.bfsar";
    fs::create_directories(beside.parent_path());
    const std::optional<fs::path> near = bezel::FindSoundSheets(beside);
    Check(near && fs::equivalent(*near, sheets), "the search finds Data/SheetBin in a folder above");

    const fs::path deep = folder.Path() / "romfs/Archive/Sound.nx.bea/a/b/c/d/Sound.bfsar";
    fs::create_directories(deep.parent_path());
    Check(!bezel::FindSoundSheets(deep), "the search stops three folders up");

    const bezel::SoundLevels levels = bezel::ReadSoundLevels(sheets);
    Check(levels.Count() == 2, "the two sound sheets give their records");
    Check(levels.LevelOf("BGM_A") == 0.5f, "a voice sheet changes nothing");
    Check(levels.LevelOf("SE_A") == 0.25f, "every sound sheet in the folder counts");
}

} // namespace

int main()
{
    TestSheet();
    TestLevels();
    TestChoiceAmongRecords();
    TestBss();
    TestNamesInBss();
    TestSheetNames();
    TestFolders();

    return xsflib::testing::Summary("bezel");
}

// SPDX-License-Identifier: MIT

// The addresses in the comments are those of Pinball Challenge Deluxe, the European release (game code APLP).

#include "mod1_pieces.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"

namespace ntdlib::logikstate
{

using ntdlib::agb::Hex;
using ntdlib::agb::kRomBase;

namespace
{

// What a game's code reads of its records of the tables that play GBAMOD1 modules: the layout of a record, and the
// offsets of the orders that it starts as music and as jingles. Only the game's code says which offset is which. A
// table of games therefore lists them. The orders themselves, and the event entries, are read from the ROM.
struct Mod1Game
{
    const char* game_code;
    uint32_t module_word; // the record's word with its module's address
    uint32_t data_start;  // the record's word with the address of the table's data: a zero word, then the entries
    uint32_t data_end;    // the record's word with the address of the data's end, `record_size` after the record
    uint32_t record_size;
    std::array<uint32_t, 5> music;   // bytes of the record: orders that the game plays as music
    std::array<uint32_t, 5> jingles; // bytes of the record: orders that the game plays as jingles
    uint32_t marked_jingle;          // a byte of the record that names a jingle where the record starts with `mark`
    uint32_t mark;
    size_t title_module; // the module that the game's code names, and the orders that it plays as music
    std::array<uint32_t, 2> title_orders;
};

constexpr Mod1Game kMod1Games[] = {
    // Pinball Challenge Deluxe. The table setup (0x08009A20) copies the bytes from the record's word +0x10 to its word
    // +0x14 to RAM, the record last. Every play call of a table reads the module at +0x60.
    // Music: +0x224 (0x08007D98, 0x0800802A), +0x228 (0x080082E6), +0x22C (the launch, 0x0800558A and 0x080055BE),
    // +0x230 (0x080083EC) and +0x240 (0x08007FB2).
    // Jingles: +0x234 (0x08009454), +0x238 (0x0800C194), +0x23C (0x08007FDE, 0x0800824C), +0x244 (0x08007D0C) and
    // +0x248 (0x08006E80, 0x08007452, 0x080082B0). State 8 plays +0x258 as a jingle (0x0800855E), and only a record
    // whose first word is 0x23232323 reaches it (0x08007F10).
    // The title plays module 1 from order 0 (0x08003882, 0x0800394C) and from order 3 (0x08003F8A, 0x08003F98).
    {"APLP",
     0x60,
     0x10,
     0x14,
     0x274,
     {0x224, 0x228, 0x22C, 0x230, 0x240},
     {0x234, 0x238, 0x23C, 0x244, 0x248},
     0x258,
     0x23232323,
     1,
     {0, 3}},
};

// An event entry of 4 bytes. With bit 7 of byte 0 set, the entry is a jingle of the order in the low 7 bits, at the
// priority in byte 2, and byte 3 is 0 (0x0800B208, 0x0800B17C). Otherwise byte 0 is a sample for a sound effect, and
// byte 3 is 3. The array ends at the first word that is neither.
constexpr uint8_t kJingleEntry = 0x80;
constexpr uint8_t kEntryOrder = 0x7F;
constexpr uint8_t kLargestJingleByte1 = 4;
constexpr uint8_t kSampleEntryByte3 = 3;
constexpr uint8_t kSampleEntryLoud = 0x20;

bool IsEntry(const Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, 4))
    {
        return false;
    }
    if (rom.U8(at) & kJingleEntry)
    {
        return rom.U8(at + 3) == 0 && rom.U8(at + 1) <= kLargestJingleByte1;
    }

    return rom.U8(at + 3) == kSampleEntryByte3 && (rom.U8(at + 2) == 0 || rom.U8(at + 2) == kSampleEntryLoud);
}

// Returns the record of the table that plays `module`: the one record whose module word contains the module's address,
// whose end word is a record's size after it, and whose data word names a zero word before it. Returns 0 if none or
// several pass.
uint32_t FindRecord(const Rom& rom, uint32_t module, const Mod1Game& game)
{
    uint32_t found = 0;
    int count = 0;
    for (uint32_t at = kRomBase; rom.Contains(at, 4); at += 4)
    {
        if (rom.U32(at) != module || at < kRomBase + game.module_word)
        {
            continue;
        }

        const uint32_t record = at - game.module_word;
        const uint32_t data = rom.U32(record + game.data_start);
        if (rom.U32(record + game.data_end) == record + game.record_size && data < record && rom.Contains(data, 4) &&
            rom.U32(data) == 0)
        {
            found = record;
            count++;
        }
    }

    return count == 1 ? found : 0;
}

// What the game's data says of a piece.
struct Starts
{
    bool music = false;
    bool jingle = false;
    bool named = false; // an event entry names it as a jingle, and no word of the ROM points at the entry
};

// A table's record and the jingle entries of its data.
struct Table
{
    size_t module = 0;
    uint32_t record = 0;
    std::vector<uint32_t> entries;
};

// Returns, for each jingle entry of `tables`, the count of the ROM's words that contain its address.
std::map<uint32_t, int> CountPointers(const Rom& rom, const std::vector<Table>& tables)
{
    std::map<uint32_t, int> counts;
    for (const Table& t : tables)
    {
        for (uint32_t entry : t.entries)
        {
            counts[entry] = 0;
        }
    }
    for (uint32_t at = kRomBase; rom.Contains(at, 4); at += 4)
    {
        const auto it = counts.find(rom.U32(at));
        if (it != counts.end())
        {
            it->second++;
        }
    }

    return counts;
}

// Marks the piece that contains order `order`. `pieces` lists the songs of the module, in order.
void Mark(const std::vector<size_t>& pieces, const DriverInfo& info, uint32_t order, uint32_t order_count,
          std::vector<Starts>& starts, bool Starts::* what)
{
    if (order >= order_count)
    {
        return;
    }

    for (size_t i = pieces.size(); i-- > 0;)
    {
        if (uint32_t(info.songs[pieces[i]].order) <= order)
        {
            starts[pieces[i]].*what = true;
            return;
        }
    }
}

} // namespace

void ClassifyMod1Pieces(const Rom& rom, const std::vector<uint32_t>& modules, DriverInfo& info)
{
    const Mod1Game* game = nullptr;
    for (const Mod1Game& g : kMod1Games)
    {
        game = rom.GameCode() == g.game_code ? &g : game;
    }
    if (!game)
    {
        info.log.push_back("no table of the game's pieces, so each piece plays as the driver plays it");
        return;
    }

    // Each module's songs, and the records of the tables, with the jingle entries of their data.
    std::vector<std::vector<size_t>> pieces(modules.size());
    for (size_t s = 0; s < info.songs.size(); s++)
    {
        for (size_t m = 0; m < modules.size(); m++)
        {
            if (info.songs[s].module == modules[m])
            {
                pieces[m].push_back(s);
            }
        }
    }
    std::vector<Table> tables;
    for (size_t m = 0; m < modules.size(); m++)
    {
        Table t{m, FindRecord(rom, modules[m], *game), {}};
        for (uint32_t at = t.record ? rom.U32(t.record + game->data_start) + 4 : 0; t.record && IsEntry(rom, at);
             at += 4)
        {
            if (rom.U8(at) & kJingleEntry)
            {
                t.entries.push_back(at);
            }
        }
        if (t.record)
        {
            tables.push_back(t);
        }
    }
    const std::map<uint32_t, int> pointers = CountPointers(rom, tables);

    // The starts that the records and the entries name, and the title's.
    std::vector<Starts> starts(info.songs.size());
    for (const Table& t : tables)
    {
        Module module;
        ReadModule(rom, modules[t.module], module);
        const auto mark = [&](uint32_t order, bool Starts::* what)
        {
            Mark(pieces[t.module], info, order, module.order_count, starts, what);
        };
        for (uint32_t offset : game->music)
        {
            mark(rom.U8(t.record + offset), &Starts::music);
        }
        for (uint32_t offset : game->jingles)
        {
            mark(rom.U8(t.record + offset), &Starts::jingle);
        }
        if (rom.U32(t.record) == game->mark)
        {
            mark(rom.U8(t.record + game->marked_jingle), &Starts::jingle);
        }
        for (uint32_t entry : t.entries)
        {
            mark(rom.U8(entry) & kEntryOrder, pointers.at(entry) ? &Starts::jingle : &Starts::named);
        }
    }
    if (game->title_module < modules.size())
    {
        Module module;
        ReadModule(rom, modules[game->title_module], module);
        for (uint32_t order : game->title_orders)
        {
            Mark(pieces[game->title_module], info, order, module.order_count, starts, &Starts::music);
        }
    }

    // A piece that only jingles start, or that only an entry without a pointer names, plays once. A piece that the game
    // starts as music, or as music and as a jingle, loops as the driver plays it, and so does a piece that nothing
    // names.
    int music = 0, jingles = 0, named = 0;
    for (size_t s = 0; s < info.songs.size(); s++)
    {
        const Starts& st = starts[s];
        info.songs[s].plays_once = !st.music && (st.jingle || st.named);
        music += st.music ? 1 : 0;
        jingles += !st.music && st.jingle ? 1 : 0;
        named += !st.music && !st.jingle && st.named ? 1 : 0;
    }

    info.log.push_back(std::to_string(tables.size()) + " tables of the game's pieces, the first at " +
                       (tables.empty() ? std::string("-") : Hex(tables.front().record)));
    info.log.push_back(
        std::to_string(music) + " pieces loop as music, " + std::to_string(jingles) + " play once as jingles, " +
        std::to_string(named) + " play once as jingles that nothing starts, " +
        std::to_string(int(info.songs.size()) - music - jingles - named) + " play as the driver plays them");
}

} // namespace ntdlib::logikstate

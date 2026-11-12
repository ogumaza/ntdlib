// SPDX-License-Identifier: MIT

#include "ntdlib/logikstate/song.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/mod30.h"

namespace ntdlib::logikstate
{

using ntdlib::agb::WriteTextFile;

namespace
{

// The names of the effects that the player runs, by the effect's number. The player ignores the others.
constexpr std::array<const char*, 16> kEffectNames = {
    "arpeggio",
    "slide up",
    "slide down",
    "slide to note",
    "vibrato",
    "slide to note + volume slide",
    "vibrato + volume slide",
    nullptr,
    nullptr,
    nullptr,
    "volume slide",
    "jump",
    "volume",
    "break",
    "extended",
    "speed/tempo",
};

// The effects that the GBAMOD1 build ignores, besides those without a name.
constexpr std::array<uint16_t, 3> kMod1Ignored = {0x5, 0x6, 0xE};

// Returns the name of an effect in `format`, or nullptr for one that the player ignores.
const char* EffectName(uint16_t effect, Format format)
{
    const bool ignored =
        format == Format::kGbaMod1 && std::find(kMod1Ignored.begin(), kMod1Ignored.end(), effect) != kMod1Ignored.end();
    return effect < kEffectNames.size() && !ignored ? kEffectNames[effect] : nullptr;
}

// Returns the text of a cell: its pitch as a place in the table of pitches, its sample, and its effect with the
// operand, each left out where the cell has none.
std::string CellText(const Cell& cell, Format format)
{
    char text[96];
    std::string out;
    if (cell.pitch != kNoPitch)
    {
        std::snprintf(text, sizeof text, " pitch %d", cell.pitch);
        out += text;
    }
    if (cell.sample != 0)
    {
        std::snprintf(text, sizeof text, " sample %d", cell.sample);
        out += text;
    }
    if (cell.effect != 0 || cell.operand != 0)
    {
        const char* name = EffectName(cell.effect, format);
        std::snprintf(text, sizeof text, " effect %X %02X (%s)", unsigned(cell.effect), unsigned(cell.operand),
                      name ? name : "none");
        out += text;
    }

    return out;
}

// Returns the listing of pattern `pattern`: a line for each row that has a cell with a note, a sample or an effect.
std::string PatternText(const Rom& rom, const Module& module, uint32_t pattern)
{
    char text[96];
    std::snprintf(text, sizeof text, "Pattern %u at 0x%08X: %u rows\n", unsigned(pattern),
                  unsigned(module.patterns[pattern]), unsigned(module.rows[pattern]));
    std::string out = text;
    for (uint32_t row = 0; row < module.rows[pattern]; row++)
    {
        std::string line;
        for (uint32_t c = 0; c < module.channels; c++)
        {
            const uint32_t address = module.patterns[pattern] + (row * module.channels + c) * kCellSize;
            const std::string cell = CellText(ReadCell(rom, address), module.format);
            if (!cell.empty())
            {
                line += "  ch" + std::to_string(c + 1) + ":" + cell;
            }
        }
        if (!line.empty())
        {
            std::snprintf(text, sizeof text, "  row %2u:", unsigned(row));
            out += text + line + "\n";
        }
    }

    return out;
}

// Returns the text of a cell of a GBAMOD30 module: its note, its sample, its volume and its effect with the operand,
// each left out where the cell has none.
std::string Mod30CellText(const Mod30Cell& cell)
{
    char text[96];
    std::string out;
    if (cell.note != kMod30NoNote)
    {
        std::snprintf(text, sizeof text, " note %d", cell.note);
        out += text;
    }
    if (cell.sample != 0)
    {
        std::snprintf(text, sizeof text, " sample %d", cell.sample);
        out += text;
    }
    if (cell.volume != 0)
    {
        std::snprintf(text, sizeof text, " volume %d", cell.volume - 1);
        out += text;
    }
    if (cell.effect != 0 || cell.operand != 0)
    {
        const char* name = cell.effect < kEffectNames.size() ? kEffectNames[cell.effect] : nullptr;
        std::snprintf(text, sizeof text, " effect %X %02X (%s)", unsigned(cell.effect), unsigned(cell.operand),
                      name ? name : "none");
        out += text;
    }

    return out;
}

// Writes a listing of a GBAMOD30 module: its header, its orders, the bank's samples that its cells name, and every row
// of its patterns that has a cell with something in it.
bool DumpMod30(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    Mod30Module module;
    Mod30Bank bank;
    const Song& entry = info.songs[size_t(song)];
    if (!ReadMod30Module(rom, entry.module, module) ||
        !ReadMod30Bank(rom, info.mod30.bank, info.mod30.header_size, bank))
    {
        error = "module " + std::to_string(song) + " can't be read";
        return false;
    }

    static constexpr std::array<char, 3> kLayoutNames = {'A', 'B', 'C'};
    char line[200];
    std::snprintf(line, sizeof line,
                  "Module at 0x%08X: layout %c, %u channels, %u orders, %u patterns of %u rows, speed %u, tempo %u, "
                  "%s periods\n",
                  unsigned(module.address), kLayoutNames[size_t(module.layout)], unsigned(module.channels),
                  unsigned(module.order_count), unsigned(module.pattern_count), unsigned(module.rows),
                  unsigned(module.speed), unsigned(module.tempo), module.amiga ? "Amiga" : "linear");
    std::string out = line;
    std::snprintf(line, sizeof line, "Bank of samples at 0x%08X, the loop flag %s\n", unsigned(bank.address),
                  entry.loop_flag ? "on" : "off");
    out += line;
    out += "Orders:";
    for (uint32_t i = 0; i < module.real_orders; i++)
    {
        out += " " + std::to_string(module.orders[i]);
    }
    out += "\n";

    // Each pattern's cells, and the samples that they name.
    std::vector<std::string> patterns;
    std::array<bool, kMod30Samples> used = {};
    for (uint32_t p = 0; p < module.pattern_count; p++)
    {
        const uint32_t block = module.patterns[p];
        std::vector<std::vector<Mod30Cell>> cells;
        std::string text;
        std::snprintf(line, sizeof line, "Pattern %u at 0x%08X\n", unsigned(p), unsigned(block));
        text = line;
        if (!DecodeMod30Pattern(rom, block, module.channels, module.rows, cells, error))
        {
            text += "  " + error + "\n";
            error.clear();
            patterns.push_back(text);
            continue;
        }
        for (uint32_t row = 0; row < module.rows; row++)
        {
            std::string cells_text;
            for (uint32_t c = 0; c < module.channels; c++)
            {
                const Mod30Cell& cell = cells[c][row];
                if (cell.sample != 0)
                {
                    used[cell.sample - 1u] = true;
                }
                const std::string cell_text = Mod30CellText(cell);
                if (!cell_text.empty())
                {
                    cells_text += "  ch" + std::to_string(c + 1) + ":" + cell_text;
                }
            }
            if (!cells_text.empty())
            {
                std::snprintf(line, sizeof line, "  row %2u:", unsigned(row));
                text += line + cells_text + "\n";
            }
        }
        patterns.push_back(text);
    }

    for (size_t i = 0; i < used.size(); i++)
    {
        if (!used[i])
        {
            continue;
        }

        const Mod30Sample& s = bank.samples[i];
        std::snprintf(
            line, sizeof line,
            "Sample %zu: points at 0x%08X, length %u, relative note %d, fine tune %d, volume %u, loop from %u "
            "for %u\n",
            i + 1, unsigned(bank.points[i]), unsigned(s.length), int(s.relative_note), int(s.fine_tune),
            unsigned(s.volume), unsigned(s.loop_start), unsigned(s.loop_length));
        out += line;
    }
    for (const std::string& text : patterns)
    {
        out += text;
    }

    return WriteTextFile(path, out, error);
}

} // namespace

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    if (info.format == Format::kGbaMod30 && song >= 0 && size_t(song) < info.songs.size())
    {
        return DumpMod30(rom, info, song, path, error);
    }

    Module module;
    if (song < 0 || size_t(song) >= info.songs.size() || !ReadModule(rom, info.songs[size_t(song)].module, module))
    {
        error = "module " + std::to_string(song) + " can't be read";
        return false;
    }

    // The header, and where the song starts and reads its samples from.
    const Song& entry = info.songs[size_t(song)];
    char line[160];
    std::snprintf(line, sizeof line, "Module at 0x%08X: %u channels, %u orders, %u patterns, mixed at %u Hz\n",
                  unsigned(module.address), unsigned(module.channels), unsigned(module.order_count),
                  unsigned(module.pattern_count), unsigned(module.rate));
    std::string out = line;
    if (module.format == Format::kGbaMod1)
    {
        std::snprintf(line, sizeof line, "Starts at order %d, with the range of orders %d to %d\n", entry.order,
                      entry.first_order, entry.last_order);
    }
    else if (entry.bank != 0 && !module.own_samples)
    {
        std::snprintf(line, sizeof line, "Starts at order %d, with the bank of samples at 0x%08X\n", entry.order,
                      unsigned(entry.bank));
    }
    else
    {
        std::snprintf(line, sizeof line, "Starts at order %d, with the samples after its patterns\n", entry.order);
    }
    out += line;

    out += "Orders:";
    for (uint32_t i = 0; i < module.order_count; i++)
    {
        out += " " + std::to_string(module.orders[i]);
    }
    out += "\n";

    // The samples that have points or a volume, with the address of each one's points.
    const std::array<uint32_t, kSamples> points = SamplePoints(rom, module, entry);
    for (size_t i = 0; i < module.samples.size(); i++)
    {
        const Sample& s = module.samples[i];
        if (s.length == 0 && s.volume == 0 && points[i] == 0)
        {
            continue;
        }

        std::snprintf(line, sizeof line,
                      "Sample %zu: points at 0x%08X, length %u, fine tune %u, volume %u, loop from %u for %u\n", i + 1,
                      unsigned(points[i]), unsigned(s.length), unsigned(s.fine_tune), unsigned(s.volume),
                      unsigned(s.loop_start), unsigned(s.loop_length));
        out += line;
    }

    for (uint32_t p = 0; p < module.pattern_count; p++)
    {
        out += PatternText(rom, module, p);
    }

    return WriteTextFile(path, out, error);
}

} // namespace ntdlib::logikstate

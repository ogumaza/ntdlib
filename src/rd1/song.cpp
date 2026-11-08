// SPDX-License-Identifier: MIT

// Reading the Standard MIDI Files that Nintendo R&D1's driver plays.

#include "ntdlib/rd1/song.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"

namespace ntdlib::rd1
{

using ntdlib::agb::Hex;
using ntdlib::agb::Rom;
using ntdlib::agb::WriteTextFile;

namespace
{

uint32_t BigEndian32(const Rom& rom, uint32_t at)
{
    return uint32_t(rom.U8(at)) << 24 | uint32_t(rom.U8(at + 1)) << 16 | uint32_t(rom.U8(at + 2)) << 8 | rom.U8(at + 3);
}

uint16_t BigEndian16(const Rom& rom, uint32_t at)
{
    return uint16_t(rom.U8(at) << 8 | rom.U8(at + 1));
}

// Appends a line for each event of the track from `at` to `end` to `text`.
void DumpTrack(const Rom& rom, uint32_t at, uint32_t end, std::string& text)
{
    char line[64];
    uint64_t tick = 0;
    int status = 0;

    const auto bytes = [&](uint32_t from, uint32_t length)
    {
        for (uint32_t k = 0; k < length && k < 64; k++)
        {
            std::snprintf(line, sizeof line, " %02X", unsigned(rom.U8(from + k)));
            text += line;
        }
    };

    while (at < end)
    {
        tick += ReadVariable(rom, at);
        if (at >= end)
        {
            break;
        }

        if (rom.U8(at) & 0x80)
        {
            status = rom.U8(at++);
        }
        std::snprintf(line, sizeof line, "  %8llu %08X %02X", static_cast<unsigned long long>(tick), unsigned(at),
                      unsigned(status));
        text += line;

        if (status == 0xFF)
        {
            const int type = rom.U8(at++);
            const uint32_t length = ReadVariable(rom, at);
            std::snprintf(line, sizeof line, " meta %02X:", unsigned(type));
            text += line;
            bytes(at, length);
            at += length;
        }
        else if (status >= 0xF0)
        {
            const uint32_t length = ReadVariable(rom, at);
            text += " sysex:";
            bytes(at, length);
            at += length;
        }
        else
        {
            const uint32_t size = (status & 0xE0) == 0xC0 ? 1 : 2;
            bytes(at, size);
            at += size;
        }

        text += "\n";
    }
}

} // namespace

uint32_t ReadVariable(const Rom& rom, uint32_t& at)
{
    uint32_t value = 0;
    for (;;)
    {
        const uint8_t b = rom.U8(at++);
        value = value << 7 | (b & 0x7F);
        if (!(b & 0x80) || !rom.Contains(at))
        {
            return value;
        }
    }
}

bool ReadSongFile(const Rom& rom, uint32_t address, SongFile& file, std::string& error)
{
    // The start routine skips "MThd", reads the header's length, then the format, the track count and the division.
    if (!rom.Contains(address, 14) || BigEndian32(rom, address) != 0x4D546864u)
    {
        error = "no MIDI file at " + Hex(address);
        return false;
    }

    const uint32_t header = BigEndian32(rom, address + 4);
    file.format = BigEndian16(rom, address + 8);
    const int tracks = BigEndian16(rom, address + 10) & 0x1F;
    file.division = BigEndian16(rom, address + 12);
    if (file.division == 0 || file.division & 0x8000)
    {
        error = "the MIDI file at " + Hex(address) + " has a division the driver doesn't play";
        return false;
    }

    uint32_t at = address + 8 + header;
    file.tracks.clear();
    for (int t = 0; t < tracks && t < kMaxChannels; t++)
    {
        if (!rom.Contains(at, 8))
        {
            error = "the MIDI file at " + Hex(address) + " ends before its track " + std::to_string(t);
            return false;
        }

        const uint32_t length = BigEndian32(rom, at + 4);
        MidiTrackRange range;
        range.start = at + 8;
        range.end = at + 8 + length;
        if (!rom.Contains(range.start, length))
        {
            error = "track " + std::to_string(t) + " of the MIDI file at " + Hex(address) + " runs past the ROM's end";
            return false;
        }

        file.tracks.push_back(range);
        at = range.end;
    }

    return true;
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    SongRecord record;
    SongFile file;
    if (!ReadSongRecord(rom, info, song, record) || !ReadSongFile(rom, record.midi, file, error))
    {
        if (error.empty())
        {
            error = "song " + std::to_string(song) + " has no record";
        }
        return false;
    }

    char line[200];
    std::snprintf(line, sizeof line,
                  "song %d: %s, record at %08X, MIDI file at %08X, player %d, bank %d, volume %d, "
                  "priority %d\n",
                  song, record.name.c_str(), unsigned(record.address), unsigned(record.midi), record.player,
                  record.bank, record.volume, record.priority);
    std::string text = line;
    std::snprintf(line, sizeof line, "format %d, %zu tracks, %d ticks a quarter note\n", file.format,
                  file.tracks.size(), file.division);
    text += line;

    for (size_t t = 0; t < file.tracks.size(); t++)
    {
        text += "track " + std::to_string(t) + "\n";
        DumpTrack(rom, file.tracks[t].start, file.tracks[t].end, text);
    }

    return WriteTextFile(path, text, error);
}

} // namespace ntdlib::rd1

// SPDX-License-Identifier: MIT

// JAudio2's start-up archive (BAA): a list of commands that each name a block of the archive. The archive holds the
// sound table and its names, the sequence collection, the wave systems (WSYS) and the instrument banks (IBNK). The
// layouts are the ones that JAudio2 reads in Four Swords Adventures (2004), the earliest game known to use it: banks of
// both versions, instruments with effects and velocity regions, and wave systems that name their AW files. The sound
// table's entries can also have a later layout (Twilight Princess, 2006; see LayoutOf). A game keeps its sequence
// archive, an RARC of BMS files, beside it (rarc.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/wave_system.h"

namespace ntdlib::jaudio
{

// A sound of the sound table and the sequence it plays.
struct SoundEntry
{
    enum class Source : uint8_t
    {
        kCollection, // a sequence of the archive's sequence collection
        kArchive,    // a file of the sequence archive
    };

    // The ID that the game passes to the sound library for the sound: the section, the group and the index.
    uint32_t Id() const
    {
        return uint32_t{section} << 24 | uint32_t{group} << 16 | index;
    }

    std::string name; // from the name table, or empty
    uint8_t section = 0, group = 0;
    uint16_t index = 0;
    uint8_t type = 0; // the table's type byte. It gives the entry's layout (see LayoutOf)
    uint8_t volume = 127;

    // The names of the sound's section and group in the name table, or empty.
    std::string section_name, group_name;

    Source source = Source::kCollection;
    uint32_t resource = 0; // the collection's sequence, or the archive's file id

    // Where the sound's entry starts in the archive. LayoutOf gives where the entry holds the volume and the file.
    uint32_t offset = 0;
};

// Where a sound's entry holds its volume and, for a sound that plays a file of the sequence archive, the file's 16-bit
// number. The sound's type gives the layout. A type with bit 0x40 set has the later layout (Twilight Princess, 2006):
// the volume in the second byte and the number at +2. Types 0x60 to 0x6f play the file with that number. Types 0x70 to
// 0x7f play streams: audio files on the disc. Otherwise the entry has the earlier layout (Four Swords Adventures,
// 2004): the volume in the third byte. Type 0x11 plays the file in the low half of the word at +8. The game's code
// distinguishes the layouts by the same bit.
struct ItemLayout
{
    uint32_t volume = 2;          // the volume's offset in the entry
    std::optional<uint32_t> file; // the offset of the file's number, for a type that plays a file
    bool stream = false;          // whether the type plays a stream
};

ItemLayout LayoutOf(uint8_t type);

// How the readers below treat a damaged archive, and the words of their messages. SoundArchive::Parse reads by the
// defaults. `BaaOptions` can select stricter rules and other messages.
struct BaaOptions
{
    // The messages for data that isn't a start-up archive, and for a command that isn't known.
    std::string not_an_archive = "not a JAudio2 start-up archive (BAA)";
    std::string unknown_command = "archive has a chunk of unknown kind";

    // The message for a read past the end of the data. Without one, the reader's message gives the offset ("read out of
    // bounds at offset N").
    std::string past_end = {};

    // With check_magic, the message for a sound table or a name table without its magic word ("BST ", "BSTN").
    std::string table_damaged = {};

    // The messages that reject an archive without a sound table, and a sound table without sounds. Without them, the
    // readers return no sounds.
    std::string no_table = {};
    std::string no_sounds = {};

    // WaveArchiveNames' message for a wave system without its magic words ("WSYS", "WINF").
    std::string wave_system_damaged = "wave system is damaged";

    // Data shorter than this isn't a start-up archive, whatever it starts with.
    std::size_t min_size = 8;

    // The sound table and its names are read from their blocks' starts to the end of the archive, and a block at offset
    // 0 counts as none. WaveArchiveNames reads each wave system to the end of the archive too. Otherwise each block is
    // read inside its bounds, and a block that ends before it starts is an error.
    bool tables_reach_end = false;

    // The sound table's rules: check the tables' magic words; skip a section, group or sound whose offset is 0; read at
    // most 0x100 sections, 0x100 groups in a section and 0x10000 sounds in a group (and 0x100 groups of a wave system's
    // archives); read a name only where the name table's counts reach; reject a name whose offset is the end of the
    // data.
    bool check_magic = false;
    bool skip_empty = false;
    bool cap_counts = false;
    bool check_name_counts = false;
    bool names_inside = false;

    // Give each sound its source and resource, and its section's and group's names.
    bool sources = true;
    bool section_names = false;
};

// The sounds of the archive's sound table, in its order. Reads the command list and the sound table, and none of the
// wave systems, the banks or the sequence collection. Throws FormatError.
std::vector<SoundEntry> ReadSounds(std::span<const uint8_t> baa, const BaaOptions& options = {});

// The file names of the wave archives (AW) that the archive's wave systems name (WINF), each once, in the order that
// the wave systems give them. Reads nothing else of the wave systems. Throws FormatError.
std::vector<std::string> WaveArchiveNames(std::span<const uint8_t> baa, const BaaOptions& options = {});

// The archive's wave systems ("ws  "), in the order of their commands, each with the number that its command gives.
// Reads nothing else of the archive. Throws FormatError.
std::vector<WaveSystem> ReadWaveSystems(std::span<const uint8_t> baa, const BaaOptions& options = {});

// The start-up archive (BAA): the sound table and its names, the sequence collection, the wave systems and the banks.
class SoundArchive
{
public:
    // Reads a BAA file by BaaOptions' defaults. Throws FormatError if it isn't one, or is damaged.
    static SoundArchive Parse(std::vector<uint8_t> baa);

    // The sound table's sounds, in its order.
    const std::vector<SoundEntry>& Sounds() const
    {
        return sounds_;
    }

    // The bank with number `number`, or nullptr.
    const Bank* FindBank(uint32_t number) const;

    // The wave system with number `number`, or nullptr.
    const WaveSystem* FindWaveSystem(uint32_t number) const;

    // The sequence collection's data, and the offset of a sequence in it. Sequences address the data from its start.
    std::span<const uint8_t> Collection() const;
    std::optional<uint32_t> CollectionSequence(uint32_t category, uint32_t index) const;

    // Problems found while reading that leave the rest readable, such as a bank that can't be read.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    std::vector<uint8_t> data_;
    std::vector<SoundEntry> sounds_;
    std::vector<WaveSystem> wave_systems_;
    std::vector<Bank> banks_;
    std::size_t collection_offset_ = 0;
    std::size_t collection_size_ = 0;
    std::vector<std::vector<uint32_t>> collection_; // each category's sequence offsets
    std::vector<std::string> warnings_;
};

} // namespace ntdlib::jaudio

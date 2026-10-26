// SPDX-License-Identifier: MIT

// A bank of the Wii (RBNK) as its tree, its regions and its waves, in parts that a writer can put back together. The
// writer lays the bank out as NintendoWare for Revolution's tools do.
//
// The bank's header takes 32 bytes. It lists DATA and, in a bank of version 1.1, WAVE. DATA holds the number of
// programs, then a reference for each program's instrument. Each reference takes 8 bytes: 1 and its data type, two
// zeros, and its offset from the number of programs. A reference to nothing takes 8 zeros. The tree's nodes follow the
// references in pre-order: each node where a reference first names it, then the nodes that its entries name. A region
// takes 48 bytes: its values, then zeros. A range table holds its number of entries and each entry's highest key, then
// zeros to a multiple of 4 bytes, then a reference for each entry. An index table holds its lowest and its highest key
// and two zeros, then a reference for each key. Several references can name one node. A reference of data type 4 names
// a node of any kind, and NW4R SND plays nothing for it. A node that only such references name, and no reference of its
// kind, takes 4 zeros. Zeros pad DATA to 32 bytes.
//
// A bank of version 1.1 describes its waves in WAVE (see wave_file.h). WAVE holds the number of waves, then a reference
// to each wave's info from the number, then the infos one after another. Zeros pad WAVE to 32 bytes. The bank's wave
// data holds the raw samples. A later bank has no WAVE block, and its wave data is an RWAR of RWAV files.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"

namespace ntdlib::rsar
{

struct BankFile
{
    // The data types of the tree's references.
    static constexpr uint8_t kNone = 0;
    static constexpr uint8_t kRegion = 1;
    static constexpr uint8_t kRangeTable = 2;
    static constexpr uint8_t kIndexTable = 3;
    static constexpr uint8_t kOtherNode = 4; // names a node of any kind, which NW4R SND doesn't play

    // A reference of the tree: its data type, and the node that it names, an index into `nodes`.
    struct Reference
    {
        friend bool operator==(const Reference&, const Reference&) = default;

        uint8_t type = kNone;
        uint32_t node = 0;
    };

    // A node of the tree: a region, a range table or an index table, by its kind. A node of kind kNone is the 4 zeros
    // that only references of data type 4 name.
    struct Node
    {
        friend bool operator==(const Node&, const Node&) = default;

        uint8_t kind = kRegion;
        Region region;             // a region's values
        std::vector<uint8_t> keys; // a range table's highest key of each entry, or an index table's lowest and highest
        std::vector<Reference> entries; // a table's references
    };

    // A run of velocities of a key region, and the region that plays it.
    struct VelocityRegion
    {
        friend bool operator==(const VelocityRegion&, const VelocityRegion&) = default;

        uint8_t first_velocity = 0;
        uint8_t last_velocity = 127;
        Region values;
    };

    // A run of keys of an instrument, and its velocity regions in the order of their velocities.
    struct KeyRegion
    {
        friend bool operator==(const KeyRegion&, const KeyRegion&) = default;

        uint8_t first_key = 0;
        uint8_t last_key = 127;
        std::vector<VelocityRegion> regions;
    };

    // An instrument: its key regions in the order of their keys.
    struct Instrument
    {
        friend bool operator==(const Instrument&, const Instrument&) = default;

        std::vector<KeyRegion> key_regions;
    };

    // The bytes of a bank file and of its wave data.
    struct Files
    {
        friend bool operator==(const Files&, const Files&) = default;

        std::vector<uint8_t> file;
        std::vector<uint8_t> wave_data;
    };

    // Reads the bank in `file` and its waves in `wave_data`. The nodes follow the order of the layout. Throws
    // xsflib::FormatError for bytes that aren't a bank of version 1.1 or 1.2, wave data that doesn't hold the samples
    // that a bank of version 1.1 describes or isn't an RWAR for a later bank, and a bank that the tools didn't lay out:
    // Write has to give `file` and `wave_data` back.
    static BankFile Read(std::span<const uint8_t> file, std::span<const uint8_t> wave_data);

    // The bank's file and its wave data, laid out as the tools lay a bank out. Read gives the bank back. Throws
    // std::invalid_argument for a version other than 1.1 and 1.2, a reference to a node that `nodes` lacks or of a data
    // type that doesn't match the node's kind, nodes out of the order of the layout, a node that no reference names, a
    // node of a kind that only references of data type 4 reach, a range table whose keys don't match its entries, an
    // index table whose keys run backwards or don't match its entries, a tree deeper than 256 nodes, and the exceptions
    // of WaveFile::Write.
    Files Write() const;

    // The instruments as NW4R SND plays them, by program (Bank::Find): each run of keys and velocities that a region
    // plays. A program without a region at any key and velocity has no instrument. A region with a negative wave index
    // counts as a region, as the tools wrote it.
    std::vector<std::optional<Instrument>> Instruments() const;

    // A bank of `version` with `instruments` and `waves`, laid out as the tools lay out a bank whose instruments share
    // no node. An instrument of one key region over every key, with one velocity region over every velocity, takes a
    // region. Any other instrument takes a key table: a range table for up to 11 entries, an index table for more. A
    // range table holds an entry for each gap before or between the key regions, one for each key region, and one for
    // the gap after the last. Its last entry ends at key 127. An index table runs from the first key region's first key
    // to the last key region's last. Each key region takes a region, or a range table of its velocities by the same
    // rules. Throws std::invalid_argument for runs that overlap, don't follow the order of their keys or velocities or
    // end before they start, and a run past 127.
    static BankFile FromInstruments(uint16_t version, std::span<const std::optional<Instrument>> instruments,
                                    std::vector<WaveFile> waves);

    friend bool operator==(const BankFile&, const BankFile&) = default;

    uint16_t version = 0x0102;
    std::vector<Reference> instruments; // by program
    std::vector<Node> nodes;            // in the order of the layout
    std::vector<WaveFile> waves;
};

} // namespace ntdlib::rsar

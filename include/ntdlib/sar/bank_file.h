// SPDX-License-Identifier: MIT

// A bank file of the 3DS (CBNK), the Wii U or the Switch (FBNK) as its values: the table of waves, and each program's
// instrument with its key regions and velocity regions. BankFile::Read takes them from a bank that NintendoWare's tools
// laid out, and Write lays a bank out from them the same way (see bank_edit.h for the layout).
//
// An instrument's key regions cover runs of keys, and a key region's velocity regions cover runs of velocities. A
// velocity region plays a wave of the bank's table of waves with its values. The file holds each instrument's key
// regions in a table, and each key region's velocity regions in a table too. The tools choose each table's kind by its
// runs: the runs of keys or velocities with a region, and the gaps before and between them. One region over every key
// or velocity takes a direct table. Up to 11 runs take a range table, with an entry for each run and one for the gap
// after the last region. More runs take an index table, with an entry for each key or velocity from 0 to the last
// region's last. A table names each region at its run's entries. The tools write all six values of each region.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

struct BankFile
{
    // A velocity region: the velocities that it covers, and the values that it plays with. Its wave is an index into
    // the table of waves.
    struct Region
    {
        friend bool operator==(const Region&, const Region&) = default;

        uint8_t first_velocity = 0;
        uint8_t last_velocity = 127;
        VelocityRegion values;
    };

    // A key region: the keys that it covers, and its velocity regions in the order of their velocities.
    struct KeyRegion
    {
        friend bool operator==(const KeyRegion&, const KeyRegion&) = default;

        uint8_t first_key = 0;
        uint8_t last_key = 127;
        std::vector<Region> regions;
    };

    // An instrument: its key regions in the order of their keys.
    struct Instrument
    {
        friend bool operator==(const Instrument&, const Instrument&) = default;

        std::vector<KeyRegion> key_regions;
    };

    // Reads `file`. `order` stays empty when the instruments lie in the order of their programs. Throws
    // xsflib::FormatError for a file that isn't a bank file, and for a bank that NintendoWare's tools didn't lay out:
    // Write has to give `file` back.
    static BankFile Read(std::span<const uint8_t> file);

    // The file's bytes, laid out as the tools lay a bank out. An instrument without key regions, and a key region
    // without velocity regions, take a range table with one entry that names nothing. Throws std::invalid_argument for
    // regions whose runs overlap, don't follow the order of their keys or velocities or end before they start, for a
    // run past 127, and for an order that doesn't name each program with an instrument once.
    std::vector<uint8_t> Write() const;

    friend bool operator==(const BankFile&, const BankFile&) = default;

    Flavor flavor = Flavor::kNx;
    uint32_t version = 0x00010000;
    std::vector<WaveId> waves;
    std::vector<std::optional<Instrument>> instruments; // by program

    // The programs of the instruments in the order that the file lays them out in. Empty: in the order of the programs.
    std::vector<uint32_t> order;
};

} // namespace ntdlib::sar

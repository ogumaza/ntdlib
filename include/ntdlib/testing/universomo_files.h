// SPDX-License-Identifier: MIT

// Files of Universomo's driver, built by hand for the tests of the conversion and of the DS frame's search of an image.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::testing::universomo_files
{

using Bytes = std::vector<uint8_t>;

// A song file of one pattern of one row, with one track and one instrument that plays the bank's sample 0.
inline Bytes SongFile()
{
    const std::string magic = "UNIMUSICSQ:";
    Bytes out(magic.begin(), magic.end());
    for (const uint32_t value : {150u, 4u, 1u, 1u, 1u, 1u, 0u}) // tempo, speed, orders, tracks, patterns, instruments,
    {                                                           // restart
        xsflib::AppendLe32(out, value);
    }
    out.insert(out.end(), {0, 0}); // the instrument's sample, the order list
    xsflib::AppendLe32(out, 1);    // the pattern's rows
    out.push_back(0);              // the track's first rows
    xsflib::AppendLe32(out, 1);    // one event: a note
    out.insert(out.end(), {0x50, 48, 1, 0});
    out.insert(out.end(), {64, 128, 0, 0}); // the instrument
    return out;
}

// A bank of one 8-bit sample of 4 bytes that plays once.
inline Bytes BankFile()
{
    const std::string magic = "UNISAMBA";
    Bytes out(magic.begin(), magic.end());
    out.push_back(0x3A);
    xsflib::AppendLe32(out, 1);
    xsflib::AppendLe32(out, 0);
    xsflib::AppendLe32(out, 4);
    out.push_back(8);
    xsflib::AppendLe32(out, 0);
    xsflib::AppendLe32(out, 0);
    out.push_back(0);
    out.insert(out.end(), {1, 2, 3, 4});
    return out;
}

// A table of sounds of 106 entries: song 0 plays once and song 105 loops.
inline Bytes TableFile()
{
    Bytes out = {'A', 'U', '0', 106};
    for (int i = 0; i < 106; i++)
    {
        out.insert(out.end(),
                   {0, static_cast<uint8_t>(i == 105 ? 1 : 0), static_cast<uint8_t>(i == 105 ? 1 : 0), 0, 1, 0, 0});
    }
    return out;
}

} // namespace ntdlib::testing::universomo_files

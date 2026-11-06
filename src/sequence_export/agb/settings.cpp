// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/agb/settings.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include "ntdlib/agb/files.h"

namespace ntdlib::sequence_export::agb
{
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::Utf8;

std::string SongNumber(int song, int count)
{
    int digits = 2;
    for (int n = count - 1; n >= 100; n /= 10)
    {
        digits++;
    }

    // Padded by hand: GCC can't tell the field width from `digits` and warns about snprintf.
    const std::string number = std::to_string(song);
    return std::string(size_t(std::max(0, digits - int(number.size()))), '0') + number;
}

std::string SongStem(const ConvertSettings& settings, const std::string& number)
{
    return Utf8(PathFromUtf8(settings.out_dir) / PathFromUtf8(SafeFileName(settings.base_name) + "_" + number));
}

} // namespace ntdlib::sequence_export::agb

// SPDX-License-Identifier: MIT

// The start and the name of a bank's SFZ file. Such a file plays one program of the bank by itself, without MIDI. Every
// bank model writes them the same way. The header requires only seqlib. A library of bank models that doesn't link
// sequence_export can therefore use it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "seqlib/file_name.h"
#include "seqlib/sfz/text.h"
#include "seqlib/sfz/track_file.h"

namespace ntdlib::sequence_export
{

// The start of a bank's SFZ file. The first comment names the preset `name`, the bank `title` and `program`, the
// program that made the file. The frame of seqlib's track files follows: the samples' folder, CC10's pan curve, a bend
// range of `bend_range` semitones, and CC7, CC10 and CC11 without smoothing.
inline std::string SfzBankFileStart(const std::string& name, const std::string& title, const std::string& program,
                                    int bend_range)
{
    return "// " + seqlib::sfz::CommentText(name) + " of " + seqlib::sfz::CommentText(title) + ", converted by " +
           program + ".\n\n" + seqlib::sfz::ControlHeader() + seqlib::sfz::GlobalHeader(bend_range);
}

// The number of a bank's SFZ file in its name. Zeros pad `number` to the digits of `last`, the highest number of the
// bank's files. A number with more digits isn't padded.
inline std::string SfzBankFileNumber(uint32_t number, uint32_t last)
{
    const std::string text = std::to_string(number);
    const std::size_t digits = std::to_string(last).size();

    return std::string(digits > text.size() ? digits - text.size() : 0, '0') + text;
}

// The name of a bank's SFZ file: "<title>_<number>.sfz", with the number padded as SfzBankFileNumber pads it.
inline std::string SfzBankFileName(const std::string& title, uint32_t number, uint32_t last)
{
    return seqlib::SafeFileName(title) + "_" + SfzBankFileNumber(number, last) + ".sfz";
}

} // namespace ntdlib::sequence_export

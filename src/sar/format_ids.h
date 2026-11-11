// SPDX-License-Identifier: MIT

// The values that NintendoWare's files of the 3DS, the Wii U and the Switch give their blocks, references and tables,
// and the versions and options of an archive's entries that more than one source of the sar module reads or writes.
// The sources name them with using-declarations.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ntdlib::sar::detail
{

// An ID, a string index or an offset that refers to nothing, and an offset that refers to nothing read as a signed
// value.
inline constexpr uint32_t kNone = 0xFFFFFFFF;
inline constexpr int32_t kNoOffset = -1;

// The blocks of an archive, and those of a group's file.
inline constexpr uint16_t kStringBlock = 0x2000;
inline constexpr uint16_t kInfoBlock = 0x2001;
inline constexpr uint16_t kFileBlock = 0x2002;
inline constexpr uint16_t kGroupInfoBlock = 0x7800;
inline constexpr uint16_t kGroupFileBlock = 0x7801;

// The INFO blocks of a bank file and of a wave sound file.
inline constexpr uint16_t kBankInfoBlock = 0x5800;
inline constexpr uint16_t kWaveSoundInfoBlock = 0x6800;

// The references that every kind of file holds: to a list of values, to a table of references, to data in a block
// (such as a file in FILE or a channel's samples in DATA), and to a channel's DSP-ADPCM info.
inline constexpr uint16_t kListReference = 0x0100;
inline constexpr uint16_t kTableReference = 0x0101;
inline constexpr uint16_t kDataReference = 0x1F00;
inline constexpr uint16_t kDspAdpcmReference = 0x0300;

// The references of an archive's INFO block to its tables, in the order of the tables' places in INFO.
inline constexpr uint16_t kSoundTable = 0x2100;
inline constexpr uint16_t kSoundGroupTable = 0x2104;
inline constexpr uint16_t kBankTable = 0x2101;
inline constexpr uint16_t kWaveArchiveTable = 0x2103;
inline constexpr uint16_t kGroupTable = 0x2105;
inline constexpr uint16_t kPlayerTable = 0x2102;
inline constexpr uint16_t kFileTable = 0x2106;

// The references from a sound's entry to its details, and the parts of a stream sound's details.
inline constexpr uint16_t kStreamDetails = 0x2201;
inline constexpr uint16_t kWaveSoundDetails = 0x2202;
inline constexpr uint16_t kSequenceDetails = 0x2203;
inline constexpr uint16_t kStreamTrack = 0x220E;
inline constexpr uint16_t kSends = 0x220F;

// The references from a file's entry to its place in the FILE block and to its path.
inline constexpr uint16_t kInternalFile = 0x220C;
inline constexpr uint16_t kExternalFile = 0x220D;

// The size of a sound's 3D settings.
inline constexpr std::size_t kSettings3dSize = 16;

// The options of a sound's entry that the tools write: the string index, the pan, the priorities, bit 3, the 3D
// settings, bit 17 and the user's bits 28 to 31. Only the offset of the 3D settings leads to other bytes.
inline constexpr uint32_t kKnownSoundOptions = 0xF002010F;

// The options of a sequence sound's details that the tools write: the start and the channel priority.
inline constexpr uint32_t kKnownSequenceOptions = 3;

// The references of a bank file: from the table of instruments to an instrument, from an instrument's table to a key
// region, from a key region's table to a velocity region, to no instrument or no region, and to a region's envelope.
inline constexpr uint16_t kInstrumentReference = 0x5900;
inline constexpr uint16_t kKeyRegionReference = 0x5901;
inline constexpr uint16_t kVelocityRegionReference = 0x5902;
inline constexpr uint16_t kNoItemReference = 0x5903;
inline constexpr uint16_t kEnvelopeReference = 0x0000;

// The kinds of a bank file's tables of regions: one region for every value, a region for each run of values up to a
// limit, and a region for each value from a first to a last.
inline constexpr uint16_t kDirectTable = 0x6000;
inline constexpr uint16_t kRangeTable = 0x6001;
inline constexpr uint16_t kIndexTable = 0x6002;

// The options of a velocity region: the original key, the volume, the pan, the pitch, the flags and the envelope. Each
// set bit has a value of 4 bytes, in the order of the bits. The flags hold whether the region ignores a note-off, its
// key group and its interpolation. The envelope's value is the offset of a reference to the envelope's 5 bytes.
inline constexpr uint32_t kRegionOptions = 0x21F;
inline constexpr int kEnvelopeOption = 9;
inline constexpr std::size_t kEnvelopeBytes = 5;

// The boundary that each structure of a bank file's INFO block starts on.
inline constexpr std::size_t kBankAlignment = 4;

} // namespace ntdlib::sar::detail

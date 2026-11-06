// SPDX-License-Identifier: MIT

// JAudio1's start-up archive, the AAF: a list of chunks. Each chunk is a type word, then three words for a block of the
// archive: its offset, its size and a value. A chunk of the bank list (type 2) or the wave system list (type 3) lists
// blocks until a 0 word. A bank's value is the wave system that it plays. A wave system's value is its load type. The
// readers here take four kinds of block. The sound table (type 1) lists the sounds by category. Each bank (IBNK) holds
// instruments. Each wave system (WSYS) names the wave archives (AW) that hold its samples. A game keeps its sequences
// beside the archive. In an earlier JAudio1, such as Luigi's Mansion's, they're one file that isn't a RARC. The
// sequence index (type 4) divides it.
//
// The Switch's release of Pikmin 2 stores its archive little-endian. Each field of 16 or 32 bits is little-endian. The
// archive's first word then holds its type in its first byte. Each entry of the two lists widens its offset to 64
// bits. An entry then takes 16 bytes, with its size at +8 and its value at +12. A block of type 5 takes 16 bytes in
// the same way. The other blocks keep 12 bytes (the archive's parser, 0x3c5430 in the release's main). The readers
// here take the byte order from the first word.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"

namespace ntdlib::jaudio
{

// A JAudio1 sound ID gives the sound's type in its top two bits. An effect's ID gives its category in bits 12 to 19.
// The low 10 bits of any ID give the sound's index in its category. Bits 10 and 11 of an effect's ID set how long the
// sound library plays it. Without either, the library stops the effect a few frames after it starts. A game keeps such
// an effect playing by starting it again. With bit 11, the effect plays to its end.
constexpr uint32_t kAafTypeMask = 0xc0000000;
constexpr uint32_t kAafEffect = 0x00000000;
constexpr uint32_t kAafSequence = 0x80000000;
constexpr uint32_t kAafStream = 0xc0000000;
constexpr uint32_t kAafToEnd = 0x800;
constexpr uint32_t kAafIndexMask = 0x3ff;

// The category of an effect, by its ID.
constexpr uint32_t AafCategory(uint32_t id)
{
    return id >> 12 & 0xff;
}

// Where an entry of the AAF's sound table gives its volume, and a sequence's file.
inline constexpr ItemLayout kAafItemLayout = {.volume = 0xc, .file = 6};

// Whether `data` starts as an AAF does: with the type of a chunk that gives a block, 1 to 8, in either byte order. A
// BAA starts with "AA_<" or "<_AA" instead.
bool IsAaf(std::span<const uint8_t> data);

// The sounds of an AAF's sound table, in the table's order: the effects of each category, then the sequences. Streams
// are left out. So is the first sequence, the effects sequence. The sound library starts it at start-up. The effects
// play in its tracks.
//
// A sound's ID is the one that the game passes to the sound library. An effect's ID sets bit 11 (kAafToEnd). A sound's
// type is its ID's top byte. The sound's volume is its entry's volume byte, over 127. The sound's offset is the entry's
// offset. A sequence plays a file of the sequence archive: its source is kArchive and its resource is the file's
// number. That number is the sequence's offset number. The table's header gives each kind of sound a format byte. With
// bit 0 of the sequences' format, an entry gives the offset number (kAafItemLayout's file). Without it, the offset
// number is the sequence's index. The Wind Waker's sequences set the bit, and Luigi's Mansion's don't. An effect plays
// in the effects sequence, and keeps the default source. Its resource is its offset number, by the effects' format in
// the same way (JAIBasic::getSoundOffsetNumberFromID). The effects sequence's slot reads it to pick the effect. With
// bit 1 of the effects' format, an effect's pitch is the float at +8 of its entry. The Wind Waker's effects have that
// bit. The table names no sound. Every name is empty.
//
// Throws FormatError if the archive or its sound table is damaged, or if the table lists no sounds.
std::vector<SoundEntry> ReadAafSounds(std::span<const uint8_t> aaf);

// The file of the sequence archive that the effects sequence plays: its offset number, as for ReadAafSounds. Throws
// FormatError if the archive or its sound table is damaged, or if the table lists no sequences.
uint16_t AafEffectsSequenceFile(std::span<const uint8_t> aaf);

// The effects sequence's count of slots for each category of effects in sound scene 0, from the AAF's table of scenes
// (type 6), or nothing if the AAF has no such table. The table starts with its count of scenes, then gives each scene's
// offset in the table. A little-endian archive widens those offsets to 64 bits. A scene's row gives each category two
// bytes: its count of slots, then a limit on the effects of one object. The readers here skip the limit. The sound
// library copies the table at start-up (JAInter::InitData::checkInitDataOnMemory). It plays scene 0 unless the game
// changes it. Without the table, each category has four slots (JAInter::Const::sCInfos_0). Throws FormatError if the
// archive or its table of scenes is damaged.
std::optional<std::vector<uint8_t>> AafEffectSlotCounts(std::span<const uint8_t> aaf);

// The sequences of a sequence archive that the AAF's sequence index divides, as ReadRarc gives a RARC's files: each
// sequence's number in the index as its id, its name, its data in `archive`, and the number of the sequence whose data
// the game reads (ArchiveFile::data_id). The index (a BARC block, barc.h) has a 32-byte header, then a 32-byte entry
// for each sequence: the name in up to 14 bytes, the number of the sequence whose data it shares (0xffff for none), two
// words that the reader skips, then the sequence's offset and size in the archive. The names are cut to 13 characters,
// with or without their extension. Returns nothing if the AAF has no sequence index. Throws FormatError if the archive
// or its index is damaged, if a sequence lies outside `archive`, or if a sequence's chain of shared data loops or names
// a sequence past the index's end.
std::optional<std::vector<ArchiveFile>> ReadAafSequenceFiles(std::span<const uint8_t> aaf,
                                                             std::span<const uint8_t> archive);

// The AAF's wave systems, in the order of its list. A system's number is its place in the list. In The Wind Waker, bank
// n plays wave system n. A little-endian archive's systems have wide records of their archives (kWideWaveCountAt).
// Throws FormatError if the archive or a wave system is damaged.
std::vector<WaveSystem> ReadAafWaveSystems(std::span<const uint8_t> aaf);

// The AAF's banks, in the order of its list. Each bank plays the wave system that its block's value gives. The earlier
// JAudio1's archive, the one with a sequence index, works another way. That's Luigi's Mansion's. Each block's value is
// 0 there. The high half of a wave's id names the wave's system by the id in the system's header. A bank can then play
// the waves of more than one system (Bank::named_systems). An archive whose systems share an id keeps to the block's
// values: Super Mario Sunshine's has a sequence index, and its systems all give the id 0. A later JAudio1 keeps to the
// block's value too. In Pikmin 2's
// Wii and Switch releases, one bank's waves name system 0 while its value gives system 1. A sequence selects a
// bank by the number in the bank's header (JASBankMgr::registBankBNK maps that number to the bank's place in the list).
// Bank::number holds that number. A bank that can't be read is left out, and `warnings` gets a message for it. Throws
// FormatError if the archive is damaged.
std::vector<Bank> ReadAafBanks(std::span<const uint8_t> aaf, std::vector<std::string>& warnings);

// The file names of the wave archives that the AAF's wave systems name, each once, in the order that the systems give
// them. Throws FormatError if the archive or a wave system is damaged.
std::vector<std::string> AafWaveArchiveNames(std::span<const uint8_t> aaf);

// The same for the wave systems that the sound library loads at start-up: load types 0 and 1. A scene loads the others
// (load type 2).
std::vector<std::string> AafStayingWaveArchiveNames(std::span<const uint8_t> aaf);

// A sound that a JAudio1 name file (.asn) names, as The Wind Waker HD has one: its sound ID and its name.
struct AsnSound
{
    uint32_t id = 0;
    std::string name;
};

// The sounds of a name file, in its order. The file is a 16-byte header, then 32-byte records. The header's last word
// gives the number of sounds. The first 18 records name the sound table's categories: 16 of effects, then one of
// sequences and one of streams. A record for each sound follows: a name of up to 28 bytes, padded with zeros, then the
// sound's ID. With `big_endian` false, each ID is little-endian. The Switch's release of Pikmin 2 stores its IDs that
// way. Its number of sounds stays big-endian. The release's code never reads the file. Throws FormatError if the file
// is too short for its sounds.
std::vector<AsnSound> ReadAsnSounds(std::span<const uint8_t> asn, bool big_endian = true);

} // namespace ntdlib::jaudio

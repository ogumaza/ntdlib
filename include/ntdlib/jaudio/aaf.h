// SPDX-License-Identifier: MIT

// JAudio1's start-up archive, the AAF: a list of chunks. Each chunk is a type word, then three words for a block of the
// archive: its offset, its size and a value. A chunk of the bank list (type 2) or the wave system list (type 3) lists
// blocks until a 0 word. A bank's value is the wave system that it plays. A wave system's value is its load type. The
// readers here take two kinds of block. The sound table (type 1) lists the sounds by category. Each wave system (WSYS)
// names the wave archives (AW) that hold its samples. A game keeps its sequences beside the archive.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"
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

// The sounds of an AAF's sound table, in the table's order: the effects of each category, then the sequences. Streams
// are left out. So is the first sequence, the effects sequence. The sound library starts it at start-up. The effects
// play in its tracks.
//
// A sound's ID is the one that the game passes to the sound library. An effect's ID sets bit 11 (kAafToEnd). A sound's
// type is its ID's top byte. The sound's volume is its entry's volume byte, over 127. The sound's offset is the entry's
// offset. A sequence plays a file of the sequence archive: its source is kArchive and its resource is the file's
// number. An effect plays in the effects sequence, and keeps the default source and resource. The table names no sound.
// Every name is empty.
//
// Throws FormatError if the archive or its sound table is damaged, or if the table lists no sounds.
std::vector<SoundEntry> ReadAafSounds(std::span<const uint8_t> aaf);

// The file of the sequence archive that the effects sequence plays. Throws FormatError if the archive or its sound
// table is damaged, or if the table lists no sequences.
uint16_t AafEffectsSequenceFile(std::span<const uint8_t> aaf);

// The AAF's wave systems, in the order of its list. A system's number is its place in the list. In The Wind Waker, bank
// n plays wave system n. Throws FormatError if the archive or a wave system is damaged.
std::vector<WaveSystem> ReadAafWaveSystems(std::span<const uint8_t> aaf);

// The file names of the wave archives that the AAF's wave systems name, each once, in the order that the systems give
// them. Throws FormatError if the archive or a wave system is damaged.
std::vector<std::string> AafWaveArchiveNames(std::span<const uint8_t> aaf);

// The same for the wave systems that the sound library loads at start-up: load types 0 and 1. A scene loads the others
// (load type 2).
std::vector<std::string> AafStayingWaveArchiveNames(std::span<const uint8_t> aaf);

} // namespace ntdlib::jaudio

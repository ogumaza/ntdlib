// SPDX-License-Identifier: MIT

#include "ntdlib/spd/song.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "common/magic.h"

namespace ntdlib::spd
{

using ntdlib::detail::HasMagic;

namespace
{

constexpr char kMagic[] = "SDFDAT";
constexpr std::size_t kMagicSize = 8;
constexpr std::size_t kSlotTable = 0x14;
constexpr std::size_t kInstrumentSize = 12;
constexpr std::size_t kSampleHeaderSize = 12;
constexpr std::size_t kPlayedStart = 8;
constexpr uint8_t kNone = 0xFF;

void Require(std::span<const uint8_t> bytes, std::size_t offset, std::size_t size, const char* what)
{
    if (offset > bytes.size() || size > bytes.size() - offset)
    {
        throw xsflib::FormatError(std::string(what) + " runs past the block's end");
    }
}

uint32_t Word(std::span<const uint8_t> bytes, std::size_t offset, const char* what)
{
    Require(bytes, offset, 4, what);
    return xsflib::LoadLe32(bytes.data() + offset);
}

} // namespace

InstrumentKind Instrument::Kind() const
{
    return static_cast<InstrumentKind>(settings & 7);
}

int Instrument::Kit() const
{
    return static_cast<int>((settings >> 3) & 0x7F);
}

int Instrument::Duty() const
{
    return static_cast<int>((settings >> 3) & 7);
}

std::optional<int> Instrument::Modulation() const
{
    // The driver reads bits 10-16 as a signed number and takes 0 to 15.
    const int value = static_cast<int>(static_cast<int32_t>(settings << 15) >> 25);
    if (value < 0 || value >= kModulations)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<int> Instrument::FixedKey() const
{
    const int value = static_cast<int8_t>(static_cast<uint8_t>(settings >> 17));
    if (value < 0)
    {
        return std::nullopt;
    }
    return value;
}

int Instrument::KitPan() const
{
    return static_cast<int>(settings >> 25);
}

Envelope Instrument::EnvelopeOf() const
{
    Envelope result;
    result.bend_range = static_cast<int>(envelope & 0x1F);
    result.attack = static_cast<int>((envelope >> 5) & 0x7F);
    result.decay = static_cast<int>((envelope >> 12) & 0x7F);
    result.sustain = static_cast<int>((envelope >> 19) & 0x3F) + 1;
    result.release = static_cast<int>(envelope >> 25);
    return result;
}

SampleFormat Sample::Format() const
{
    return static_cast<SampleFormat>((control >> 5) & 3);
}

bool Sample::Loops() const
{
    return ((control >> 3) & 3) == 1;
}

double Sample::Rate() const
{
    // The ARM7's bus clock over 2 drives the channels' timers. At the root key the driver scales the timer by
    // 65,535 / 65,536 and keeps the register's value on a multiple of 4 (0x037FA1F8).
    constexpr double kTimerClock = 33513982.0 / 2.0;
    const int period = static_cast<int>((static_cast<int64_t>(timer) * 0xFFFF) >> 16);
    const int played = 0x10000 - ((0x10000 - period) & 0xFFFC);
    return played == 0 ? 0.0 : kTimerClock / played;
}

Block::Block(std::span<const uint8_t> bytes) : bytes_(bytes)
{
    Require(bytes_, 0, kBlockHeaderEnd, "the block's header");
    program_table_ = xsflib::LoadLe32(bytes_.data() + 4);
    kit_table_ = xsflib::LoadLe32(bytes_.data() + 8);
    sound_table_ = xsflib::LoadLe32(bytes_.data() + 12);
    const uint32_t sounds = xsflib::LoadLe32(bytes_.data() + 16);
    if (program_table_ > kit_table_ || kit_table_ > sound_table_)
    {
        throw xsflib::FormatError("the block's tables aren't in the order programs, kits, sounds");
    }
    // The tables follow one another. Each one ends where the next starts.
    program_count_ = static_cast<int>((kit_table_ - program_table_) / 4);
    // The kit table ends where the first list of instruments starts.
    uint32_t first_list = sound_table_;
    for (int program = 0; program < program_count_; ++program)
    {
        const uint32_t list = Word(bytes_, program_table_ + static_cast<std::size_t>(program) * 4, "the program table");
        if (list >= kit_table_ && list < first_list)
        {
            first_list = list;
        }
    }
    kit_count_ = std::min(static_cast<int>((first_list - kit_table_) / 4), kKitMaps);
    Require(bytes_, sound_table_, static_cast<std::size_t>(sounds) * 8, "the sound table");
    sound_count_ = static_cast<int>(sounds);
}

std::optional<Sound> Block::SoundAt(int index) const
{
    if (index < 0 || index >= sound_count_)
    {
        return std::nullopt;
    }
    const std::size_t entry = sound_table_ + static_cast<std::size_t>(index) * 8;
    Sound sound;
    sound.offset = xsflib::LoadLe32(bytes_.data() + entry);
    sound.flags = xsflib::LoadLe32(bytes_.data() + entry + 4);
    if (sound.offset == 0)
    {
        return std::nullopt;
    }
    const uint32_t tracks = Word(bytes_, sound.offset, "a sound's header");
    Require(bytes_, sound.offset + 4, static_cast<std::size_t>(tracks) * 4, "a sound's track table");
    for (uint32_t i = 0; i < tracks; ++i)
    {
        sound.tracks.push_back(xsflib::LoadLe32(bytes_.data() + sound.offset + 4 + i * 4));
    }
    return sound;
}

std::optional<Instrument> Block::ProgramInstrument(int program, int index) const
{
    if (program < 0 || program >= program_count_ || program >= kProgramMaps || index < 0 || index >= kProgramMapSize)
    {
        return std::nullopt;
    }
    const uint8_t entry = bytes_[kProgramMapsOffset + static_cast<std::size_t>(program) * kProgramMapSize + index];
    if (entry == kNone)
    {
        return std::nullopt;
    }
    const uint32_t list = Word(bytes_, program_table_ + static_cast<std::size_t>(program) * 4, "the program table");
    const std::size_t at = list + static_cast<std::size_t>(entry) * kInstrumentSize;
    Require(bytes_, at, kInstrumentSize, "an instrument");
    return Instrument{xsflib::LoadLe32(bytes_.data() + at), xsflib::LoadLe32(bytes_.data() + at + 4),
                      xsflib::LoadLe32(bytes_.data() + at + 8)};
}

std::optional<Instrument> Block::KitInstrument(int kit, int key) const
{
    const int place = key - kFirstKitKey;
    if (kit < 0 || kit >= kit_count_ || place < 0 || place >= kKitKeys)
    {
        return std::nullopt;
    }
    const uint8_t entry = bytes_[kKitMapsOffset + static_cast<std::size_t>(kit) * kKitKeys + place];
    if (entry == kNone)
    {
        return std::nullopt;
    }
    const uint32_t list = Word(bytes_, kit_table_ + static_cast<std::size_t>(kit) * 4, "the kit table");
    const std::size_t at = list + static_cast<std::size_t>(entry) * kInstrumentSize;
    Require(bytes_, at, kInstrumentSize, "an instrument");
    return Instrument{xsflib::LoadLe32(bytes_.data() + at), xsflib::LoadLe32(bytes_.data() + at + 4),
                      xsflib::LoadLe32(bytes_.data() + at + 8)};
}

uint32_t Block::ModulationWord(int index) const
{
    if (index < 0 || index >= kModulations)
    {
        return 0;
    }
    return xsflib::LoadLe32(bytes_.data() + kModulationsOffset + static_cast<std::size_t>(index) * 4);
}

Sample Block::SampleAt(uint32_t offset) const
{
    Require(bytes_, offset, kSampleHeaderSize, "a sample's header");
    const uint8_t* header = bytes_.data() + offset;
    Sample sample;
    sample.offset = offset;
    sample.control = header[1];
    sample.root_key = header[2];
    sample.timer = xsflib::LoadLe16(header + 4);
    sample.loop_start = xsflib::LoadLe16(header + 6);
    sample.loop_length = xsflib::LoadLe16(header + 8);
    // The driver gives the hardware the header's address plus 10. The game loads blocks on a multiple of 4 bytes, and
    // the hardware ignores the address's low 2 bits. The hardware therefore plays from the header's byte 8.
    const std::size_t size = (static_cast<std::size_t>(sample.loop_start) + sample.loop_length) * 4;
    Require(bytes_, offset + kPlayedStart, size, "a sample's data");
    sample.data = bytes_.subspan(offset + kPlayedStart, size);
    return sample;
}

uint16_t Block::EventAt(uint32_t offset) const
{
    Require(bytes_, offset, 2, "a track");
    return xsflib::LoadLe16(bytes_.data() + offset);
}

bool IsSoundData(std::span<const uint8_t> bytes)
{
    return bytes.size() >= kMagicSize && HasMagic(bytes, kMagic) && bytes[6] == 0 && bytes[7] == 0;
}

std::vector<BlockPlace> ReadBlocks(std::span<const uint8_t> bytes)
{
    if (!IsSoundData(bytes))
    {
        throw xsflib::FormatError("not a sound data file of Nintendo SPD's driver");
    }
    // The header's u16 at 8 gives the last slot.
    xsflib::Reader reader(bytes);
    const int slots = reader.U16(8) + 1;
    std::vector<BlockPlace> blocks;
    for (int slot = 0; slot < slots; ++slot)
    {
        const std::size_t entry = kSlotTable + static_cast<std::size_t>(slot) * 8;
        const uint32_t offset = reader.U32(entry);
        const uint32_t size = reader.U32(entry + 4);
        if (size == 0)
        {
            continue;
        }
        if (offset > bytes.size() || size > bytes.size() - offset)
        {
            throw xsflib::FormatError("a block runs past the sound data's end");
        }
        blocks.push_back({slot, offset, size});
    }
    return blocks;
}

int PlayerOf(uint32_t flags)
{
    return static_cast<int>(flags & 0xF);
}

int VolumeOf(uint32_t flags)
{
    return static_cast<int>((flags >> 4) & 0x7F);
}

} // namespace ntdlib::spd

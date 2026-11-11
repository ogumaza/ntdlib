// SPDX-License-Identifier: MIT

#include "ntdlib/n64/sbn.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

constexpr std::size_t kSbnHeaderSize = 0x40;
constexpr std::size_t kFileEntrySize = 8;
constexpr std::size_t kInitHeaderSize = 0x20;
constexpr std::size_t kBgmHeaderSize = 0x24;
constexpr std::size_t kBkHeaderSize = 0x40;
constexpr std::size_t kBkInstrumentSize = 0x30;
constexpr std::size_t kPerHeaderSize = 0x10;
constexpr std::size_t kSongSize = 8;
constexpr std::size_t kInitBankSize = 4;
constexpr std::size_t kPhraseSize = 4 * kBgmTrackCount;
constexpr std::size_t kCompositionCount = 4;
constexpr uint32_t kOffsetMask = 0xFFFFFF;
constexpr uint32_t kBranchEntrySize = 3;
constexpr int kMaxFiles = 0x10000;

// The arguments of commands 0xE0 to 0xFF, from a table in the game's code (0x538F0 in Paper Mario's USA image).
constexpr std::array<uint8_t, 32> kArgumentCounts = {2, 1, 1, 1, 4, 3, 2, 0, 2, 1, 1, 1, 1, 1, 1, 2,
                                                     3, 1, 1, 0, 2, 1, 3, 1, 0, 0, 0, 0, 3, 3, 3, 3};

constexpr uint8_t kFirstCommand = 0xE0;
constexpr uint8_t kFirstInvalidNote = 0xD4;
constexpr uint8_t kFirstLongWait = 0x78;
constexpr uint8_t kFirstNote = 0x80;
constexpr uint8_t kFirstLongLength = 0xC0;

constexpr std::array<BgmOp, 32> kOps = {
    BgmOp::kTempo,
    BgmOp::kMasterVolume,
    BgmOp::kMasterTranspose,
    BgmOp::kBusEffect,
    BgmOp::kTempoFade,
    BgmOp::kMasterVolumeFade,
    BgmOp::kMasterEffect,
    BgmOp::kUnused,
    BgmOp::kPatch,
    BgmOp::kInstrumentVolume,
    BgmOp::kInstrumentPan,
    BgmOp::kInstrumentReverb,
    BgmOp::kTrackVolume,
    BgmOp::kCoarseTune,
    BgmOp::kFineTune,
    BgmOp::kTrackDetune,
    BgmOp::kTremolo,
    BgmOp::kTremoloRate,
    BgmOp::kTremoloDepth,
    BgmOp::kTremoloStop,
    BgmOp::kRandomPan,
    BgmOp::kInstrument,
    BgmOp::kInstrumentVolumeFade,
    BgmOp::kReverbType,
    BgmOp::kUnused2,
    BgmOp::kUnused2,
    BgmOp::kUnused2,
    BgmOp::kUnused2,
    BgmOp::kBranch,
    BgmOp::kEventTrigger,
    BgmOp::kDetour,
    BgmOp::kSpecial,
};

std::string NameAt(std::span<const uint8_t> data, std::size_t offset)
{
    return std::string(reinterpret_cast<const char*>(data.data() + offset), 4);
}

// Reads one byte of a track as the game's player does: the end of a detour takes the reader back.
uint8_t NextByte(std::span<const uint8_t> file, BgmCursor& cursor)
{
    if (cursor.position >= file.size())
    {
        throw xsflib::FormatError("a track's commands leave the file at " + std::to_string(cursor.position));
    }

    const uint8_t value = file[cursor.position++];
    if (cursor.detour_left != 0)
    {
        cursor.detour_left--;
        if (cursor.detour_left == 0)
        {
            cursor.position = cursor.saved;
        }
    }

    return value;
}

BgmDrum ReadDrum(std::span<const uint8_t> data, std::size_t at)
{
    BgmDrum drum;
    drum.bank_patch = xsflib::LoadBe16(data.data() + at);
    drum.key_base = xsflib::LoadBe16(data.data() + at + 2);
    drum.volume = data[at + 4];
    drum.pan = static_cast<int8_t>(data[at + 5]);
    drum.reverb = data[at + 6];
    drum.random_tune = data[at + 7];
    drum.random_volume = data[at + 8];
    drum.random_pan = data[at + 9];
    drum.random_reverb = data[at + 10];
    drum.padding = data[at + 11];

    return drum;
}

void WriteDrum(std::span<uint8_t> data, std::size_t at, const BgmDrum& drum)
{
    xsflib::StoreBe16(data.data() + at, drum.bank_patch);
    xsflib::StoreBe16(data.data() + at + 2, drum.key_base);
    data[at + 4] = drum.volume;
    data[at + 5] = static_cast<uint8_t>(drum.pan);
    data[at + 6] = drum.reverb;
    data[at + 7] = drum.random_tune;
    data[at + 8] = drum.random_volume;
    data[at + 9] = drum.random_pan;
    data[at + 10] = drum.random_reverb;
    data[at + 11] = drum.padding;
}

BgmInstrument ReadInstrument(std::span<const uint8_t> data, std::size_t at)
{
    BgmInstrument instrument;
    instrument.bank_patch = xsflib::LoadBe16(data.data() + at);
    instrument.volume = data[at + 2];
    instrument.pan = static_cast<int8_t>(data[at + 3]);
    instrument.reverb = data[at + 4];
    instrument.coarse_tune = static_cast<int8_t>(data[at + 5]);
    instrument.fine_tune = static_cast<int8_t>(data[at + 6]);
    instrument.padding = data[at + 7];

    return instrument;
}

void WriteInstrument(std::span<uint8_t> data, std::size_t at, const BgmInstrument& instrument)
{
    xsflib::StoreBe16(data.data() + at, instrument.bank_patch);
    data[at + 2] = instrument.volume;
    data[at + 3] = static_cast<uint8_t>(instrument.pan);
    data[at + 4] = instrument.reverb;
    data[at + 5] = static_cast<uint8_t>(instrument.coarse_tune);
    data[at + 6] = static_cast<uint8_t>(instrument.fine_tune);
    data[at + 7] = instrument.padding;
}

// Walks one track from `start`, and follows each branch's places.
class TrackWalker
{
public:
    TrackWalker(std::span<const uint8_t> file, BgmWalk& walk) : file_(file), walk_(walk)
    {
    }

    void Walk(uint32_t start)
    {
        if (!seen_.insert(start).second)
        {
            return;
        }

        BgmCursor cursor;
        cursor.position = start;
        Follow(cursor);
    }

private:
    void Follow(BgmCursor cursor)
    {
        for (;;)
        {
            BgmCommand command;
            try
            {
                command = ReadBgmCommand(file_, cursor);
            }
            catch (const xsflib::FormatError&)
            {
                walk_.bad_offsets.push_back(cursor.position);

                return;
            }

            walk_.commands++;
            switch (command.op)
            {
            case BgmOp::kEnd:
            case BgmOp::kReturn:
                return;
            case BgmOp::kInvalid:
                walk_.bad_offsets.push_back(command.offset);

                return;
            case BgmOp::kNote:
                walk_.notes++;
                break;
            case BgmOp::kDetour:
                walk_.detours++;
                break;
            case BgmOp::kBranch:
                FollowBranch(command);
                break;
            default:
                break;
            }
        }
    }

    void FollowBranch(const BgmCommand& command)
    {
        walk_.branches++;
        for (uint8_t value = 0; value < command.args[2]; value++)
        {
            try
            {
                Walk(ReadBgmBranch(file_, command, value).position);
            }
            catch (const xsflib::FormatError&)
            {
                walk_.bad_offsets.push_back(command.offset);
            }
        }
    }

    std::span<const uint8_t> file_;
    BgmWalk& walk_;
    std::set<uint32_t> seen_;
};

// Adds the start of each track that a phrase plays to `tracks`.
void AddPhraseTracks(const BgmFile& file, uint32_t phrase, std::set<uint32_t>& tracks)
{
    for (const BgmTrackInfo& track : file.Phrase(phrase))
    {
        if (track.offset != 0 && !track.disabled)
        {
            tracks.insert(track.offset);
        }
    }
}

} // namespace

SbnArchive SbnArchive::Parse(std::span<const uint8_t> data)
{
    if (!Matches(data))
    {
        throw xsflib::FormatError("not an SBN archive");
    }

    const std::size_t size = xsflib::LoadBe32(data.data() + 4);
    SbnArchive archive;
    archive.bytes_.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(size));

    const std::span<const uint8_t> bytes(archive.bytes_);
    const std::size_t list = xsflib::LoadBe32(bytes.data() + 0x10);
    const std::size_t count = xsflib::LoadBe32(bytes.data() + 0x14);
    archive.entries_.reserve(count);
    for (std::size_t i = 0; i < count; i++)
    {
        const uint8_t* at = bytes.data() + list + i * kFileEntrySize;
        const uint32_t info = xsflib::LoadBe32(at + 4);
        archive.entries_.push_back(
            {xsflib::LoadBe32(at) & kOffsetMask, static_cast<uint8_t>(info >> 24), info & kOffsetMask});
    }

    const std::size_t init = xsflib::LoadBe32(bytes.data() + 0x24);
    if (init == 0)
    {
        return archive;
    }

    if (init + kInitHeaderSize > size || std::memcmp(bytes.data() + init, "INIT", 4) != 0)
    {
        throw xsflib::FormatError("an SBN archive's INIT is missing");
    }

    const std::size_t banks = init + xsflib::LoadBe16(bytes.data() + init + 8);
    const std::size_t banks_size = xsflib::LoadBe16(bytes.data() + init + 10);
    const std::size_t songs = init + xsflib::LoadBe16(bytes.data() + init + 12);
    const std::size_t songs_size = xsflib::LoadBe16(bytes.data() + init + 14);
    const std::size_t others = init + xsflib::LoadBe16(bytes.data() + init + 16);
    const std::size_t others_size = xsflib::LoadBe16(bytes.data() + init + 18);
    if (banks + banks_size > size || songs + songs_size > size || others + others_size > size)
    {
        throw xsflib::FormatError("an SBN archive's INIT leaves the archive");
    }

    for (std::size_t i = 0; i + kInitBankSize <= banks_size; i += kInitBankSize)
    {
        const uint8_t* at = bytes.data() + banks + i;
        const InitBank bank = {xsflib::LoadBe16(at), at[2], at[3]};
        if (bank.file == 0xFFFF)
        {
            break;
        }

        archive.init_banks_.push_back(bank);
    }

    for (std::size_t i = 0; i + kSongSize <= songs_size; i += kSongSize)
    {
        const uint8_t* at = bytes.data() + songs + i;
        archive.songs_.push_back(
            {xsflib::LoadBe16(at), {xsflib::LoadBe16(at + 2), xsflib::LoadBe16(at + 4), xsflib::LoadBe16(at + 6)}});
    }

    for (std::size_t i = 0; i + 2 <= others_size; i += 2)
    {
        archive.other_files_.push_back(xsflib::LoadBe16(bytes.data() + others + i));
    }

    return archive;
}

bool SbnArchive::Matches(std::span<const uint8_t> data)
{
    if (data.size() < kSbnHeaderSize || std::memcmp(data.data(), "SBN ", 4) != 0)
    {
        return false;
    }

    const uint64_t size = xsflib::LoadBe32(data.data() + 4);
    const uint64_t list = xsflib::LoadBe32(data.data() + 0x10);
    const uint64_t count = xsflib::LoadBe32(data.data() + 0x14);

    return size <= data.size() && count < kMaxFiles && list >= kSbnHeaderSize && list + count * kFileEntrySize <= size;
}

const std::vector<SbnArchive::Entry>& SbnArchive::Entries() const
{
    return entries_;
}

std::span<const uint8_t> SbnArchive::File(std::size_t index) const
{
    const Entry& entry = entries_.at(index);
    if (entry.offset + 8ULL > bytes_.size())
    {
        throw xsflib::FormatError("a file of an SBN archive leaves the archive");
    }

    const uint64_t size = xsflib::LoadBe32(bytes_.data() + entry.offset + 4);
    if (entry.offset + size > bytes_.size())
    {
        throw xsflib::FormatError("a file of an SBN archive leaves the archive");
    }

    return std::span<const uint8_t>(bytes_).subspan(entry.offset, static_cast<std::size_t>(size));
}

const std::vector<SbnArchive::Song>& SbnArchive::Songs() const
{
    return songs_;
}

const std::vector<SbnArchive::InitBank>& SbnArchive::InitBanks() const
{
    return init_banks_;
}

const std::vector<uint16_t>& SbnArchive::OtherFiles() const
{
    return other_files_;
}

std::span<const uint8_t> SbnArchive::Bytes() const
{
    return bytes_;
}

BgmCompositionOp BgmCompositionCommand::Op() const
{
    return static_cast<BgmCompositionOp>(raw >> 28);
}

uint32_t BgmCompositionCommand::PhraseOffset(uint32_t composition) const
{
    return composition + ((raw & 0xFFFF) << 2);
}

uint8_t BgmCompositionCommand::Label() const
{
    return static_cast<uint8_t>(raw & 0x1F);
}

uint8_t BgmCompositionCommand::LoopCount() const
{
    return static_cast<uint8_t>((raw >> 5) & 0x7F);
}

BgmFile BgmFile::Parse(std::span<const uint8_t> data)
{
    if (!Matches(data))
    {
        throw xsflib::FormatError("not a BGM file");
    }

    BgmFile file;
    file.bytes_.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(xsflib::LoadBe32(data.data() + 4)));

    const std::span<const uint8_t> bytes(file.bytes_);
    file.name = NameAt(bytes, 8);
    file.timing_preset = bytes[0x10];
    for (std::size_t i = 0; i < kCompositionCount; i++)
    {
        file.compositions_[i] = xsflib::LoadBe16(bytes.data() + 0x14 + i * 2);
    }

    file.drums_offset_ = xsflib::LoadBe16(bytes.data() + 0x1C);
    const std::size_t drums = xsflib::LoadBe16(bytes.data() + 0x1E);
    file.instruments_offset_ = xsflib::LoadBe16(bytes.data() + 0x20);
    const std::size_t instruments = xsflib::LoadBe16(bytes.data() + 0x22);
    const std::size_t drums_at = static_cast<std::size_t>(file.drums_offset_) << 2;
    const std::size_t instruments_at = static_cast<std::size_t>(file.instruments_offset_) << 2;
    if (drums_at + drums * kBgmDrumSize > bytes.size() ||
        instruments_at + instruments * kBgmInstrumentSize > bytes.size())
    {
        throw xsflib::FormatError("a BGM file's tables leave the file");
    }

    for (std::size_t i = 0; i < drums; i++)
    {
        file.drums.push_back(ReadDrum(bytes, drums_at + i * kBgmDrumSize));
    }

    for (std::size_t i = 0; i < instruments; i++)
    {
        file.instruments.push_back(ReadInstrument(bytes, instruments_at + i * kBgmInstrumentSize));
    }

    return file;
}

bool BgmFile::Matches(std::span<const uint8_t> data)
{
    return data.size() >= kBgmHeaderSize && std::memcmp(data.data(), "BGM ", 4) == 0 &&
           xsflib::LoadBe32(data.data() + 4) <= data.size() && xsflib::LoadBe32(data.data() + 4) >= kBgmHeaderSize;
}

uint32_t BgmFile::CompositionOffset(int index) const
{
    return static_cast<uint32_t>(compositions_.at(static_cast<std::size_t>(index))) << 2;
}

std::vector<BgmCompositionCommand> BgmFile::Composition(uint32_t offset) const
{
    std::vector<BgmCompositionCommand> commands;
    for (uint32_t at = offset;; at += 4)
    {
        if (at + 4ULL > bytes_.size())
        {
            throw xsflib::FormatError("a BGM file's composition leaves the file");
        }

        commands.push_back({at, xsflib::LoadBe32(bytes_.data() + at)});
        if (commands.back().raw == 0)
        {
            return commands;
        }
    }
}

std::array<BgmTrackInfo, kBgmTrackCount> BgmFile::Phrase(uint32_t offset) const
{
    if (offset + kPhraseSize > bytes_.size())
    {
        throw xsflib::FormatError("a BGM file's phrase leaves the file");
    }

    std::array<BgmTrackInfo, kBgmTrackCount> tracks = {};
    for (std::size_t i = 0; i < tracks.size(); i++)
    {
        const uint32_t info = xsflib::LoadBe32(bytes_.data() + offset + i * 4);
        const uint32_t relative = info >> 16;
        BgmTrackInfo& track = tracks[i];
        if (relative == 0)
        {
            continue;
        }

        track.offset = offset + relative;
        track.polyphony = static_cast<uint8_t>((info >> 13) & 7);
        track.linked_track = static_cast<uint8_t>((info >> 9) & 0xF);
        track.disabled = (info & 0x100) != 0;
        track.drums = (info & 0x80) != 0;
    }

    return tracks;
}

std::vector<uint8_t> BgmFile::Write() const
{
    std::vector<uint8_t> out = bytes_;
    std::memcpy(out.data() + 8, name.data(), std::min<std::size_t>(name.size(), 4));
    out[0x10] = timing_preset;
    const std::size_t drums_at = static_cast<std::size_t>(drums_offset_) << 2;
    for (std::size_t i = 0; i < drums.size(); i++)
    {
        WriteDrum(out, drums_at + i * kBgmDrumSize, drums[i]);
    }

    const std::size_t instruments_at = static_cast<std::size_t>(instruments_offset_) << 2;
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        WriteInstrument(out, instruments_at + i * kBgmInstrumentSize, instruments[i]);
    }

    return out;
}

std::span<const uint8_t> BgmFile::Bytes() const
{
    return bytes_;
}

BgmCommand ReadBgmCommand(std::span<const uint8_t> file, BgmCursor& cursor)
{
    BgmCommand command;
    command.offset = cursor.position;
    command.opcode = NextByte(file, cursor);
    const uint8_t opcode = command.opcode;
    if (opcode == 0)
    {
        if (cursor.branch_end != 0)
        {
            cursor.position = cursor.branch_end;
            cursor.branch_end = 0;
            command.op = BgmOp::kReturn;
        }
        else
        {
            command.op = BgmOp::kEnd;
        }

        return command;
    }

    if (opcode < kFirstNote)
    {
        command.op = BgmOp::kWait;
        command.length = opcode;
        if (opcode >= kFirstLongWait)
        {
            command.length = static_cast<uint16_t>(((opcode & 7) << 8) + NextByte(file, cursor) + kFirstLongWait);
        }

        return command;
    }

    if (opcode < kFirstInvalidNote)
    {
        command.op = BgmOp::kNote;
        command.key = opcode & 0x7F;
        command.velocity = NextByte(file, cursor);
        command.length = NextByte(file, cursor);
        if (command.length >= kFirstLongLength)
        {
            command.length =
                static_cast<uint16_t>(((command.length & 0x3F) << 8) + NextByte(file, cursor) + kFirstLongLength);
        }

        return command;
    }

    if (opcode < kFirstCommand)
    {
        command.op = BgmOp::kInvalid;

        return command;
    }

    const std::size_t index = opcode - kFirstCommand;
    command.op = kOps[index];
    for (std::size_t i = 0; i < kArgumentCounts[index]; i++)
    {
        command.args[i] = NextByte(file, cursor);
    }

    if (command.op == BgmOp::kDetour)
    {
        cursor.detour_left = command.args[2];
        cursor.saved = cursor.position;
        cursor.position = static_cast<uint32_t>(command.args[0]) << 8 | command.args[1];
    }

    return command;
}

BgmBranchTarget ReadBgmBranch(std::span<const uint8_t> file, const BgmCommand& branch, uint8_t value)
{
    uint32_t table = static_cast<uint32_t>(branch.args[0]) << 8 | branch.args[1];
    if (value < branch.args[2])
    {
        table += value * kBranchEntrySize;
    }

    if (table + kBranchEntrySize > file.size())
    {
        throw xsflib::FormatError("a branch's table leaves the file");
    }

    return {static_cast<uint32_t>(file[table]) << 8 | file[table + 1], file[table + 2] != 0};
}

BgmWalk WalkBgm(const BgmFile& file)
{
    BgmWalk walk;
    std::set<uint32_t> phrases;
    std::set<uint32_t> tracks;
    for (int index = 0; index < static_cast<int>(kCompositionCount); index++)
    {
        const uint32_t composition = file.CompositionOffset(index);
        if (composition == 0)
        {
            continue;
        }

        walk.compositions++;
        for (const BgmCompositionCommand& command : file.Composition(composition))
        {
            if (command.Op() != BgmCompositionOp::kPlayPhrase)
            {
                continue;
            }

            const uint32_t phrase = command.PhraseOffset(composition);
            if (phrases.insert(phrase).second)
            {
                walk.phrases++;
                AddPhraseTracks(file, phrase, tracks);
            }
        }
    }

    TrackWalker walker(file.Bytes(), walk);
    for (const uint32_t start : tracks)
    {
        walk.tracks++;
        walker.Walk(start);
    }

    return walk;
}

BkFile BkFile::Parse(std::span<const uint8_t> data)
{
    if (!Matches(data))
    {
        throw xsflib::FormatError("not a BK file");
    }

    BkFile file;
    file.bytes_.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(xsflib::LoadBe32(data.data() + 4)));
    const std::span<const uint8_t> bytes(file.bytes_);
    file.name = NameAt(bytes, 8);
    for (std::size_t i = 0; i < kBkInstrumentCount; i++)
    {
        const std::size_t offset = xsflib::LoadBe16(bytes.data() + 0x12 + i * 2);
        file.slots[i] = -1;
        if (offset == 0)
        {
            continue;
        }

        if (offset + kBkInstrumentSize > bytes.size())
        {
            throw xsflib::FormatError("an instrument of a BK file leaves the file");
        }

        const uint8_t* at = bytes.data() + offset;
        BkInstrument instrument;
        instrument.offset = static_cast<uint32_t>(offset);
        instrument.wave = xsflib::LoadBe32(at);
        instrument.wave_size = xsflib::LoadBe32(at + 4);
        instrument.loop_state = xsflib::LoadBe32(at + 8);
        instrument.loop_start = xsflib::LoadBe32(at + 0xC);
        instrument.loop_end = xsflib::LoadBe32(at + 0x10);
        instrument.loop_count = xsflib::LoadBe32(at + 0x14);
        instrument.book = xsflib::LoadBe32(at + 0x18);
        instrument.book_size = xsflib::LoadBe16(at + 0x1C);
        instrument.key_base = xsflib::LoadBe16(at + 0x1E);
        instrument.sample_rate = xsflib::LoadBe32(at + 0x20);
        instrument.type = at[0x24];
        std::copy_n(at + 0x25, instrument.unused.size(), instrument.unused.begin());
        instrument.envelopes = xsflib::LoadBe32(at + 0x2C);
        file.slots[i] = static_cast<int32_t>(file.instruments.size());
        file.instruments.push_back(instrument);
    }

    return file;
}

bool BkFile::Matches(std::span<const uint8_t> data)
{
    return data.size() >= kBkHeaderSize && data[0] == 'B' && data[1] == 'K' && data[0xC] == 'C' && data[0xD] == 'R' &&
           xsflib::LoadBe32(data.data() + 4) >= kBkHeaderSize && xsflib::LoadBe32(data.data() + 4) <= data.size();
}

VadpcmBook BkFile::Book(const BkInstrument& instrument) const
{
    constexpr std::size_t kOrder = 2;
    constexpr std::size_t kPredictorBytes = kOrder * kVadpcmGroupSamples * 2;
    const std::size_t size = instrument.book_size;
    if (size == 0 || size % kPredictorBytes != 0 || instrument.book + size > bytes_.size())
    {
        throw xsflib::FormatError("a book of a BK file has a bad place or size");
    }

    VadpcmBook book;
    book.order = kOrder;
    book.predictor_count = static_cast<int32_t>(size / kPredictorBytes);
    book.coefficients.reserve(size / 2);
    for (std::size_t i = 0; i < size; i += 2)
    {
        book.coefficients.push_back(static_cast<int16_t>(xsflib::LoadBe16(bytes_.data() + instrument.book + i)));
    }

    return book;
}

VadpcmState BkFile::LoopState(const BkInstrument& instrument) const
{
    VadpcmState state = {};
    if (instrument.loop_state + state.size() * 2 > bytes_.size())
    {
        throw xsflib::FormatError("a loop's state of a BK file leaves the file");
    }

    for (std::size_t i = 0; i < state.size(); i++)
    {
        state[i] = static_cast<int16_t>(xsflib::LoadBe16(bytes_.data() + instrument.loop_state + i * 2));
    }

    return state;
}

std::span<const uint8_t> BkFile::Wave(const BkInstrument& instrument) const
{
    if (instrument.wave + static_cast<uint64_t>(instrument.wave_size) > bytes_.size())
    {
        throw xsflib::FormatError("a wave of a BK file leaves the file");
    }

    return std::span<const uint8_t>(bytes_).subspan(instrument.wave, instrument.wave_size);
}

std::vector<uint8_t> BkFile::Write() const
{
    std::vector<uint8_t> out = bytes_;
    std::memcpy(out.data() + 8, name.data(), std::min<std::size_t>(name.size(), 4));
    for (const BkInstrument& instrument : instruments)
    {
        uint8_t* at = out.data() + instrument.offset;
        xsflib::StoreBe32(at, instrument.wave);
        xsflib::StoreBe32(at + 4, instrument.wave_size);
        xsflib::StoreBe32(at + 8, instrument.loop_state);
        xsflib::StoreBe32(at + 0xC, instrument.loop_start);
        xsflib::StoreBe32(at + 0x10, instrument.loop_end);
        xsflib::StoreBe32(at + 0x14, instrument.loop_count);
        xsflib::StoreBe32(at + 0x18, instrument.book);
        xsflib::StoreBe16(at + 0x1C, instrument.book_size);
        xsflib::StoreBe16(at + 0x1E, instrument.key_base);
        xsflib::StoreBe32(at + 0x20, instrument.sample_rate);
        at[0x24] = instrument.type;
        std::copy(instrument.unused.begin(), instrument.unused.end(), at + 0x25);
        xsflib::StoreBe32(at + 0x2C, instrument.envelopes);
    }

    return out;
}

std::span<const uint8_t> BkFile::Bytes() const
{
    return bytes_;
}

std::vector<BgmDrum> ParsePerFile(std::span<const uint8_t> data)
{
    if (data.size() < kPerHeaderSize || std::memcmp(data.data(), "PER ", 4) != 0)
    {
        throw xsflib::FormatError("not a PER file");
    }

    std::vector<BgmDrum> drums;
    for (std::size_t at = kPerHeaderSize; at + kBgmDrumSize <= data.size(); at += kBgmDrumSize)
    {
        drums.push_back(ReadDrum(data, at));
    }

    return drums;
}

std::vector<BgmInstrument> ParsePrgFile(std::span<const uint8_t> data)
{
    if (data.size() < kPerHeaderSize || std::memcmp(data.data(), "PRG ", 4) != 0)
    {
        throw xsflib::FormatError("not a PRG file");
    }

    std::vector<BgmInstrument> instruments;
    for (std::size_t at = kPerHeaderSize; at + kBgmInstrumentSize <= data.size(); at += kBgmInstrumentSize)
    {
        instruments.push_back(ReadInstrument(data, at));
    }

    return instruments;
}

std::size_t FindSbnArchive(std::span<const uint8_t> image)
{
    constexpr std::size_t kAlignment = 16;
    for (std::size_t offset = 0; offset + kSbnHeaderSize <= image.size(); offset += kAlignment)
    {
        if (SbnArchive::Matches(image.subspan(offset)))
        {
            return offset;
        }
    }

    return image.size();
}

} // namespace ntdlib::n64

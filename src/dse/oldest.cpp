// SPDX-License-Identifier: MIT

#include "ntdlib/dse/oldest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "common/magic.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/rounding.h"

namespace ntdlib::dse
{

namespace
{

using ntdlib::detail::HasMagic;

constexpr std::size_t kSongSettings = 0x10;
constexpr std::size_t kUpgradedSettings = 0x30;
constexpr std::size_t kKeptSettings = 0x20; // where an upgraded song keeps the 16 bytes of the old settings
constexpr std::size_t kEntrySize = 0x40;
constexpr std::size_t kShortEntrySize = 0x30; // in some banks, the entries end before the envelope. No build reads it.
constexpr std::size_t kProgramHeader = 0x10;
constexpr std::size_t kEntriesOf8Bytes = 0x80; // the 16 entries of 8 bytes after a program's header
constexpr std::size_t kLfoSize = 0x10;
constexpr std::size_t kSplitSize = 0x30;

bool IsChunk(const OldestFile::Chunk& chunk, const char* label)
{
    return HasMagic(chunk.header, label);
}

// The size of a table of `count` 16-bit offsets, padded to 16 bytes.
std::size_t TableSize(std::size_t count)
{
    return AlignUp(2 * count, 16);
}

std::size_t HeaderSize(std::span<const uint8_t> file)
{
    if (HasMagic(file, "swdl"))
    {
        return WaveBank::kHeaderSize;
    }

    return Song::kHeaderSize;
}

// The header and the chunks of the file of version 0x402 at the start of `bytes`, up to its end chunk. A chunk's
// padding ends at its alignment or at the end of `bytes`.
OldestFile Walk(std::span<const uint8_t> bytes)
{
    const std::size_t header_size = HeaderSize(bytes);
    OldestFile parsed;
    parsed.header.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header_size));
    std::size_t at = header_size;
    while (true)
    {
        if (at + 16 > bytes.size())
        {
            throw xsflib::FormatError("the file doesn't end with its end chunk");
        }

        OldestFile::Chunk chunk;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), 16, chunk.header.begin());
        at += 16;
        const bool song = IsChunk(chunk, "song");
        const std::size_t length = song ? kSongSettings : xsflib::LoadLe32(&chunk.header[12]);
        if (length > bytes.size() - at)
        {
            throw xsflib::FormatError("a chunk runs past the file's end");
        }

        chunk.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                          bytes.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        const std::size_t alignment = song ? 1 : std::max<std::size_t>(chunk.header[8], 1);
        const std::size_t next = std::min(AlignUp(at, alignment), bytes.size());
        chunk.padding.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                             bytes.begin() + static_cast<std::ptrdiff_t>(next));
        at = next;
        const bool end = IsChunk(chunk, "eoc ") || IsChunk(chunk, "eod ");
        parsed.chunks.push_back(std::move(chunk));
        if (end)
        {
            return parsed;
        }
    }
}

// Writes a chunk's header for version 0x415: the kind's bytes of a bank's chunk give 0x415 in place of 0x402.
std::array<uint8_t, 16> UpgradedChunkHeader(const OldestFile::Chunk& chunk, bool bank)
{
    std::array<uint8_t, 16> header = chunk.header;
    if (bank)
    {
        xsflib::StoreLe16(&header[6], kVersion);
    }

    return header;
}

std::vector<uint8_t> UpgradedSong(const OldestFile& song)
{
    std::vector<uint8_t> out(song.header);
    xsflib::StoreLe16(&out[0x0C], kVersion);
    for (const OldestFile::Chunk& chunk : song.chunks)
    {
        out.insert(out.end(), chunk.header.begin(), chunk.header.end());
        if (!IsChunk(chunk, "song"))
        {
            out.insert(out.end(), chunk.data.begin(), chunk.data.end());
            out.insert(out.end(), chunk.padding.begin(), chunk.padding.end());
            continue;
        }

        // The settings, in the places of version 0x415 (0x020C0784 in Luminous Arc's ARM9 binary).
        const std::vector<uint8_t>& old = chunk.data;
        std::array<uint8_t, kUpgradedSettings> settings = {};
        settings[0] = old[0];
        settings[1] = old[1];
        settings[2] = old[2];
        settings[3] = old[3];
        settings[4] = old[6];
        settings[6] = old[4];
        settings[7] = old[5];
        settings[0x19] = static_cast<uint8_t>(old[7] >> 4);
        settings[0x1B] = old[8];
        std::copy(old.begin(), old.end(), settings.begin() + kKeptSettings);
        out.insert(out.end(), settings.begin(), settings.end());
    }

    xsflib::StoreLe32(&out[0x08], static_cast<uint32_t>(out.size()));

    return out;
}

// A wave's entry in the places of version 0x415.
std::array<uint8_t, kEntrySize> UpgradedWave(std::span<const uint8_t> old)
{
    std::array<uint8_t, kEntrySize> entry = {};
    entry[0x00] = old[0x00];
    xsflib::StoreLe16(&entry[0x02], old[0x01]);
    entry[0x06] = old[0x04];
    entry[0x08] = old[0x06];
    entry[0x09] = old[0x07];
    entry[0x13] = old[0x10];
    entry[0x15] = old[0x11];
    xsflib::StoreLe32(&entry[0x20], xsflib::LoadLe16(&old[0x12]));
    std::copy_n(old.begin() + 0x14, 12, entry.begin() + 0x24);
    if (old.size() >= kEntrySize)
    {
        std::copy_n(old.begin() + 0x30, 16, entry.begin() + 0x30);
    }

    return entry;
}

// The size of the entries of the wavi chunk `data` with `count` slots: 64 bytes, or 48 where the entries are 48 bytes
// apart or the last ends 48 bytes before the chunk's end.
std::size_t EntrySize(const std::vector<uint8_t>& data, std::size_t count)
{
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < count && 2 * i + 2 <= data.size(); i++)
    {
        if (const std::size_t offset = xsflib::LoadLe16(&data[2 * i]); offset != 0)
        {
            offsets.push_back(offset);
        }
    }

    offsets.push_back(data.size());
    std::ranges::sort(offsets);
    for (std::size_t i = 0; i + 1 < offsets.size(); i++)
    {
        if (offsets[i + 1] - offsets[i] < kEntrySize)
        {
            return kShortEntrySize;
        }
    }

    return kEntrySize;
}

// A split in the places of version 0x415.
std::array<uint8_t, kSplitSize> UpgradedSplit(std::span<const uint8_t> old)
{
    std::array<uint8_t, kSplitSize> split = {};
    std::copy_n(old.begin(), 0x10, split.begin());
    xsflib::StoreLe16(&split[SplitView::kWave], old[0x11]);
    split[SplitView::kTuning] = old[0x12];
    split[SplitView::kTuning + 1] = old[0x13];
    split[SplitView::kRootKey] = old[0x14];
    split[SplitView::kCoarseTuning] = old[0x15];
    split[SplitView::kVolume] = old[0x16];
    split[SplitView::kPan] = old[0x17];
    split[SplitView::kKeyGroup] = old[0x18];
    std::copy_n(old.begin() + SplitView::kEnvelope, 16, split.begin() + SplitView::kEnvelope);

    return split;
}

// A table of 16-bit offsets of `count` entries at the start of `data`, and the entries that it names, each laid out
// again by `upgrade`. `upgrade` returns the entry and the size it takes in `data`.
template <typename Upgrade>
std::vector<uint8_t> UpgradedTable(const std::vector<uint8_t>& data, std::size_t count, Upgrade upgrade)
{
    const std::size_t table = TableSize(count);
    if (table > data.size())
    {
        throw xsflib::FormatError("a table of offsets runs past its chunk");
    }

    std::vector<uint8_t> out(table, 0);
    for (std::size_t i = 0; i < count; i++)
    {
        const std::size_t offset = xsflib::LoadLe16(&data[2 * i]);
        if (offset == 0)
        {
            continue;
        }

        if (offset >= data.size())
        {
            throw xsflib::FormatError("a table names an entry past its chunk");
        }

        xsflib::StoreLe16(&out[2 * i], static_cast<uint16_t>(out.size()));
        const std::vector<uint8_t> entry = upgrade(std::span(data).subspan(offset));
        out.insert(out.end(), entry.begin(), entry.end());
    }

    return out;
}

std::vector<uint8_t> UpgradedProgram(std::span<const uint8_t> old)
{
    if (old.size() < kProgramHeader + kEntriesOf8Bytes)
    {
        throw xsflib::FormatError("a program runs past its chunk");
    }

    const std::size_t splits = old[0x01];
    const std::size_t lfos = old[0x0B];
    const std::size_t first_split = kProgramHeader + kEntriesOf8Bytes + lfos * kLfoSize;
    if (first_split + splits * kSplitSize > old.size())
    {
        throw xsflib::FormatError("a program runs past its chunk");
    }

    // The header with the number and the count of splits in 16 bits, the LFOs, 16 bytes of filler and the splits.
    std::vector<uint8_t> out(old.begin(), old.begin() + kProgramHeader);
    out[0x01] = 0;
    out[0x02] = static_cast<uint8_t>(splits);
    out[0x03] = 0;
    const auto lfo_bytes = old.subspan(kProgramHeader + kEntriesOf8Bytes, lfos * kLfoSize);
    out.insert(out.end(), lfo_bytes.begin(), lfo_bytes.end());
    out.insert(out.end(), 16, 0);
    for (std::size_t i = 0; i < splits; i++)
    {
        const std::array<uint8_t, kSplitSize> split = UpgradedSplit(old.subspan(first_split + i * kSplitSize));
        out.insert(out.end(), split.begin(), split.end());
    }

    return out;
}

std::vector<uint8_t> UpgradedBank(const OldestFile& bank)
{
    const std::size_t waves = bank.header[0x46];
    const std::size_t programs = bank.header[0x47];
    const std::size_t key_groups = bank.header[0x48];
    std::vector<uint8_t> out(bank.header);
    xsflib::StoreLe16(&out[0x0C], kVersion);
    xsflib::StoreLe16(&out[0x46], static_cast<uint16_t>(waves));
    xsflib::StoreLe16(&out[0x48], static_cast<uint16_t>(programs));
    out[0x4A] = static_cast<uint8_t>(key_groups);
    out[0x4B] = 0;
    for (const OldestFile::Chunk& chunk : bank.chunks)
    {
        std::vector<uint8_t> data;
        std::vector<uint8_t> padding;
        if (IsChunk(chunk, "wavi"))
        {
            const std::size_t size = EntrySize(chunk.data, waves);
            const auto wave = [size](std::span<const uint8_t> old)
            {
                if (old.size() < size)
                {
                    throw xsflib::FormatError("a wave's entry runs past its chunk");
                }

                const std::array<uint8_t, kEntrySize> entry = UpgradedWave(old.first(size));
                return std::vector<uint8_t>(entry.begin(), entry.end());
            };
            data = UpgradedTable(chunk.data, waves, wave);
        }
        else if (IsChunk(chunk, "prgi"))
        {
            data = UpgradedTable(chunk.data, programs, UpgradedProgram);
        }
        else
        {
            // The chunk keeps its padding. Each chunk starts at a multiple of 16 in both files. The padding therefore
            // fits.
            data = chunk.data;
            padding = chunk.padding;
        }

        std::array<uint8_t, 16> header = UpgradedChunkHeader(chunk, true);
        xsflib::StoreLe32(&header[12], static_cast<uint32_t>(data.size()));
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), data.begin(), data.end());
        out.insert(out.end(), padding.begin(), padding.end());
        out.resize(AlignUp(out.size(), 16), 0);
    }

    xsflib::StoreLe32(&out[0x08], static_cast<uint32_t>(out.size()));

    return out;
}

} // namespace

bool IsOldestVersion(std::span<const uint8_t> file)
{
    return file.size() >= Song::kHeaderSize && xsflib::LoadLe16(&file[0x0C]) == kOldestVersion &&
           (HasMagic(file, "smdl") || HasMagic(file, "swdl") || HasMagic(file, "sedl"));
}

OldestFile OldestFile::Parse(std::span<const uint8_t> file)
{
    if (!IsOldestVersion(file))
    {
        throw xsflib::FormatError("not a DSE file of version 0x402");
    }

    std::size_t size = xsflib::LoadLe32(&file[0x08]);
    if (size == 0)
    {
        size = file.size();
    }
    if (size > file.size() || size < HeaderSize(file))
    {
        throw xsflib::FormatError("the file's size runs past its end");
    }

    OldestFile parsed = Walk(file.first(size));
    if (parsed.Size() != size)
    {
        throw xsflib::FormatError("the file holds bytes after its end chunk");
    }

    return parsed;
}

std::optional<std::size_t> OldestFileSize(std::span<const uint8_t> bytes)
{
    if (!IsOldestVersion(bytes) || bytes.size() < HeaderSize(bytes))
    {
        return std::nullopt;
    }
    if (const std::size_t size = xsflib::LoadLe32(&bytes[0x08]); size != 0)
    {
        return size <= bytes.size() ? std::optional(size) : std::nullopt;
    }

    try
    {
        return Walk(bytes).Size();
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

std::size_t OldestFile::Size() const
{
    std::size_t size = header.size();
    for (const Chunk& chunk : chunks)
    {
        size += chunk.header.size() + chunk.data.size() + chunk.padding.size();
    }

    return size;
}

std::vector<uint8_t> OldestFile::Write() const
{
    std::vector<uint8_t> out(header);
    for (const Chunk& chunk : chunks)
    {
        out.insert(out.end(), chunk.header.begin(), chunk.header.end());
        out.insert(out.end(), chunk.data.begin(), chunk.data.end());
        out.insert(out.end(), chunk.padding.begin(), chunk.padding.end());
    }

    if (xsflib::LoadLe32(&header[0x08]) != 0)
    {
        xsflib::StoreLe32(&out[0x08], static_cast<uint32_t>(out.size()));
    }

    return out;
}

std::vector<uint8_t> Upgraded(std::span<const uint8_t> file)
{
    const OldestFile parsed = OldestFile::Parse(file);
    if (HasMagic(file, "smdl"))
    {
        return Song::Parse(UpgradedSong(parsed)).Write();
    }
    if (HasMagic(file, "swdl"))
    {
        return WaveBank::Parse(UpgradedBank(parsed)).Write();
    }

    throw UnsupportedLayout("a bank of sound effects of version 0x402");
}

namespace
{

constexpr std::size_t kMostHeld = 0xFF;     // the counts and the numbers that version 0x402 holds in single bytes
constexpr uint32_t kMostRate = 0xFFFF;      // a wave's sample rate takes 16 bits
constexpr std::size_t kMostTable = 0x10000; // the offsets of a table take 16 bits
constexpr std::size_t kWaveCount = 0x46;    // the counts of the waves, the programs and the key groups in a header
constexpr std::size_t kProgramCount = 0x47;
constexpr std::size_t kKeyGroupCount = 0x48;
constexpr std::size_t kNewerCounts = 0x40; // where version 0x415 starts to differ in a bank's header

std::size_t ProgramCount(const WaveBank& bank)
{
    return bank.instruments ? bank.instruments->programs.size() : 0;
}

std::size_t KeyGroupCount(const WaveBank& bank)
{
    return bank.instruments ? bank.instruments->key_groups.size() : 0;
}

// Throws std::invalid_argument for a value of `bank` that version 0x402 can't hold.
void CheckHeld(const WaveBank& bank)
{
    if (bank.waves.size() > kMostHeld || ProgramCount(bank) > kMostHeld || KeyGroupCount(bank) > kMostHeld)
    {
        throw std::invalid_argument("a file of version 0x402 holds at most 255 wave slots, programs and key groups");
    }

    for (const std::optional<WaveInfo>& wave : bank.waves)
    {
        if (wave && wave->SampleRate() > kMostRate)
        {
            throw std::invalid_argument("a file of version 0x402 holds sample rates up to 65,535 Hz");
        }
    }

    if (!bank.instruments)
    {
        return;
    }

    for (const std::optional<Program>& program : bank.instruments->programs)
    {
        if (!program)
        {
            continue;
        }

        if (program->splits.size() > kMostHeld)
        {
            throw std::invalid_argument("a file of version 0x402 holds at most 255 splits in a program");
        }

        // The count of LFOs is a byte at 0x0B, and the driver plays every LFO that it counts: a run of Luminous Arc's
        // code played programs of 3 and of 5 LFOs as the model plays them.
        if (program->lfos.size() > kMostHeld)
        {
            throw std::invalid_argument("a file of version 0x402 holds at most 255 LFOs in a program");
        }

        for (const std::array<uint8_t, kSplitSize>& split : program->splits)
        {
            if (SplitView(split).Wave() > kMostHeld)
            {
                throw std::invalid_argument("a split of version 0x402 plays waves 0 to 255");
            }
        }
    }
}

// Writes the values of `wave` at their places in `old`, a wave's entry of version 0x402 of 48 or 64 bytes: the reverse
// of UpgradedWave.
void WriteWave(std::span<uint8_t> old, const WaveInfo& wave)
{
    const std::array<uint8_t, kEntrySize>& entry = wave.bytes;
    old[0x00] = entry[0x00];
    old[0x01] = entry[0x02];
    old[0x04] = entry[0x06];
    old[0x06] = entry[0x08];
    old[0x07] = entry[0x09];
    old[0x10] = entry[0x13];
    old[0x11] = entry[0x15];
    xsflib::StoreLe16(&old[0x12], static_cast<uint16_t>(wave.SampleRate()));
    std::copy_n(entry.begin() + 0x24, 12, old.begin() + 0x14);
    if (old.size() >= kEntrySize)
    {
        std::copy_n(entry.begin() + 0x30, 16, old.begin() + 0x30);
    }
}

// Writes the values of `split` at their places in `old`, a split of version 0x402: the reverse of UpgradedSplit.
void WriteSplit(std::span<uint8_t> old, const std::array<uint8_t, kSplitSize>& split)
{
    std::copy_n(split.begin(), 0x10, old.begin());
    old[0x11] = split[SplitView::kWave];
    old[0x12] = split[SplitView::kTuning];
    old[0x13] = split[SplitView::kTuning + 1];
    old[0x14] = split[SplitView::kRootKey];
    old[0x15] = split[SplitView::kCoarseTuning];
    old[0x16] = split[SplitView::kVolume];
    old[0x17] = split[SplitView::kPan];
    old[0x18] = split[SplitView::kKeyGroup];
    std::copy_n(split.begin() + SplitView::kEnvelope, 16, old.begin() + SplitView::kEnvelope);
}

// The size of `program` in version 0x402: its header, the 16 entries of 8 bytes, its LFOs and its splits.
std::size_t ProgramSize(const Program& program)
{
    return kProgramHeader + kEntriesOf8Bytes + program.lfos.size() * kLfoSize + program.splits.size() * kSplitSize;
}

// Writes `program` at `old`, a program of version 0x402 with as many LFOs and splits: the header's number, counts and
// values, the LFOs and the splits. The reverse of UpgradedProgram. The 16 entries of 8 bytes after the header stay.
void WriteProgram(std::span<uint8_t> old, const Program& program)
{
    old[0x00] = program.header[0x00];
    old[0x01] = static_cast<uint8_t>(program.splits.size());
    std::copy(program.header.begin() + 0x04, program.header.end(), old.begin() + 0x04);
    old[0x0B] = static_cast<uint8_t>(program.lfos.size());
    std::size_t at = kProgramHeader + kEntriesOf8Bytes;
    for (const std::array<uint8_t, kLfoSize>& lfo : program.lfos)
    {
        std::ranges::copy(lfo, old.begin() + static_cast<std::ptrdiff_t>(at));
        at += kLfoSize;
    }

    for (const std::array<uint8_t, kSplitSize>& split : program.splits)
    {
        WriteSplit(old.subspan(at, kSplitSize), split);
        at += kSplitSize;
    }
}

// The offset of entry `slot` of the table of `count` 16-bit offsets at the start of `data`, or 0 for an empty slot.
std::size_t EntryAt(const std::vector<uint8_t>& data, std::size_t slot)
{
    return 2 * slot + 2 <= data.size() ? xsflib::LoadLe16(&data[2 * slot]) : 0;
}

// Whether the wavi chunk `data` of version 0x402 holds an entry in the same slots as `bank`, each inside the chunk.
bool SameWaves(const std::vector<uint8_t>& data, const WaveBank& bank)
{
    const std::size_t size = EntrySize(data, bank.waves.size());
    for (std::size_t slot = 0; slot < bank.waves.size(); slot++)
    {
        const std::size_t offset = EntryAt(data, slot);
        if ((offset != 0) != bank.waves[slot].has_value() || offset + size > data.size())
        {
            return false;
        }
    }

    return true;
}

// Whether the prgi chunk `data` of version 0x402 holds a program in the same slots as `bank`, each with as many LFOs
// and splits, inside the chunk.
bool SamePrograms(const std::vector<uint8_t>& data, const WaveBank& bank)
{
    const std::size_t count = ProgramCount(bank);
    for (std::size_t slot = 0; slot < count; slot++)
    {
        const std::optional<Program>& program = bank.instruments->programs[slot];
        const std::size_t offset = EntryAt(data, slot);
        if ((offset != 0) != program.has_value())
        {
            return false;
        }
        if (!program)
        {
            continue;
        }

        if (offset + kProgramHeader > data.size() || std::size_t{data[offset + 0x01]} != program->splits.size() ||
            std::size_t{data[offset + 0x0B]} != program->lfos.size() || offset + ProgramSize(*program) > data.size())
        {
            return false;
        }
    }

    return true;
}

// Whether `like`, a bank of version 0x402, has the chunks and the tables of `newer`, the chunks of bank.Write(), so
// that OldestBank writes `bank` into it.
bool SameLayout(const OldestFile& like, const OldestFile& newer, const WaveBank& bank)
{
    if (like.chunks.size() != newer.chunks.size() || std::size_t{like.header[kWaveCount]} != bank.waves.size() ||
        std::size_t{like.header[kProgramCount]} != ProgramCount(bank) ||
        std::size_t{like.header[kKeyGroupCount]} != KeyGroupCount(bank))
    {
        return false;
    }

    for (std::size_t c = 0; c < like.chunks.size(); c++)
    {
        const OldestFile::Chunk& chunk = like.chunks[c];
        if (std::memcmp(chunk.header.data(), newer.chunks[c].header.data(), 4) != 0 ||
            (IsChunk(chunk, "wavi") && !SameWaves(chunk.data, bank)) ||
            (IsChunk(chunk, "prgi") && (!bank.instruments || !SamePrograms(chunk.data, bank))))
        {
            return false;
        }
    }

    return true;
}

// `data`, a wavi or prgi chunk of `like` with the same layout as `bank`, with `bank`'s values.
std::vector<uint8_t> PatchedTable(const OldestFile::Chunk& chunk, const WaveBank& bank)
{
    std::vector<uint8_t> data = chunk.data;
    if (IsChunk(chunk, "wavi"))
    {
        const std::size_t size = EntrySize(chunk.data, bank.waves.size());
        for (std::size_t slot = 0; slot < bank.waves.size(); slot++)
        {
            if (bank.waves[slot])
            {
                WriteWave(std::span(data).subspan(EntryAt(chunk.data, slot), size), *bank.waves[slot]);
            }
        }
    }
    else
    {
        for (std::size_t slot = 0; slot < ProgramCount(bank); slot++)
        {
            if (const std::optional<Program>& program = bank.instruments->programs[slot])
            {
                WriteProgram(std::span(data).subspan(EntryAt(chunk.data, slot)), *program);
            }
        }
    }

    return data;
}

// A table of 16-bit offsets of `count` slots, padded to 16 bytes, and the entries that `entry` gives for the filled
// slots, in their order: the layout of UpgradedTable. Throws std::invalid_argument for an entry that would start at or
// past 64 KiB. Only the programs can reach that: 255 wave entries of 64 bytes take less than 16 KiB.
template <typename Entry>
std::vector<uint8_t> NewTable(std::size_t count, Entry entry)
{
    std::vector<uint8_t> out(TableSize(count), 0);
    for (std::size_t slot = 0; slot < count; slot++)
    {
        std::optional<std::vector<uint8_t>> bytes = entry(slot);
        if (!bytes)
        {
            continue;
        }

        if (out.size() >= kMostTable)
        {
            throw std::invalid_argument("a program has to start within the first 64 KiB of the programs");
        }

        xsflib::StoreLe16(&out[2 * slot], static_cast<uint16_t>(out.size()));
        out.insert(out.end(), bytes->begin(), bytes->end());
    }

    return out;
}

std::vector<uint8_t> NewWaves(const WaveBank& bank)
{
    const auto entry = [&bank](std::size_t slot) -> std::optional<std::vector<uint8_t>>
    {
        if (!bank.waves[slot])
        {
            return std::nullopt;
        }

        std::vector<uint8_t> bytes(kEntrySize, 0);
        WriteWave(bytes, *bank.waves[slot]);

        return bytes;
    };

    return NewTable(bank.waves.size(), entry);
}

std::vector<uint8_t> NewPrograms(const WaveBank& bank)
{
    const auto entry = [&bank](std::size_t slot) -> std::optional<std::vector<uint8_t>>
    {
        const std::optional<Program>& program = bank.instruments->programs[slot];
        if (!program)
        {
            return std::nullopt;
        }

        std::vector<uint8_t> bytes(ProgramSize(*program), 0);
        WriteProgram(bytes, *program);

        return bytes;
    };

    return NewTable(ProgramCount(bank), entry);
}

// A header of version 0x402 for `bank` without a bank to keep the bytes of.
std::vector<uint8_t> NewHeader(const WaveBank& bank)
{
    std::vector<uint8_t> header(bank.header.begin(), bank.header.end());
    xsflib::StoreLe16(&header[0x0C], kOldestVersion);
    std::fill(header.begin() + kNewerCounts, header.begin() + kWaveCount, uint8_t{0});
    std::fill(header.begin() + kKeyGroupCount + 1, header.end(), uint8_t{0xFF});
    xsflib::StoreLe32(&header[0x08], 1); // the size follows

    return header;
}

} // namespace

std::vector<uint8_t> OldestBank(const WaveBank& bank, std::span<const uint8_t> like)
{
    CheckHeld(bank);
    const OldestFile newer = Walk(bank.Write());
    std::optional<OldestFile> old;
    if (!like.empty())
    {
        if (!HasMagic(like, "swdl"))
        {
            throw std::invalid_argument("not a wave bank of version 0x402");
        }
        old = OldestFile::Parse(like);
    }

    const bool patch = old && SameLayout(*old, newer, bank);
    if (!patch && !bank.samples)
    {
        throw std::invalid_argument("a file of version 0x402 holds no bank without samples");
    }

    // The header, with the counts of the bank.
    OldestFile out;
    out.header = old ? old->header : NewHeader(bank);
    out.header[kWaveCount] = static_cast<uint8_t>(bank.waves.size());
    out.header[kProgramCount] = static_cast<uint8_t>(ProgramCount(bank));
    out.header[kKeyGroupCount] = static_cast<uint8_t>(KeyGroupCount(bank));

    // The chunks of version 0x415 in the same order, each with a chunk header of version 0x402. The tables keep the
    // bytes of `like` where its layout is the bank's. The other chunks are the same in both versions.
    std::size_t at = out.header.size();
    for (std::size_t c = 0; c < newer.chunks.size(); c++)
    {
        const OldestFile::Chunk& chunk = newer.chunks[c];
        OldestFile::Chunk written;
        written.header = patch ? old->chunks[c].header : chunk.header;
        xsflib::StoreLe16(&written.header[6], kOldestVersion);
        const bool table = IsChunk(chunk, "wavi") || IsChunk(chunk, "prgi");
        if (patch && table)
        {
            written.data = PatchedTable(old->chunks[c], bank);
        }
        else if (table)
        {
            written.data = IsChunk(chunk, "wavi") ? NewWaves(bank) : NewPrograms(bank);
        }
        else
        {
            written.data = chunk.data;
        }

        // A chunk that isn't a table keeps the padding that the bank holds in version 0x415. The bytes after the key
        // groups are such padding. Each chunk starts on a multiple of 16 bytes in both versions. A table of the same
        // size keeps the padding of `like`. A new table takes zeros.
        xsflib::StoreLe32(&written.header[12], static_cast<uint32_t>(written.data.size()));
        at += written.header.size() + written.data.size();
        const std::size_t alignment = std::max<std::size_t>(written.header[8], 1);
        written.padding.assign(AlignUp(at, alignment) - at, 0);
        if (!table && chunk.padding.size() == written.padding.size())
        {
            written.padding = chunk.padding;
        }
        else if (patch && written.data.size() == old->chunks[c].data.size())
        {
            written.padding = old->chunks[c].padding;
        }
        at += written.padding.size();
        out.chunks.push_back(std::move(written));
    }

    return out.Write();
}

} // namespace ntdlib::dse

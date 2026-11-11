// SPDX-License-Identifier: MIT

#include "ntdlib/sdat/heap.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/sdat/formats.h"

namespace ntdlib::sdat
{

namespace
{

// The fields of a bank and a wave archive that the driver reads in main memory: a bank's links to its wave archives,
// 8 bytes each, a wave archive's first link, and the count and table of its waves.
constexpr uint32_t kBankLinks = 0x18;
constexpr uint32_t kWaveArchiveFirstLink = 0x18;
constexpr uint32_t kWaveArchiveSize = 0x08;
constexpr uint32_t kWaveCount = 0x38;
constexpr uint32_t kWaveTable = 0x3C;

constexpr uint32_t kSingleWaves = 1u << 24;

} // namespace

HeapFullError::HeapFullError() : std::runtime_error("the files don't fit in the sound heap")
{
}

SoundHeap::SoundHeap(std::span<uint8_t> memory, uint32_t base, uint32_t start, uint32_t end)
    : memory_(memory), base_(base), next_(start), end_(end)
{
}

uint32_t SoundHeap::LoadFile(const Archive& archive, uint32_t id)
{
    if (const auto found = files_.find(id); found != files_.end())
    {
        return found->second;
    }

    const std::span<const uint8_t> file = archive.File(id);
    const uint32_t at = Allocate(file.size());
    std::memcpy(&memory_[at - base_], file.data(), file.size());
    files_[id] = at;
    return at;
}

uint32_t SoundHeap::LoadBank(const Archive& archive, uint32_t bank)
{
    if (bank >= archive.banks.size() || !archive.banks[bank])
    {
        throw xsflib::FormatError("the archive has no bank " + std::to_string(bank));
    }

    const BankInfo& info = *archive.banks[bank];
    const uint32_t at = LoadFile(archive, info.file_id);
    const Bank parsed = Bank::Parse(archive.File(info.file_id));
    for (uint32_t slot = 0; slot < info.wave_archives.size(); slot++)
    {
        const uint16_t index = info.wave_archives[slot];
        if (index == kNoWaveArchive || index >= archive.wave_archives.size() || !archive.wave_archives[index])
        {
            continue;
        }

        const uint32_t wave_archive = archive.wave_archives[index]->file_id;
        const uint32_t id = wave_archive & kFileIdBits;
        if ((wave_archive & kSingleWaves) == 0)
        {
            Link(at, slot, LoadFile(archive, id));
            continue;
        }

        // Load the waves that the bank's PCM instruments play from this slot, in the order of the instruments.
        const uint32_t table = LoadWaveTable(archive, id);
        const auto load = [&](const Region& region)
        {
            if (region.type == InstrumentType::kPcm && region.wave_archive == slot)
            {
                LoadWave(archive, id, table, region.wave);
            }
        };
        for (const Instrument& instrument : parsed.instruments)
        {
            if (instrument.type == InstrumentType::kDrumSet || instrument.type == InstrumentType::kKeySplit)
            {
                for (const Region& region : instrument.regions)
                {
                    load(region);
                }
            }
            else
            {
                load(instrument.region);
            }
        }
        Link(at, slot, table);
    }

    return at;
}

uint32_t SoundHeap::End() const
{
    return next_;
}

uint32_t SoundHeap::Allocate(std::size_t size)
{
    const uint32_t at = next_;
    if (size > end_ - at)
    {
        throw HeapFullError();
    }

    next_ = static_cast<uint32_t>((at + size + 31) & ~std::size_t{31});
    if (next_ > end_)
    {
        next_ = end_;
    }
    return at;
}

uint32_t SoundHeap::LoadWaveTable(const Archive& archive, uint32_t id)
{
    if (const auto found = files_.find(id); found != files_.end())
    {
        return found->second;
    }

    // The header with the table, then the table again. The first copy becomes the waves' addresses.
    const std::span<const uint8_t> file = archive.File(id);
    const xsflib::Reader r(file);
    const uint32_t count = r.U32(kWaveCount);
    const std::span<const uint8_t> header = r.Sub(0, kWaveTable + std::size_t{4} * count);
    const uint32_t at = Allocate(header.size() + std::size_t{4} * count);
    std::memcpy(&memory_[at - base_], header.data(), header.size());
    std::memcpy(&memory_[at - base_ + header.size()], &header[kWaveTable], std::size_t{4} * count);
    std::memset(&memory_[at - base_ + kWaveTable], 0, std::size_t{4} * count);
    files_[id] = at;
    return at;
}

void SoundHeap::LoadWave(const Archive& archive, uint32_t id, uint32_t table, uint32_t wave)
{
    const uint32_t count = Read32(table + kWaveCount);
    if (wave >= count || Read32(table + kWaveTable + 4 * wave) != 0)
    {
        return;
    }

    // A wave runs to the next wave's offset, and the last to the file's end.
    const uint32_t offsets = table + kWaveTable + 4 * count;
    const uint32_t offset = Read32(offsets + 4 * wave);
    const uint32_t end = wave + 1 < count ? Read32(offsets + 4 * (wave + 1)) : Read32(table + kWaveArchiveSize);
    const std::span<const uint8_t> file = archive.File(id);
    if (end < offset || end > file.size())
    {
        throw xsflib::FormatError("wave " + std::to_string(wave) + " of a wave archive lies outside its file");
    }

    const uint32_t at = Allocate(end - offset);
    std::memcpy(&memory_[at - base_], &file[offset], end - offset);
    Write32(table + kWaveTable + 4 * wave, at);
}

void SoundHeap::Link(uint32_t bank, uint32_t slot, uint32_t wave_archive)
{
    // As SND_AssignWaveArc: unlink the slot from the wave archive that it names, then put it first in the new one's
    // list.
    const uint32_t link = bank + kBankLinks + 8 * slot;
    const uint32_t old = Read32(link);
    if (old == wave_archive)
    {
        return;
    }
    if (old != 0)
    {
        if (Read32(old + kWaveArchiveFirstLink) == link)
        {
            Write32(old + kWaveArchiveFirstLink, Read32(link + 4));
        }
        else
        {
            for (uint32_t other = Read32(old + kWaveArchiveFirstLink); other != 0; other = Read32(other + 4))
            {
                if (Read32(other + 4) == link)
                {
                    Write32(other + 4, Read32(link + 4));
                    break;
                }
            }
        }
    }

    Write32(link + 4, Read32(wave_archive + kWaveArchiveFirstLink));
    Write32(link, wave_archive);
    Write32(wave_archive + kWaveArchiveFirstLink, link);
}

uint32_t SoundHeap::Read32(uint32_t address) const
{
    return xsflib::LoadLe32(&memory_[address - base_]);
}

void SoundHeap::Write32(uint32_t address, uint32_t value)
{
    xsflib::StoreLe32(&memory_[address - base_], value);
}

} // namespace ntdlib::sdat

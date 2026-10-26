// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/wsd_file.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/wave_file.h"
#include "rsar/file_blocks.h"
#include "rsar/wave_infos.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar
{
namespace
{

constexpr std::size_t kReferenceSize = 8;
constexpr std::size_t kSoundSize = 3 * kReferenceSize;
constexpr std::size_t kSettingsSize = 0x20;
constexpr std::size_t kEventSize = 0x10;
constexpr std::size_t kNoteSize = 0x30;
constexpr uint16_t kWaveBlockVersion = 0x0102;
constexpr uint16_t kArchiveVersion = 0x0103;

using detail::BlockOf;
using detail::CloseBlock;
using detail::CloseFile;
using detail::Header;
using detail::kBlockHeader;
using detail::OpenBlock;
using detail::VersionOf;
using sar::detail::Out;

xsflib::FormatError OtherLayout(const std::string& what)
{
    return xsflib::FormatError("a wave sound file that the tools didn't lay out: " + what);
}

// The reader of DATA's structures, from the number of wave sounds at `base`.
class DataReader
{
public:
    DataReader(const xsflib::Reader& r, std::size_t base) : r_(r), base_(base)
    {
    }

    // Where the structure lies that the reference at `at` names.
    std::size_t Follow(std::size_t at) const
    {
        if (r_.U8(at) != 1 || r_.U8(at + 1) != 0 || r_.U16(at + 2) != 0)
        {
            throw OtherLayout("a reference of another kind");
        }

        return base_ + r_.U32(at + 4);
    }

    // Where the entries lie that the table at `at` names.
    std::vector<std::size_t> Table(std::size_t at) const
    {
        const uint32_t count = r_.U32(at);
        if (count > r_.Size() / kReferenceSize)
        {
            throw xsflib::FormatError("an RWSD file's table lists more entries than the file can hold");
        }
        std::vector<std::size_t> entries;
        for (uint32_t i = 0; i < count; ++i)
        {
            entries.push_back(Follow(at + 4 + i * kReferenceSize));
        }

        return entries;
    }

    WsdFile::WaveSound Sound(std::size_t at) const
    {
        WsdFile::WaveSound sound;
        const std::size_t settings = Follow(at);
        sound.pitch = r_.F32(settings);
        sound.pan = r_.U8(settings + 4);
        sound.surround_pan = r_.U8(settings + 5);
        for (std::size_t bus = 0; bus < sound.fx_sends.size(); ++bus)
        {
            sound.fx_sends[bus] = r_.U8(settings + 6 + bus);
        }
        sound.main_send = r_.U8(settings + 9);

        for (const std::size_t track : Table(Follow(at + kReferenceSize)))
        {
            WsdFile::Track& read = sound.tracks.emplace_back();
            for (const std::size_t event : Table(Follow(track)))
            {
                read.events.push_back({r_.F32(event), r_.F32(event + 4), r_.U32(event + 8)});
            }
        }
        for (const std::size_t note : Table(Follow(at + 2 * kReferenceSize)))
        {
            WsdFile::Note& read = sound.notes.emplace_back();
            read.wave_index = r_.S32(note);
            read.attack = r_.U8(note + 4);
            read.decay = r_.U8(note + 5);
            read.sustain = r_.U8(note + 6);
            read.release = r_.U8(note + 7);
            read.hold = r_.U8(note + 8);
            read.original_key = r_.U8(note + 0xC);
            read.volume = r_.U8(note + 0xD);
            read.pan = r_.U8(note + 0xE);
            read.surround_pan = r_.U8(note + 0xF);
            read.pitch = r_.F32(note + 0x10);
        }

        return sound;
    }

private:
    const xsflib::Reader& r_;
    std::size_t base_ = 0;
};

// The writer of DATA's structures in pre-order, from the number of wave sounds at `base`.
class DataWriter
{
public:
    DataWriter(Out& out, std::size_t base) : out_(out), base_(base)
    {
    }

    // Puts a reference at `at` to the structure at `target`.
    void Name(std::size_t at, std::size_t target)
    {
        out_.Reference(at, 0x0100, static_cast<uint32_t>(target - base_));
    }

    // Appends a table of `count` entries, and returns where its references start.
    std::size_t Table(std::size_t count)
    {
        const std::size_t at = out_.Zeros(4 + count * kReferenceSize);
        out_.U32(at, static_cast<uint32_t>(count));

        return at + 4;
    }

    void Sound(std::size_t ref_at, const WsdFile::WaveSound& sound)
    {
        const std::size_t at = out_.Zeros(kSoundSize);
        Name(ref_at, at);

        const std::size_t settings = out_.Zeros(kSettingsSize);
        Name(at, settings);
        out_.U32(settings, std::bit_cast<uint32_t>(sound.pitch));
        out_.U8(settings + 4, sound.pan);
        out_.U8(settings + 5, sound.surround_pan);
        for (std::size_t bus = 0; bus < sound.fx_sends.size(); ++bus)
        {
            out_.U8(settings + 6 + bus, sound.fx_sends[bus]);
        }
        out_.U8(settings + 9, sound.main_send);

        Name(at + kReferenceSize, out_.Size());
        const std::size_t tracks = Table(sound.tracks.size());
        for (std::size_t t = 0; t < sound.tracks.size(); ++t)
        {
            const std::size_t track = out_.Zeros(kReferenceSize);
            Name(tracks + t * kReferenceSize, track);
            Name(track, out_.Size());
            const std::vector<WsdFile::NoteEvent>& events = sound.tracks[t].events;
            const std::size_t refs = Table(events.size());
            for (std::size_t e = 0; e < events.size(); ++e)
            {
                const std::size_t event = out_.Zeros(kEventSize);
                Name(refs + e * kReferenceSize, event);
                out_.U32(event, std::bit_cast<uint32_t>(events[e].position));
                out_.U32(event + 4, std::bit_cast<uint32_t>(events[e].length));
                out_.U32(event + 8, events[e].note);
            }
        }

        Name(at + 2 * kReferenceSize, out_.Size());
        const std::size_t notes = Table(sound.notes.size());
        for (std::size_t n = 0; n < sound.notes.size(); ++n)
        {
            const WsdFile::Note& note = sound.notes[n];
            const std::size_t place = out_.Zeros(kNoteSize);
            Name(notes + n * kReferenceSize, place);
            out_.U32(place, static_cast<uint32_t>(note.wave_index));
            out_.U8(place + 4, note.attack);
            out_.U8(place + 5, note.decay);
            out_.U8(place + 6, note.sustain);
            out_.U8(place + 7, note.release);
            out_.U8(place + 8, note.hold);
            out_.U8(place + 0xC, note.original_key);
            out_.U8(place + 0xD, note.volume);
            out_.U8(place + 0xE, note.pan);
            out_.U8(place + 0xF, note.surround_pan);
            out_.U32(place + 0x10, std::bit_cast<uint32_t>(note.pitch));
        }
    }

private:
    Out& out_;
    std::size_t base_ = 0;
};

} // namespace

WsdFile WsdFile::Read(std::span<const uint8_t> file, std::span<const uint8_t> wave_data)
{
    const xsflib::Reader r(file, true);
    WsdFile wsd;
    wsd.version = VersionOf(r, "RWSD");
    if (wsd.version != kWaveBlockVersion && wsd.version != kArchiveVersion)
    {
        throw xsflib::FormatError("an RWSD file of a version other than 1.2 and 1.3");
    }

    const auto [data, data_size] = BlockOf(r, 0, "DATA", "RWSD");
    const DataReader reader(r, data + kBlockHeader);
    for (const std::size_t sound : reader.Table(data + kBlockHeader))
    {
        wsd.sounds.push_back(reader.Sound(sound));
    }

    // The waves: in WAVE over the raw samples, or in the RWAR.
    if (wsd.version == kWaveBlockVersion)
    {
        const auto [wave, wave_size] = BlockOf(r, 1, "WAVE", "RWSD");
        const uint32_t count = r.U32(wave + kBlockHeader);
        if (count > file.size() / 4)
        {
            throw xsflib::FormatError("an RWSD file lists more waves than it can hold");
        }
        std::vector<std::size_t> infos;
        for (uint32_t i = 0; i < count; ++i)
        {
            infos.push_back(wave + r.U32(wave + kBlockHeader + 4 + i * std::size_t{4}));
        }
        wsd.waves = detail::ReadRawWaves(r, infos, wave_data);
    }
    else
    {
        for (const std::vector<uint8_t>& rwav : WaveArchiveFile::Read(wave_data).waves)
        {
            wsd.waves.push_back(WaveFile::Read(rwav));
        }
    }

    Files written;
    try
    {
        written = wsd.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw OtherLayout(error.what());
    }
    if (!std::ranges::equal(written.file, file))
    {
        throw OtherLayout("Write gives other bytes of the file");
    }
    if (!std::ranges::equal(written.wave_data, wave_data))
    {
        throw OtherLayout("Write gives other bytes of the wave data");
    }

    return wsd;
}

WsdFile::Files WsdFile::Write() const
{
    if (version != kWaveBlockVersion && version != kArchiveVersion)
    {
        throw std::invalid_argument("a wave sound file of a version other than 1.2 and 1.3");
    }

    Out out = Header("RWSD", version, version == kWaveBlockVersion ? 2 : 1);
    const std::size_t data = OpenBlock(out, "DATA");
    const std::size_t base = out.Size();
    DataWriter writer(out, base);
    const std::size_t refs = writer.Table(sounds.size());
    for (std::size_t s = 0; s < sounds.size(); ++s)
    {
        writer.Sound(refs + s * kReferenceSize, sounds[s]);
    }
    CloseBlock(out, data, 0);

    Files files;
    if (version == kWaveBlockVersion)
    {
        const detail::RawWaves raw = detail::LayRawWaves(waves);
        const std::size_t wave = OpenBlock(out, "WAVE");
        const std::size_t table = out.Zeros(4 + waves.size() * 4);
        out.U32(table, static_cast<uint32_t>(waves.size()));
        for (std::size_t i = 0; i < waves.size(); ++i)
        {
            const std::size_t info = out.Zeros(detail::WaveInfoSize(waves[i].encoding, waves[i].channels.size()));
            out.U32(table + 4 + i * 4, static_cast<uint32_t>(info - wave));
            detail::PutWaveInfo(out, info, waves[i], raw.places[i], raw.channel_offsets[i]);
        }
        CloseBlock(out, wave, 1);
        files.wave_data = raw.wave_data;
    }
    else
    {
        WaveArchiveFile archive;
        for (const WaveFile& wave : waves)
        {
            archive.waves.push_back(wave.Write());
        }
        files.wave_data = archive.Write();
    }
    files.file = CloseFile(out);

    return files;
}

} // namespace ntdlib::rsar

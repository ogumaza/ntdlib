// SPDX-License-Identifier: MIT

#include "ntdlib/sar/wave_sound_edit.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/reference.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kInfoBlock = 0x6800;
constexpr uint16_t kWaveTable = 0x0100;
constexpr uint16_t kSoundTable = 0x0101;
constexpr std::size_t kFileSizeAt = 0x0C;
constexpr std::size_t kBlockCountAt = 0x10;
constexpr std::size_t kBlocksAt = 0x14; // the table of blocks: a type, 2 bytes, an offset and a size for each
constexpr std::size_t kBlockEntry = 12;
constexpr std::size_t kWaveEntry = 8; // a wave archive's item ID and a wave's index
constexpr uint32_t kFiltersVersion = 0x00010100;
constexpr int kPanBit = 0;
constexpr int kPitchBit = 1;
constexpr int kFiltersBit = 2;
constexpr int kSendsBit = 8;
constexpr int kEnvelopeBit = 9;
constexpr std::size_t kMostAuxSends = 3;
constexpr std::size_t kMostCtrAuxSends = 2;

// A wave sound file's places.
struct Places
{
    std::size_t info_size_at = 0; // INFO's size in the table of blocks
    std::size_t info = 0;
    std::size_t sounds_reference = 0; // the INFO body's reference to the table of wave sounds
    std::size_t waves = 0;
    std::size_t sounds = 0;
};

// Checks `file` and finds its places.
Places Open(std::span<const uint8_t> file, Flavor flavor)
{
    Options options;
    options.flavor = flavor;
    options.rules = Rules::kStopAtFirstProblem;
    WaveSoundFile::Parse(file, options);

    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    Places places;
    for (std::size_t i = 0; i < r.U16(kBlockCountAt); ++i)
    {
        const std::size_t entry = kBlocksAt + i * kBlockEntry;
        if (r.U16(entry) == kInfoBlock)
        {
            places.info = r.U32(entry + 4);
            places.info_size_at = entry + 8;
        }
    }
    const std::size_t body = places.info + 8;
    for (std::size_t i = 0; i < 2; ++i)
    {
        const Reference reference = ReadRef(r, body + i * 8);
        if (reference.type == kWaveTable)
        {
            places.waves = body + reference.offset;
        }
        else if (reference.type == kSoundTable)
        {
            places.sounds = body + reference.offset;
            places.sounds_reference = body + i * 8;
        }
    }

    return places;
}

// Where wave sound `sound`'s settings start.
std::size_t SettingsOf(const xsflib::Reader& r, const Places& places, std::size_t sound)
{
    if (sound >= r.U32(places.sounds))
    {
        throw std::invalid_argument("a wave sound past the file's table");
    }
    const std::size_t at = places.sounds + ReadRef(r, places.sounds + 4 + sound * 8).offset;

    return at + ReadRef(r, at).offset;
}

// Where the value of option `bit` lies in the settings at `settings`, or 0 without the option.
std::size_t ValueOf(const xsflib::Reader& r, std::size_t settings, int bit)
{
    const uint32_t bits = r.U32(settings);
    if ((bits >> bit & 1) == 0)
    {
        return 0;
    }

    return settings + 4 + 4 * static_cast<std::size_t>(std::popcount(bits & ((1u << bit) - 1)));
}

// Stores `value` at `at` in the file's byte order.
void Store32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value, bool big_endian)
{
    big_endian ? xsflib::StoreBe32(&bytes[at], value) : xsflib::StoreLe32(&bytes[at], value);
}

// Refuses a value that has no place and differs from the value that the reader gives without it.
template <typename T>
void CheckDefault(const T& value, const T& without, const char* what)
{
    if (!(value == without))
    {
        throw std::invalid_argument(std::string("a wave sound without a place for its ") + what);
    }
}

} // namespace

WaveSoundPlaces WaveSoundValuePlaces(std::span<const uint8_t> file, Flavor flavor, std::size_t sound)
{
    const Places places = Open(file, flavor);
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    const std::size_t settings = SettingsOf(r, places, sound);

    // The 3DS's reader reads no filters, and 2 aux sends at most (WaveSoundFile::Parse).
    WaveSoundPlaces result;
    const bool ctr = flavor == Flavor::kCtr;
    result.pan = ValueOf(r, settings, kPanBit) != 0;
    result.pitch = ValueOf(r, settings, kPitchBit) != 0;
    result.filters = !ctr && ValueOf(r, settings, kFiltersBit) != 0 && r.U32(0x08) >= kFiltersVersion;
    result.envelope = ValueOf(r, settings, kEnvelopeBit) != 0;
    if (const std::size_t at = ValueOf(r, settings, kSendsBit); at != 0)
    {
        result.sends = true;
        const std::size_t most = ctr ? kMostCtrAuxSends : kMostAuxSends;
        result.aux_sends = static_cast<uint8_t>(std::min<std::size_t>(r.U8(settings + r.U32(at) + 1), most));
    }

    return result;
}

std::vector<uint8_t> WithWaveSoundValues(std::span<const uint8_t> file, Flavor flavor, std::size_t sound,
                                         const WaveSoundFile::WaveSound& values)
{
    const Places places = Open(file, flavor);
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(file, big_endian);
    const std::size_t settings = SettingsOf(r, places, sound);
    const WaveSoundPlaces has = WaveSoundValuePlaces(file, flavor, sound);
    const WaveSoundFile::WaveSound without;
    std::vector<uint8_t> bytes(file.begin(), file.end());

    // The pan and the surround pan share a value. The three values of the filters share another. Their other bytes
    // stay.
    if (has.pan)
    {
        const std::size_t at = ValueOf(r, settings, kPanBit);
        const uint32_t kept = r.U32(at) & 0xFFFF0000u;
        Store32(bytes, at, kept | values.pan | uint32_t{values.span} << 8, big_endian);
    }
    else
    {
        CheckDefault(values.pan, without.pan, "pan");
        CheckDefault(values.span, without.span, "surround pan");
    }
    if (has.pitch)
    {
        Store32(bytes, ValueOf(r, settings, kPitchBit), std::bit_cast<uint32_t>(values.pitch), big_endian);
    }
    else
    {
        CheckDefault(values.pitch, without.pitch, "pitch");
    }
    if (has.filters)
    {
        const std::size_t at = ValueOf(r, settings, kFiltersBit);
        const uint32_t filters =
            values.low_pass | uint32_t{values.biquad_type} << 8 | uint32_t{values.biquad_value} << 16;
        Store32(bytes, at, (r.U32(at) & 0xFF000000u) | filters, big_endian);
    }
    else
    {
        CheckDefault(values.low_pass, without.low_pass, "low-pass filter");
        CheckDefault(values.biquad_type, without.biquad_type, "biquad filter");
        CheckDefault(values.biquad_value, without.biquad_value, "biquad filter");
    }

    // The sends: the main send, then the aux sends that the file holds. Each aux send past them reads as 0.
    const std::size_t sends = has.sends ? settings + r.U32(ValueOf(r, settings, kSendsBit)) : 0;
    if (has.sends)
    {
        bytes[sends] = values.main_send;
    }
    else
    {
        CheckDefault(values.main_send, without.main_send, "main send");
    }
    for (std::size_t bus = 0; bus < kMostAuxSends; ++bus)
    {
        if (bus < has.aux_sends)
        {
            bytes[sends + 2 + bus] = values.aux_sends[bus];
        }
        else
        {
            CheckDefault(values.aux_sends[bus], uint8_t{0}, "aux send");
        }
    }

    if (has.envelope)
    {
        const std::size_t reference = settings + r.U32(ValueOf(r, settings, kEnvelopeBit));
        const std::size_t at = reference + ReadRef(r, reference).offset;
        const Adshr& adshr = values.adshr;
        const uint8_t envelope[] = {adshr.attack, adshr.decay, adshr.sustain, adshr.hold, adshr.release};
        std::copy(std::begin(envelope), std::end(envelope), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    }
    else
    {
        CheckDefault(values.adshr, without.adshr, "envelope");
    }

    return bytes;
}

std::vector<uint8_t> WithWaveSoundWave(std::span<const uint8_t> file, Flavor flavor, std::size_t sound,
                                       std::size_t note, WaveId wave)
{
    const Places places = Open(file, flavor);
    const bool big_endian = flavor == Flavor::kCafe;
    const xsflib::Reader r(file, big_endian);
    const uint32_t entries = r.U32(places.waves);
    const std::size_t table_end = places.waves + 4 + entries * kWaveEntry;
    const std::size_t info_size = r.U32(places.info_size_at);
    if (places.sounds != table_end || places.info + info_size != file.size())
    {
        throw std::invalid_argument("a wave sound file that NintendoWare's tools didn't lay out");
    }
    SettingsOf(r, places, sound);

    // Each note's index and its wave, in the order of the sounds and their notes, with the note's new wave.
    struct Note
    {
        std::size_t index_at = 0;
        WaveId wave;
    };
    std::vector<Note> notes;
    bool found = false;
    for (std::size_t i = 0; i < r.U32(places.sounds); ++i)
    {
        const std::size_t at = places.sounds + ReadRef(r, places.sounds + 4 + i * 8).offset;
        const std::size_t table = at + ReadRef(r, at + 16).offset;
        for (std::size_t k = 0; k < r.U32(table); ++k)
        {
            const std::size_t index_at = table + ReadRef(r, table + 4 + k * 8).offset;
            if (index_at < table_end)
            {
                throw std::invalid_argument("a wave sound file that NintendoWare's tools didn't lay out");
            }
            const uint32_t index = r.U32(index_at);
            if (index >= entries)
            {
                throw xsflib::FormatError("a wave sound's note names a wave past the table");
            }
            const std::size_t entry = places.waves + 4 + index * kWaveEntry;
            const bool target = i == sound && k == note;
            notes.push_back({index_at, target ? wave : WaveId{r.U32(entry), r.U32(entry + 4)}});
            found = found || target;
        }
    }
    if (!found)
    {
        throw std::invalid_argument("a note past the wave sound's table");
    }

    // The new table lists the notes' waves in the order of their first use.
    std::vector<WaveId> table;
    std::vector<uint32_t> indices;
    for (const Note& entry : notes)
    {
        const auto place = std::find(table.begin(), table.end(), entry.wave);
        indices.push_back(static_cast<uint32_t>(place - table.begin()));
        if (place == table.end())
        {
            table.push_back(entry.wave);
        }
    }

    // The bytes before the table, the new table and the bytes after it. The references after the table count from
    // places after it.
    const std::ptrdiff_t delta =
        (static_cast<std::ptrdiff_t>(table.size()) - entries) * static_cast<std::ptrdiff_t>(kWaveEntry);
    std::vector<uint8_t> bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(places.waves + 4));
    bytes.resize(bytes.size() + table.size() * kWaveEntry);
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        Store32(bytes, places.waves + 4 + i * kWaveEntry, table[i].wave_archive_item, big_endian);
        Store32(bytes, places.waves + 8 + i * kWaveEntry, table[i].index, big_endian);
    }
    bytes.insert(bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(table_end), file.end());
    const auto moved = [delta](std::size_t at)
    {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + delta);
    };

    Store32(bytes, places.waves, static_cast<uint32_t>(table.size()), big_endian);
    Store32(bytes, places.sounds_reference + 4, static_cast<uint32_t>(moved(places.sounds) - (places.info + 8)),
            big_endian);
    const auto new_info_size = static_cast<uint32_t>(static_cast<std::ptrdiff_t>(info_size) + delta);
    Store32(bytes, places.info_size_at, new_info_size, big_endian);
    Store32(bytes, places.info + 4, new_info_size, big_endian);
    Store32(bytes, kFileSizeAt, static_cast<uint32_t>(bytes.size()), big_endian);
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        Store32(bytes, moved(notes[i].index_at), indices[i], big_endian);
    }

    return bytes;
}

} // namespace ntdlib::sar

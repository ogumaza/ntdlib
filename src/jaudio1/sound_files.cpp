// SPDX-License-Identifier: MIT

// The sound files of the game: the start-up archive, the sequences, the wave archives, the banks and the FX
// scenes (see engine.cpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "jaudio1/engine_internal.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/jaudio1/engine.h"
#include "ntdlib/jaudio1/instruments.h"
#include "ntdlib/jaudio2/channel.h"
#include "ntdlib/jaudio2/engine.h"

namespace ntdlib::jaudio1
{

namespace
{

namespace nj = ntdlib::jaudio;

// JASDsp's table of sends (0x944b96 in the release's main). An FX scene names each of a line's two buses by an index
// into it.
constexpr std::array<uint16_t, 12> kSends = {0x0d00, 0x0d60, 0x0dc0, 0x0e20, 0x0e80, 0x0ee0,
                                             0x0ca0, 0x0f40, 0x0fa0, 0x0b00, 0x09a0, 0x0000};

// The sound table (JAInter::SoundTable): the formats and the version in its first 4 bytes, the categories' counts and
// first places from byte 6, and the sounds from kSoundInfos. Categories 0 to 15 hold effects
// (SoundFiles::kEffectCategories), 16 the sequences and 17 the streams.
constexpr std::size_t kSoundInfos = 0x50;
constexpr uint32_t kSoundCategories = 18;

// Whether the start-up archive is little-endian: its first word is a block's type in that order.
bool LittleEndianAaf(std::span<const uint8_t> aaf)
{
    if (aaf.size() < 4)
    {
        return false;
    }

    const uint32_t first = xsflib::LoadLe32(aaf.data());
    return first >= 1 && first <= 8;
}

// The start-up archive's blocks that the engine reads: the sound table (type 1), the effects' scenes (type 6) and the
// FX scenes (type 7). Each type word is followed by three words for a block: its offset, its size and a value. The
// lists of banks (type 2) and wave systems (type 3) give blocks up to a 0 word. A little-endian archive widens the
// offsets of the lists' entries and of a block of type 5 to 64 bits.
struct AafBlocks
{
    std::span<const uint8_t> sound_table;
    std::span<const uint8_t> effect_scenes;
    std::span<const uint8_t> fx_scenes;
};

AafBlocks FindAafBlocks(std::span<const uint8_t> aaf)
{
    const bool little = LittleEndianAaf(aaf);
    const xsflib::Reader r(aaf, !little);
    const std::size_t entry = little ? 16 : 12;
    AafBlocks blocks;
    for (std::size_t at = 0;;)
    {
        const uint32_t type = r.U32(at);
        at += 4;
        if (type == 0)
        {
            return blocks;
        }

        if (type == 2 || type == 3)
        {
            while (r.U32(at) != 0)
            {
                at += entry;
            }
            at += 4;
        }
        else if (type <= 8)
        {
            if (type == 1)
            {
                blocks.sound_table = r.Sub(r.U32(at), r.U32(at + 4));
            }
            else if (type == 6)
            {
                blocks.effect_scenes = r.Sub(r.U32(at), r.U32(at + 4));
            }
            else if (type == 7)
            {
                blocks.fx_scenes = r.Sub(r.U32(at), r.U32(at + 4));
            }
            at += type == 5 ? entry : 12;
        }
        else
        {
            while (r.U32(at) != 0)
            {
                at += 4;
            }
            at += 4;
        }
    }
}

} // namespace

SoundFiles::SoundFiles(std::span<const uint8_t> aaf, std::span<const uint8_t> sequences, const WaveArchives& waves)
    : aaf_(aaf.begin(), aaf.end())
{
    if (!nj::IsAaf(aaf_))
    {
        throw xsflib::FormatError("the start-up archive isn't an AAF");
    }

    std::vector<uint8_t> decoded;
    for (const nj::ArchiveFile& file : nj::ReadRarc(nj::DecodeIfYaz0(sequences, decoded), nj::kWideRarcEntrySize))
    {
        SequenceFile& entry = sequences_.emplace_back();
        entry.id = file.id;
        entry.name = file.name;
        entry.sequence = std::make_unique<Sequence>(std::vector<uint8_t>(file.data.begin(), file.data.end()));
    }

    const AafBlocks blocks = FindAafBlocks(aaf_);
    if (blocks.sound_table.size() < kSoundInfos)
    {
        throw xsflib::FormatError("the start-up archive has no sound table");
    }
    sound_table_.assign(blocks.sound_table.begin(), blocks.sound_table.end());
    sequence_format_ = sound_table_[1];

    LoadWaves(waves);
    LoadBanks();
    LoadFxLines(blocks.fx_scenes);
    LoadEffectScenes(blocks.effect_scenes);
}

uint8_t SoundFiles::EffectFormat() const
{
    return sound_table_[0];
}

uint16_t SoundFiles::SoundCount(uint32_t category) const
{
    // JAInter::SoundTable::init: each of the 18 categories gives its count and its first sound's place in the table.
    if (category >= kSoundCategories)
    {
        return 0;
    }

    return xsflib::Reader(sound_table_, !LittleEndianAaf(aaf_)).U16(6 + 4 * category);
}

std::optional<SoundInfo> SoundFiles::FindSound(uint32_t id) const
{
    // JAInter::SoundTable::getInfoPointer. The table's sounds are 16 bytes each from kSoundInfos.
    uint32_t category = 0;
    switch (id >> 30)
    {
    case 0:
        category = (id >> 12) & 0xff;
        if (category >= kEffectCategories)
        {
            return std::nullopt;
        }
        break;

    case 2:
        category = kEffectCategories;
        break;

    case 3:
        category = kEffectCategories + 1;
        break;

    default:
        return std::nullopt;
    }

    const uint32_t index = id & 0x3ff;
    if (index >= SoundCount(category))
    {
        return std::nullopt;
    }

    const xsflib::Reader r(sound_table_, !LittleEndianAaf(aaf_));
    const std::size_t at = kSoundInfos + 16 * (static_cast<std::size_t>(r.U16(8 + 4 * category)) + index);
    if (at + 16 > r.Size())
    {
        return std::nullopt;
    }

    SoundInfo info;
    info.flags = r.U32(at);
    info.offset = r.U16(at + 6);
    info.pitch = r.F32(at + 8);
    info.volume = r.U8(at + 0xc);
    info.fxmix = r.U8(at + 0xd);

    return info;
}

const SoundFiles::SequenceFile* SoundFiles::FindSequence(uint16_t id) const
{
    for (const SequenceFile& file : sequences_)
    {
        if (file.id == id)
        {
            return &file;
        }
    }

    return nullptr;
}

void SoundFiles::LoadWaves(const WaveArchives& waves)
{
    // The game plays the copy of a wave in the group that it loaded last (JASBasicWaveBank::incWaveTable). The model
    // plays the copy in the last group that lists the wave. JAudio2 puts the waves at the same place.
    static_assert(kWaves == jaudio2::SoundFiles::kWaves);
    jaudio2::LoadedWaves loaded = jaudio2::LoadWaveSystems(nj::ReadAafWaveSystems(aaf_), waves);
    memory_ = std::move(loaded.memory);
    wave_banks_ = std::move(loaded.banks);
    missing_waves_ = std::move(loaded.missing);
}

void SoundFiles::LoadBanks()
{
    // JAInter::BankWave::init reads every bank of the list, in order. A bank's place in the list is its physical
    // number. The banks' random settings step the generator as they're read.
    std::vector<std::string> warnings;
    const std::vector<nj::Bank> banks = nj::ReadAafBanks(aaf_, warnings);
    if (!warnings.empty())
    {
        throw xsflib::FormatError("the start-up archive has a damaged bank: " + warnings.front());
    }

    for (const nj::Bank& parsed : banks)
    {
        auto bank = std::make_unique<InstrumentBank>(parsed, inst_random_);
        const auto waves = wave_banks_.find(parsed.wave_system);
        bank->SetWaveBank(waves != wave_banks_.end() ? &waves->second : nullptr);
        banks_.push_back(std::move(bank));
    }
}

void SoundFiles::LoadFxLines(std::span<const uint8_t> scenes)
{
    // JAInter::Fx::init. The block of FX scenes gives the number of scenes, each line's buffer in sub-frames, and each
    // scene's offset in the block. A scene gives the four lines' settings in 0x20 bytes each (JASDsp::FxlineConfig_):
    // the flags, the indexes and levels of the two sends, the delay's length and the filter. A line whose buffer is 0
    // stays off. The others take the first scene's settings.
    if (scenes.empty())
    {
        return;
    }

    const xsflib::Reader r(scenes, !LittleEndianAaf(aaf_));
    if (r.U32(0) == 0)
    {
        return;
    }

    const uint32_t scene = r.U32(0x14);
    for (std::size_t line = 0; line < fx_lines_.size(); line++)
    {
        const uint32_t buffer = r.U32(4 + 4 * line);
        if (buffer == 0)
        {
            continue;
        }
        if (buffer > kMaxFxSubframes)
        {
            throw xsflib::FormatError("an FX line's buffer is longer than " + std::to_string(kMaxFxSubframes) +
                                      " sub-frames");
        }

        const std::size_t at = scene + line * kFxLineBytes;
        FxLine& fx = fx_lines_[line];
        fx.buffer_subframes = buffer;
        fx.flags = r.U8(at);
        fx.subframes = static_cast<uint16_t>(std::min(r.U32(at + 0xc), buffer));
        for (std::size_t send = 0; send < 2; send++)
        {
            const uint16_t index = r.U16(at + 2 + 4 * send);
            if (index >= kSends.size())
            {
                throw xsflib::FormatError("an FX line sends to a bus past the end of the table of sends");
            }
            fx.buses[send] = kSends[index];
            fx.levels[send] = r.S16(at + 4 + 4 * send);
        }
        for (std::size_t i = 0; i < fx.filter.size(); i++)
        {
            fx.filter[i] = r.S16(at + 0x10 + 2 * i);
        }
    }
}

void SoundFiles::LoadEffectScenes(std::span<const uint8_t> scenes)
{
    // JAInter::InitData's block of type 6: the number of scenes, then each scene's offset in the block (64 bits in a
    // little-endian archive). A scene gives 2 bytes for each category of effects up to the last with sounds: the
    // category's slot tracks (JAInter::SeMgr's categoryInfoTable), then the most effects that it keeps at once.
    if (scenes.empty())
    {
        return;
    }

    const bool little = LittleEndianAaf(aaf_);
    const xsflib::Reader r(scenes, !little);
    uint32_t categories = 0;
    for (uint32_t category = 0; category < kEffectCategories; category++)
    {
        if (SoundCount(category) != 0)
        {
            categories = category + 1;
        }
    }

    const uint32_t count = r.U32(0);
    const std::size_t pointer = little ? 8 : 4;
    for (uint32_t scene = 0; scene < count; scene++)
    {
        const std::size_t at = r.U32(4 + pointer * scene);
        std::vector<uint8_t>& slots = effect_scenes_.emplace_back();
        for (uint32_t category = 0; category < categories; category++)
        {
            slots.push_back(r.U8(at + 2 * category));
        }
    }
}

} // namespace ntdlib::jaudio1

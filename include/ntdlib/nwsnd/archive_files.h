// SPDX-License-Identifier: MIT

// A sound archive's banks, wave archives and waves as the model's engine loads them for its notes, from the archives of
// the Wii U and the Switch. The engine reads each one the first time a note uses it, and keeps it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::nwsnd
{

// A wave archive's file in the sound archive, and the archive read from it. For a console whose voices read samples by
// address, `address` is where its memory holds the file.
struct LoadedWaveArchive
{
    std::span<const uint8_t> file;
    sar::WaveArchive archive;
    uint32_t address = 0;
};

// A sound archive's banks, wave archives and waves. The policy supplies WaveInfo, a wave as the sound library reads it
// from its file: format (an enum with kPcm8, kPcm16 and kDspAdpcm), loop, channel_count, sample_rate, loop_start,
// loop_end, original_loop_start, and two channels, each with coefs, context and loop_context. The policy's
// PlaceSamples(channel, samples, archive) sets where the console's voices read a channel's samples.
template <typename Policy>
class ArchiveFiles
{
public:
    using WaveInfo = typename Policy::WaveInfo;

    // What a note plays: its velocity region and the region's wave.
    struct Note
    {
        const sar::VelocityRegion* region = nullptr;
        WaveInfo wave;
    };

    // `loaded` is called with each wave archive when it's first read, before any of its waves are read.
    ArchiveFiles(const sar::SoundArchive& archive, const sar::Options& options,
                 std::function<void(LoadedWaveArchive&)> loaded = {})
        : archive_(&archive), options_(options), loaded_(std::move(loaded))
    {
    }

    // The bank of item `bank_item`. Null if the item isn't one of the archive's banks, or if the archive lacks the
    // bank's file.
    const sar::Bank* Bank(uint32_t bank_item)
    {
        if (const auto it = banks_.find(bank_item); it != banks_.end())
        {
            return it->second.get();
        }

        const std::optional<std::size_t> index = sar::ItemIndex(bank_item, sar::kBankItem, archive_->Banks().size());
        if (!index)
        {
            return nullptr;
        }

        const std::span<const uint8_t> file = archive_->FileData(archive_->Banks()[*index].file_id);
        if (file.empty())
        {
            return nullptr;
        }

        return banks_.emplace(bank_item, std::make_unique<const sar::Bank>(sar::Bank::Parse(file, options_)))
            .first->second.get();
    }

    // The wave archive of item `wave_archive_item`, in the same way.
    const LoadedWaveArchive* WaveArchive(uint32_t wave_archive_item)
    {
        if (const auto it = wave_archives_.find(wave_archive_item); it != wave_archives_.end())
        {
            return it->second.get();
        }

        const std::optional<std::size_t> index =
            sar::ItemIndex(wave_archive_item, sar::kWaveArchiveItem, archive_->WaveArchives().size());
        if (!index)
        {
            return nullptr;
        }

        const std::span<const uint8_t> file = archive_->FileData(archive_->WaveArchives()[*index].file_id);
        if (file.empty())
        {
            return nullptr;
        }

        auto loaded = std::make_unique<LoadedWaveArchive>();
        loaded->file = file;
        loaded->archive = sar::WaveArchive::Parse(file, options_);
        if (loaded_)
        {
            loaded_(*loaded);
        }

        return wave_archives_.emplace(wave_archive_item, std::move(loaded)).first->second.get();
    }

    // Wave `index` of the wave archive of item `wave_archive_item` in the form that WaveFileReader::ReadWaveInfo reads:
    // PCM8 and PCM16 by their codes, and any other code as DSP-ADPCM. Nothing if the wave archive or the wave is
    // missing, or if the wave has no channels.
    std::optional<WaveInfo> Wave(uint32_t wave_archive_item, uint32_t index)
    {
        const auto key = std::make_pair(wave_archive_item, index);
        if (const auto it = waves_.find(key); it != waves_.end())
        {
            return it->second;
        }

        std::optional<WaveInfo> result;
        const LoadedWaveArchive* loaded = WaveArchive(wave_archive_item);
        if (loaded != nullptr && index < loaded->archive.waves.size())
        {
            const sar::Wave wave = sar::Wave::Parse(loaded->archive.waves[index], options_);
            if (!wave.channels.empty())
            {
                using SampleFormat = decltype(WaveInfo::format);
                WaveInfo info;
                switch (wave.encoding)
                {
                case sar::WaveEncoding::kPcm8:
                    info.format = SampleFormat::kPcm8;
                    break;
                case sar::WaveEncoding::kPcm16:
                    info.format = SampleFormat::kPcm16;
                    break;
                default:
                    info.format = SampleFormat::kDspAdpcm;
                    break;
                }

                info.loop = wave.loop;
                info.channel_count = static_cast<int>(wave.channels.size());
                info.sample_rate = wave.sample_rate;
                info.loop_start = wave.loop_start;
                info.loop_end = wave.loop_end;
                info.original_loop_start = wave.loop_start;
                for (std::size_t c = 0; c < std::min<std::size_t>(wave.channels.size(), 2); ++c)
                {
                    const sar::WaveChannel& from = wave.channels[c];
                    auto& to = info.channels[c];
                    Policy::PlaceSamples(to, from.data, *loaded);
                    if (from.adpcm)
                    {
                        to.coefs = from.adpcm->coefs;
                        to.context = {from.adpcm->pred_scale, from.adpcm->yn1, from.adpcm->yn2};
                        to.loop_context = {from.adpcm->loop_pred_scale, from.adpcm->loop_yn1, from.adpcm->loop_yn2};
                    }
                }

                result = info;
            }
        }

        waves_.emplace(key, result);

        return result;
    }

    // What the note of `key` at `velocity` plays, with program `program` of the bank in slot `slot` of `banks`. NoteOn
    // finds it the same way. Nothing if the slot, the bank, the program, the region or the region's wave is missing.
    std::optional<Note> FindNote(const std::vector<uint32_t>& banks, int slot, uint32_t program, int key, int velocity)
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= banks.size())
        {
            return std::nullopt;
        }

        const sar::Bank* bank = Bank(banks[static_cast<std::size_t>(slot)]);
        if (bank == nullptr || program >= bank->instruments.size() || !bank->instruments[program])
        {
            return std::nullopt;
        }

        const sar::VelocityRegion* region = bank->instruments[program]->Find(key, velocity);
        if (region == nullptr || region->wave_id_index >= bank->waves.size())
        {
            return std::nullopt;
        }

        const sar::WaveId& id = bank->waves[region->wave_id_index];
        std::optional<WaveInfo> wave = Wave(id.wave_archive_item, id.index);
        if (!wave)
        {
            return std::nullopt;
        }

        return Note{region, std::move(*wave)};
    }

private:
    const sar::SoundArchive* archive_;
    sar::Options options_;
    std::function<void(LoadedWaveArchive&)> loaded_;
    std::map<uint32_t, std::unique_ptr<const sar::Bank>> banks_;
    std::map<uint32_t, std::unique_ptr<LoadedWaveArchive>> wave_archives_;
    std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves_;
};

// Sets `channel` up for the note of `info` on velocity region `region`, as Bank::NoteOn does before it starts the
// channel (0x022C4A0C on the Wii U, 0xc0b140 on the Switch). The channel takes the key, the velocity's volume by the
// policy, and the region's volume, tuning, envelope, pan, key group, note-off type and interpolation.
template <typename Policy, typename Channel, typename NoteOnInfo>
void SetUpNoteChannel(Channel& channel, const sar::VelocityRegion& region, const NoteOnInfo& info)
{
    channel.key = static_cast<uint8_t>(info.key);
    channel.original_key = region.original_key;
    channel.velocity_volume = Policy::VelocityVolume(info.velocity);
    channel.init_volume = static_cast<float>(region.volume) * (1.0f / 127.0f);
    channel.tune = region.pitch;
    channel.envelope.SetAttack(region.adshr.attack);
    channel.envelope.SetHold(region.adshr.hold);
    channel.envelope.SetDecay(region.adshr.decay);
    channel.envelope.SetSustain(region.adshr.sustain);
    channel.envelope.SetRelease(region.adshr.release);
    channel.init_pan = static_cast<float>(info.init_pan + static_cast<int>(region.pan) - 0x40) / 63.0f;
    channel.key_group = region.key_group;
    channel.ignore_note_off = region.ignore_note_off;
    channel.interpolation_type = region.interpolation;
}

} // namespace ntdlib::nwsnd

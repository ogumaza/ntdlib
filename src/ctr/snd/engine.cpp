// SPDX-License-Identifier: MIT

#include "ntdlib/ctr/snd/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/ctr/dsp/fcram.h"
#include "ntdlib/ctr/dsp/firmware_model.h"
#include "ntdlib/ctr/dsp/teakra_dsp.h"
#include "ntdlib/ctr/nnsnd/sound_system.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/ctr/snd/sound_players.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/types.h"

namespace ntdlib::ctr::snd
{
namespace
{

// Reads `wave`, a wave in the wave archive `warc`, into a WaveInfo. nw::snd's WaveFileReader fills one the same way.
// Each channel's data is the address of its samples in FCRAM. Returns nothing for a wave that the DSP can't play: an
// IMA-ADPCM wave, or one without channels.
std::optional<WaveInfo> ReadWaveInfo(const Engine::LoadedWarc& warc, const ntdlib::sar::Wave& wave)
{
    WaveInfo info;
    switch (wave.encoding)
    {
    case ntdlib::sar::WaveEncoding::kPcm8:
        info.format = SampleFormat::kPcm8;
        break;
    case ntdlib::sar::WaveEncoding::kPcm16:
        info.format = SampleFormat::kPcm16;
        break;
    case ntdlib::sar::WaveEncoding::kDspAdpcm:
        info.format = SampleFormat::kDspAdpcm;
        break;
    default: // IMA-ADPCM
        return std::nullopt;
    }

    if (wave.channels.empty())
    {
        return std::nullopt;
    }

    info.loop = wave.loop;
    info.channel_count = static_cast<int>(std::min<std::size_t>(wave.channels.size(), 2));
    info.sample_rate = wave.sample_rate;
    info.loop_start = wave.loop_start;
    info.loop_end = wave.loop_end;
    for (int c = 0; c < info.channel_count; c++)
    {
        const auto& src = wave.channels[c];
        auto& dst = info.channels[c];
        dst.data = warc.base + static_cast<ntdlib::PAddr>(src.data.data() - warc.file.data());
        if (src.adpcm)
        {
            dst.coefs = src.adpcm->coefs;
            dst.context = {src.adpcm->pred_scale, src.adpcm->yn1, src.adpcm->yn2};
            dst.loop_context = {src.adpcm->loop_pred_scale, src.adpcm->loop_yn1, src.adpcm->loop_yn2};
        }
    }

    return info;
}

// The wave that a voice plays for channel `channel` of `stream`, with the channel's samples at `data` in FCRAM: the
// samples from the stream's start, with the stream's format, rate and loop, and the DSP-ADPCM decoder's state at the
// start and at the loop.
WaveInfo StreamChannelWave(const ntdlib::sar::StreamFile& stream, std::size_t channel, ntdlib::PAddr data)
{
    WaveInfo wave;
    switch (stream.encoding)
    {
    case ntdlib::sar::WaveEncoding::kPcm8:
        wave.format = SampleFormat::kPcm8;
        break;
    case ntdlib::sar::WaveEncoding::kPcm16:
        wave.format = SampleFormat::kPcm16;
        break;
    default: // StreamFile::Read refuses IMA-ADPCM
        wave.format = SampleFormat::kDspAdpcm;
        break;
    }

    wave.loop = stream.loop;
    wave.channel_count = 1;
    wave.sample_rate = stream.sample_rate;
    wave.loop_start = stream.loop_start;
    wave.loop_end = stream.sample_count;
    WaveInfo::Channel& to = wave.channels[0];
    to.data = data;
    if (const auto& adpcm = stream.channels[channel].adpcm)
    {
        to.coefs = adpcm->coefs;
        to.context = {adpcm->pred_scale, adpcm->yn1, adpcm->yn2};
        to.loop_context = {adpcm->loop_pred_scale, adpcm->loop_yn1, adpcm->loop_yn2};
    }

    return wave;
}

} // namespace

Engine::Engine(nnsnd::SoundSystem& snd, Fcram& fcram, const ntdlib::sar::SoundArchive& archive, EngineOptions options)
    : snd_(snd),
      fcram_(fcram),
      archive_(archive),
      options_(options),
      voices_(*this),
      channels_(options.enhanced ? kEnhancedChannelCount : kChannelCount)
{
    global_vars_.fill(-1);

    // nw::snd's random number generator advances once a sound frame from the moment the sound system starts. In game
    // mode the game boots and loads the sound first, and by the time the sound's sequence first runs, the generator has
    // taken 17 steps from its seed: 13 before the driver reports the sound playing, and 4 in the frames after. The
    // model's generator starts there too (the policy's kRandomStepsBeforeStart). Sounds with random commands then draw
    // the same numbers in both modes.
    for (int i = 0; i < Policy::kRandomStepsBeforeStart; i++)
    {
        random_.Next();
    }
}

const ntdlib::sar::Bank* Engine::GetBank(uint32_t bank_item)
{
    if (auto it = banks_.find(bank_item); it != banks_.end())
    {
        return it->second.get();
    }

    const std::optional<std::size_t> index =
        ntdlib::sar::ItemIndex(bank_item, ntdlib::sar::kBankItem, archive_.Banks().size());
    if (!index)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(archive_.Banks()[*index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    return banks_
        .emplace(bank_item, std::make_shared<const ntdlib::sar::Bank>(ntdlib::sar::Bank::Parse(file, kSarOptions)))
        .first->second.get();
}

const Engine::LoadedWarc* Engine::GetWarc(uint32_t wave_archive_item)
{
    if (auto it = warcs_.find(wave_archive_item); it != warcs_.end())
    {
        return it->second.get();
    }

    const std::optional<std::size_t> index =
        ntdlib::sar::ItemIndex(wave_archive_item, ntdlib::sar::kWaveArchiveItem, archive_.WaveArchives().size());
    if (!index)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(archive_.WaveArchives()[*index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    // The game loads the whole CWAR into its sound heap; place it in FCRAM the same way so the DSP sees the waves at
    // their real relative positions.
    LoadedWarc w;
    w.file = file;
    w.base = fcram_.Store(file.data(), file.size());
    w.archive = ntdlib::sar::WaveArchive::Parse(file, kSarOptions);

    return warcs_.emplace(wave_archive_item, std::make_shared<const LoadedWarc>(std::move(w))).first->second.get();
}

std::optional<WaveInfo> Engine::GetWave(uint32_t wave_archive_item, uint32_t index)
{
    const auto key = std::make_pair(wave_archive_item, index);
    if (auto it = waves_.find(key); it != waves_.end())
    {
        return it->second;
    }

    std::optional<WaveInfo> result;
    const LoadedWarc* w = GetWarc(wave_archive_item);
    if (w && index < w->archive.waves.size())
    {
        result = ReadWaveInfo(*w, ntdlib::sar::Wave::Parse(w->archive.waves[index], kSarOptions));
    }

    waves_.emplace(key, result);

    return result;
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& bank_items, int bank_index, const NoteOnInfo& info)
{
    // Bank::NoteOn (0x48f954) with BankFileReader::ReadVelocityRegionInfo (0x48e2f8). StartSequence has read the
    // player's banks.
    const bool in_slots = bank_index >= 0 && bank_index < ntdlib::sar::kBankSlots &&
                          static_cast<std::size_t>(bank_index) < bank_items.size();
    const ntdlib::sar::Bank* bank = in_slots ? GetBank(bank_items[static_cast<std::size_t>(bank_index)]) : nullptr;
    if (!bank || info.prg_no >= bank->instruments.size() || !bank->instruments[info.prg_no])
    {
        return nullptr;
    }

    const ntdlib::sar::VelocityRegion* vr = bank->instruments[info.prg_no]->Find(info.key, info.velocity);
    if (!vr || vr->wave_id_index >= bank->waves.size())
    {
        return nullptr;
    }

    const ntdlib::sar::WaveId& id = bank->waves[vr->wave_id_index];
    if (id.index == ntdlib::sar::kNoItem)
    {
        return nullptr;
    }

    const std::optional<WaveInfo> wave = GetWave(id.wave_archive_item, id.index);
    if (!wave)
    {
        return nullptr;
    }

    Channel* ch = channels_.AllocChannel(*this, std::min(wave->channel_count, 2), static_cast<uint32_t>(info.priority),
                                         info.owner_player, info.owner_track);
    if (!ch)
    {
        return nullptr;
    }

    // The 3DS's channel holds the note's whole volume, from its velocity and its region, in velocity_volume (see
    // CtrPolicy::ChannelVolume).
    ch->key = static_cast<uint8_t>(info.key);
    ch->original_key = vr->original_key;
    ch->velocity_volume = static_cast<float>(info.velocity * info.velocity * vr->volume) * Policy::kOneOver127Cubed;
    ch->tune = vr->pitch;
    ch->envelope.SetAttack(vr->adshr.attack);
    ch->envelope.SetHold(vr->adshr.hold);
    ch->envelope.SetDecay(vr->adshr.decay);
    ch->envelope.SetSustain(vr->adshr.sustain);
    ch->envelope.SetRelease(vr->adshr.release);
    ch->init_pan = static_cast<float>(static_cast<int>(vr->pan) + info.init_pan - 64) * Policy::kOneOver63;
    ch->key_group = vr->key_group;
    ch->ignore_note_off = vr->ignore_note_off;
    ch->interpolation_type = vr->interpolation;
    ch->Start(*this, *wave, info.length);

    return ch;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus)
{
    std::visit(
        [&](auto& owner)
        {
            if constexpr (requires { owner.ChannelEvent(*this, track, channel); })
            {
                owner.ChannelEvent(*this, track, channel);
            }
        },
        players_[player]);
}

void Engine::VoiceEvent(int player, int channel, VoiceCallbackStatus status)
{
    if (StreamSoundPlayer* stream = players_.Get<StreamSoundPlayer>(player))
    {
        stream->VoiceEvent(*this, channel, status);
    }
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const ntdlib::sar::SoundInfo& info = archive_.Sounds()[index];
    if (info.type != ntdlib::sar::SoundType::kSequence)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    // The banks load when the sequence starts. The sound's data loads then too.
    for (std::size_t i = 0; i < std::min<std::size_t>(ntdlib::sar::kBankSlots, info.banks.size()); i++)
    {
        GetBank(info.banks[i]);
    }

    const ntdlib::sar::Sequence sequence = ntdlib::sar::Sequence::Parse(file, kSarOptions);

    return StartNow<SequenceSoundPlayer>(info, sequence.data);
}

WaveSoundPlayer* Engine::StartWaveSound(uint32_t index)
{
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const ntdlib::sar::SoundInfo& info = archive_.Sounds()[index];
    if (info.type != ntdlib::sar::SoundType::kWaveSound)
    {
        return nullptr;
    }

    const auto file = archive_.FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const ntdlib::sar::WaveSoundFile wsd = ntdlib::sar::WaveSoundFile::Parse(file, kSarOptions);
    if (info.wave_sound_index >= wsd.sounds.size())
    {
        return nullptr;
    }

    // The wave of the sound's first note, as WaveFileReader reads it.
    const ntdlib::sar::WaveSoundFile::WaveSound& sound = wsd.sounds[info.wave_sound_index];
    std::optional<WaveInfo> wave;
    if (!sound.notes.empty() && sound.notes[0].wave_id_index < wsd.waves.size())
    {
        const ntdlib::sar::WaveId& id = wsd.waves[sound.notes[0].wave_id_index];
        wave = GetWave(id.wave_archive_item, id.index);
    }

    return StartNow<WaveSoundPlayer>(info, sound, wave);
}

StreamSoundPlayer* Engine::StartStreamSound(uint32_t index, std::span<const uint8_t> file)
{
    if (index >= archive_.Sounds().size() || archive_.Sounds()[index].type != ntdlib::sar::SoundType::kStream)
    {
        return nullptr;
    }

    return StartStreamSound(archive_.Sounds()[index], file);
}

StreamSoundPlayer* Engine::StartStreamSound(const ntdlib::sar::SoundInfo& info, std::span<const uint8_t> file)
{
    const ntdlib::sar::StreamFile stream = ntdlib::sar::StreamFile::Read(file);
    const StreamTracks tracks = StreamTracksOf(info.stream, stream);

    // The player's channels: the sound's, up to 8 (0x180898). The model plays no more than the stream holds.
    const std::size_t count =
        std::min({std::size_t{info.stream.channel_count}, StreamSoundPlayer::kMaxChannels, stream.channels.size()});
    if (tracks.tracks.empty() || count == 0)
    {
        return nullptr;
    }

    std::vector<WaveInfo> waves;
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        const std::vector<uint8_t>& samples = stream.channels[channel].samples;
        waves.push_back(StreamChannelWave(stream, channel, fcram_.Store(samples.data(), samples.size())));
    }

    return StartNow<StreamSoundPlayer>(info, tracks, std::move(waves));
}

void Engine::FrameProcess()
{
    // SoundThread::FrameProcess (0x1666d0, which falls through to the frame's work at 0x16674c): player updates,
    // channel updates, the per-frame random advance, then voice updates.
    players_.ForEach([this](auto& player) { player.Update(*this); });

    channels_.UpdateAllChannel(*this);
    random_.Next();
    voices_.UpdateAllVoices();
}

bool Engine::RunFrame()
{
    if (!snd_.WaitForDspSync())
    {
        return false;
    }

    FrameProcess();
    SetHostMixes();
    snd_.SendParameterToDsp();

    return true;
}

bool Engine::IsBusy() const
{
    return players_.AnyPlaying() || channels_.ActiveCount() > 0;
}

void Engine::MuteTrack(int player, int track, bool mute)
{
    if (mute)
    {
        muted_tracks_.insert({player, track});
    }
    else
    {
        muted_tracks_.erase({player, track});
    }
}

bool Engine::IsTrackMuted(int player, int track) const
{
    return muted_tracks_.contains({player, track});
}

void Engine::MeterTracks(int player)
{
    metered_player_ = player;
    snd_.Dsp().SetMeters(player >= 0 ? ntdlib::nwsnd::kTrackCount : 0);
}

std::array<int32_t, 2> Engine::TrackPeaks(int track) const
{
    if (metered_player_ < 0 || track < 0 || track >= ntdlib::nwsnd::kTrackCount)
    {
        return {0, 0};
    }

    return snd_.Dsp().MeterPeaks(track);
}

void Engine::SetHostMixes()
{
    // The DSP voices of each channel's voice, by their ids: the mute of the channel's track, and its meter if the
    // metered player plays it.
    const auto count = static_cast<std::size_t>(snd_.VoiceCount());
    std::vector<bool> muted(count, false);
    std::vector<int> meters(count, -1);
    for (const auto place : channels_.InUse())
    {
        const Channel& channel = channels_.At(place);
        if (channel.voice == kNoVoice)
        {
            continue;
        }

        const bool mute = IsTrackMuted(channel.source_player, channel.source_track);
        const bool metered = metered_player_ >= 0 && channel.source_player == metered_player_ &&
                             channel.source_track >= 0 && channel.source_track < ntdlib::nwsnd::kTrackCount;
        const Voice& voice = voices_.At(channel.voice);
        for (int i = 0; i < voice.channel_count_; i++)
        {
            if (const nnsnd::Voice* nn = voice.nn_[static_cast<std::size_t>(i)]; nn != nullptr)
            {
                const auto id = static_cast<std::size_t>(nn->Id());
                muted[id] = muted[id] || mute;
                meters[id] = metered ? channel.source_track : meters[id];
            }
        }
    }

    for (int id = 0; id < snd_.VoiceCount(); id++)
    {
        snd_.SetHostMuted(id, muted[static_cast<std::size_t>(id)]);
        if (metered_player_ >= 0)
        {
            snd_.Dsp().SetSourceMeter(id, meters[static_cast<std::size_t>(id)]);
        }
    }
}

Engine::Snapshot Engine::Save() const
{
    Snapshot snapshot;
    snapshot.voices = voices_.Save();
    snapshot.channels = channels_;
    snapshot.random = random_;
    snapshot.players = players_.Save();
    snapshot.banks = banks_;
    snapshot.warcs = warcs_;
    snapshot.waves = waves_;
    snapshot.global_vars = global_vars_;

    return snapshot;
}

void Engine::Restore(const Snapshot& snapshot)
{
    // The players that started after the snapshot go. The others keep their places. Callers hold pointers to them.
    players_.CheckRestore(snapshot.players);
    players_.Restore(snapshot.players);

    voices_.Restore(snapshot.voices);
    channels_ = snapshot.channels;
    random_ = snapshot.random;
    banks_ = snapshot.banks;
    warcs_ = snapshot.warcs;
    waves_ = snapshot.waves;
    global_vars_ = snapshot.global_vars;
}

ArchiveModel::Snapshot ArchiveModel::Save(const Snapshot* previous)
{
    return {fcram.Save(previous ? &previous->fcram : nullptr), dsp->SaveState(previous ? previous->dsp.get() : nullptr),
            snd, engine ? std::optional<Engine::Snapshot>(engine->Save()) : std::nullopt};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    if (engine.has_value() != snapshot.engine.has_value())
    {
        throw std::invalid_argument("archive model snapshot from before or after the engine started");
    }

    fcram.Restore(snapshot.fcram);
    dsp->RestoreState(*snapshot.dsp);
    snd = snapshot.snd;
    if (engine)
    {
        engine->Restore(*snapshot.engine);
    }
}

ArchiveModel::ArchiveModel(DspKind dsp_kind)
    : kind(dsp_kind),
      dsp(dsp_kind == DspKind::kTeakra ? std::unique_ptr<dsp::Device>(std::make_unique<dsp::TeakraDsp>(fcram))
                                       : std::unique_ptr<dsp::Device>(std::make_unique<dsp::FirmwareModel>(
                                             fcram, dsp_kind == DspKind::kEnhancedModel)))
{
}

bool ArchiveModel::Start(const ntdlib::sar::SoundArchive& archive, uint8_t output_mode)
{
    return Start({}, archive, output_mode);
}

bool ArchiveModel::Start(std::span<const uint8_t> firmware, const ntdlib::sar::SoundArchive& archive,
                         uint8_t output_mode)
{
    if (kind == DspKind::kTeakra && firmware.empty())
    {
        return false;
    }

    nnsnd::DspDefaults defaults;
    defaults.output_mode = output_mode;
    if (!snd.Initialize(firmware, defaults))
    {
        return false;
    }

    EngineOptions options;
    options.output_mode = output_mode;
    options.enhanced = kind == DspKind::kEnhancedModel;
    engine.emplace(snd, fcram, archive, options);

    return true;
}

} // namespace ntdlib::ctr::snd

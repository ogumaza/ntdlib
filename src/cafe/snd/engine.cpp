// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/engine.h"

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

#include "xsflib/rip/length.h"

#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/ax/voice.h"
#include "ntdlib/cafe/snd/channel.h"
#include "ntdlib/cafe/snd/renderer.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/cafe/snd/sound_players.h"
#include "ntdlib/cafe/snd/voice.h"
#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

namespace ntdlib::cafe::snd
{
namespace
{

// How the engine reads the archive's files: the Wii U's flavour, with the rules that stop at the first problem. The
// readers take either byte order, by each file's byte order mark.
const sar::Options kSarOptions{sar::Flavor::kCafe, {}, false, sar::Rules::kStopAtFirstProblem};

} // namespace

Engine::Engine(ax::System& system, ax::Memory& memory, const sar::SoundArchive& archive, EngineOptions options)
    : system_(&system),
      memory_(&memory),
      archive_(&archive),
      options_(options),
      ax_voices_(system, memory),
      renderer_(ax_voices_, options_.hardware, options.voice_count),
      voices_(options.voice_count),
      channels_(options.voice_count),
      files_(archive, kSarOptions, [this](ntdlib::nwsnd::LoadedWaveArchive& loaded) { MapWaveArchive(loaded); }),
      effects_(system, options_.effects, options_.hardware.output_mode)
{
    global_vars_.fill(-1);

    // HwCallbackProc counts the frame, updates the AX voices, and wakes the sound thread. RunFrame stands in for the
    // sound thread.
    system_->RegisterFrameCallback([this] { ax_voices_.UpdateAllVoices(options_.hardware.output_mode); });
}

Engine::~Engine()
{
    system_->RegisterFrameCallback({});
    for (const uint32_t address : stream_addresses_)
    {
        memory_->Unmap(address);
    }
}

uint32_t Engine::Map(std::span<const uint8_t> bytes)
{
    const uint32_t address = next_address_;
    memory_->Map(address, bytes);
    next_address_ += (static_cast<uint32_t>(bytes.size()) + 0xFF) & ~0xFFu;

    return address;
}

void Engine::MapWaveArchive(ntdlib::nwsnd::LoadedWaveArchive& loaded)
{
    loaded.address = Map(loaded.file);
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    const std::optional<Files::Note> note = files_.FindNote(banks, slot, info.prg_no, info.key, info.velocity);
    if (!note)
    {
        return nullptr;
    }

    Channel* channel = channels_.AllocChannel(*this, std::min(note->wave.channel_count, 2), info.priority,
                                              info.owner_player, info.owner_track);
    if (!channel)
    {
        return nullptr;
    }

    ntdlib::nwsnd::SetUpNoteChannel<SequencePolicy>(*channel, *note->region, info);
    channel->Start(*this, note->wave, info.length);

    return channel;
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

void Engine::VoiceEvent(const ntdlib::nwsnd::VoiceOwner& owner, ntdlib::nwsnd::VoiceCallbackStatus status)
{
    if (StreamSoundPlayer* player = players_.Get<StreamSoundPlayer>(owner.player))
    {
        player->VoiceEvent(*this, owner.stream_channel, status);
    }
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_->Sounds().size())
    {
        return nullptr;
    }

    const sar::SoundInfo& info = archive_->Sounds()[index];
    if (info.type != sar::SoundType::kSequence)
    {
        return nullptr;
    }

    const std::span<const uint8_t> file = archive_->FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const sar::Sequence sequence = sar::Sequence::Parse(file, kSarOptions);

    return &players_.Add<SequenceSoundPlayer>(1, info, sequence.data);
}

WaveSoundPlayer* Engine::StartWaveSound(uint32_t index)
{
    if (index >= archive_->Sounds().size())
    {
        return nullptr;
    }

    const sar::SoundInfo& info = archive_->Sounds()[index];
    if (info.type != sar::SoundType::kWaveSound)
    {
        return nullptr;
    }

    const std::span<const uint8_t> file = archive_->FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const sar::WaveSoundFile wsd = sar::WaveSoundFile::Parse(file, kSarOptions);
    if (info.wave_sound_index >= wsd.sounds.size())
    {
        return nullptr;
    }

    // The wave of the sound's first note, as WaveFileReader (0x022B85B0) reads it.
    const sar::WaveSoundFile::WaveSound& sound = wsd.sounds[info.wave_sound_index];
    std::optional<WaveInfo> wave;
    if (!sound.notes.empty() && sound.notes[0].wave_id_index < wsd.waves.size())
    {
        const sar::WaveId& id = wsd.waves[sound.notes[0].wave_id_index];
        wave = files_.Wave(id.wave_archive_item, id.index);
    }

    return &players_.Add<WaveSoundPlayer>(1, info, sound, wave);
}

StreamSoundPlayer* Engine::StartStreamSound(uint32_t index, std::span<const uint8_t> file)
{
    if (index >= archive_->Sounds().size() || archive_->Sounds()[index].type != sar::SoundType::kStream)
    {
        return nullptr;
    }

    return StartStreamSound(archive_->Sounds()[index], file);
}

StreamSoundPlayer* Engine::StartStreamSound(const sar::SoundInfo& info, std::span<const uint8_t> file)
{
    auto stream = std::make_unique<const sar::StreamFile>(sar::StreamFile::Read(file));
    const ntdlib::nwsnd::StreamTracks tracks = ntdlib::nwsnd::StreamTracksOf(info.stream, *stream);
    if (tracks.tracks.empty())
    {
        return nullptr;
    }

    // The player's channels: the stream's first 16. AX reads each channel's samples from its place in AX's memory.
    std::vector<WaveInfo> waves;
    const std::size_t count = std::min<std::size_t>(stream->channels.size(), 16);
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        WaveInfo wave = ntdlib::nwsnd::StreamChannelWave<WaveInfo>(*stream, channel);
        wave.channels[0].address = Map(stream->channels[channel].samples);
        stream_addresses_.push_back(wave.channels[0].address);
        waves.push_back(wave);
    }
    streams_.push_back(std::move(stream));

    return &players_.Add<StreamSoundPlayer>(1, info, tracks, std::move(waves));
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus(*this);

    // The sound thread's commands. They start the sounds that are due.
    players_.StartDue(*this);

    // The players' frames (OnUpdateFrameSoundThread), the channels and the voices.
    players_.ForEach([this](auto& player) { player.Update(*this); });
    channels_.UpdateAllChannel(*this);
    voices_.UpdateAllVoices(*this);
    ax_voices_.UpdatePriorityList();

    // HardwareManager::Update (0x0229EC08) has no fades to move here. The random numbers step once a frame.
    random_.Next();
}

void Engine::RunFrame()
{
    // The host's mutes and meters reach AX's voices first. AX renders the frame with them.
    SetHostMixes();
    system_->RenderFrame();
    FrameProcess();

    if (effects_.Any())
    {
        ringing_frames_ = xsflib::rip::Audible(system_->Output()) ? kRingingFrames : std::max(ringing_frames_ - 1, 0);
    }
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
    system_->SetMeters(player >= 0 ? nwsnd::kTrackCount : 0);
}

std::array<int32_t, 2> Engine::TrackPeaks(int track) const
{
    if (metered_player_ < 0 || track < 0 || track >= nwsnd::kTrackCount)
    {
        return {0, 0};
    }

    return system_->MeterPeaks(track);
}

void Engine::SetHostMixes()
{
    // The AX voices of each channel, by their indices: the mute of the channel's track, and its meter if the metered
    // player plays it. A channel's MultiVoice plays each of its wave's channels on an AX voice.
    std::array<bool, ax::kVoiceCount> muted{};
    std::array<int, ax::kVoiceCount> meters{};
    meters.fill(-1);
    for (const auto place : channels_.InUse())
    {
        const Channel& channel = channels_.At(place);
        if (channel.voice == ntdlib::nwsnd::kNoVoice)
        {
            continue;
        }

        const bool mute = IsTrackMuted(channel.source_player, channel.source_track);
        const bool metered = metered_player_ >= 0 && channel.source_player == metered_player_ &&
                             channel.source_track >= 0 && channel.source_track < nwsnd::kTrackCount;
        for (const MultiVoice::Voice& voice : voices_.At(channel.voice).Voices())
        {
            if (voice.renderer >= 0)
            {
                muted[static_cast<std::size_t>(voice.renderer)] = mute;
                meters[static_cast<std::size_t>(voice.renderer)] = metered ? channel.source_track : -1;
            }
        }
    }

    // Every AX voice, those of no channel included.
    const std::array<ax::Voice, ax::kVoiceCount>& all = system_->Voices();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        system_->SetVoiceMuted(all[i], muted[i]);
        system_->SetVoiceMeter(all[i], meters[i]);
    }
}

Engine::Snapshot Engine::Save() const
{
    return {this,    ax_voices_.Save(), renderer_.Save(), voices_,         channels_,
            random_, global_vars_,      players_.Save(),  effects_.Save(), ringing_frames_};
}

void Engine::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this)
    {
        throw std::invalid_argument("a snapshot of another engine");
    }
    players_.CheckRestore(snapshot.players);

    // The players and the starts that wait.
    players_.Restore(snapshot.players);

    // The voices at each level, the channels and the rest go back into their places.
    ax_voices_.Restore(snapshot.ax_voices);
    renderer_.Restore(snapshot.buffers);
    voices_ = snapshot.voices;
    channels_ = snapshot.channels;
    random_ = snapshot.random;
    global_vars_ = snapshot.global_vars;
    effects_.Restore(snapshot.effects);
    ringing_frames_ = snapshot.ringing_frames;

    // The copy of the MultiVoices can put them at other addresses. Each AX voice's dispose callback then takes its
    // voice at the new address.
    for (int place = 0; place < voices_.AddedCount(); ++place)
    {
        for (AxRenderer::Voice& voice : voices_.At(place).Voices())
        {
            renderer_.Rebind(voice);
        }
    }
}

ArchiveModel::ArchiveModel(const sar::SoundArchive& archive, EngineOptions options)
{
    system.Init(ax::System::Library::kSndCore2, options.renderer_48k);
    system.SetDeviceUpsampleStage(ax::System::Device::kTv, options.upsample_after_final_mix);
    system.SetDeviceUpsampleStage(ax::System::Device::kDrc, options.upsample_after_final_mix);
    engine = std::make_unique<Engine>(system, memory, archive, options);
}

ArchiveModel::Snapshot ArchiveModel::Save() const
{
    return {system.Save(), engine->Save()};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    if (snapshot.system.owner != &system)
    {
        throw std::invalid_argument("a snapshot of another model's AX");
    }

    // The engine checks its part before it changes anything.
    engine->Restore(snapshot.engine);
    system.Restore(snapshot.system);
}

} // namespace ntdlib::cafe::snd

// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/sequence.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rvl::snd
{
namespace
{

// A sample's index from a PB address. The index counts from address 0 in the format's units. ADPCM indices skip the two
// header nibbles of each 16-nibble frame. An address on a header gives the frame's first sample. For such an address,
// the game's conversion (AxVoice::SampleByDspAddress) gives a sample before the frame.
uint64_t SampleAt(uint32_t address, ax::SampleFormat format)
{
    if (format == ax::SampleFormat::kAdpcm)
    {
        return uint64_t{address / ax::kAdpcmNibblesPerFrame} * ax::kAdpcmSamplesPerFrame +
               std::max<uint32_t>(address % ax::kAdpcmNibblesPerFrame, 2) - 2;
    }

    return address;
}

// Does the microcode's work on a running voice's position over a frame, without the DSP. The voice advances by its
// pitch over the frame's samples and wraps to its loop. A voice that doesn't loop stops at its loop address, in the
// zero buffer, when it passes its end. Nothing is mixed.
void AdvanceWithoutDsp(ax::Pb& pb)
{
    const auto format = static_cast<ax::SampleFormat>(pb.addr.format);
    const uint64_t step =
        uint64_t{ax::Join(pb.src.ratio_hi, pb.src.ratio_lo)} * ax::kSamplesPerFrame + pb.src.current_address_frac;
    pb.src.current_address_frac = static_cast<uint16_t>(step);
    uint64_t sample = SampleAt(ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo), format) + (step >> 16);
    const uint64_t end = SampleAt(ax::Join(pb.addr.end_address_hi, pb.addr.end_address_lo), format);
    if (sample > end)
    {
        const uint64_t loop = SampleAt(ax::Join(pb.addr.loop_address_hi, pb.addr.loop_address_lo), format);
        if (pb.addr.loop_flag && loop <= end)
        {
            sample = loop + (sample - end - 1) % (end - loop + 1);
        }
        else
        {
            sample = loop;
            pb.state = static_cast<uint16_t>(ax::VoiceState::kStop);
        }
    }

    const uint32_t address = AxVoice::DspAddressBySample(0, static_cast<uint32_t>(sample), format);
    pb.addr.current_address_hi = static_cast<uint16_t>(address >> 16);
    pb.addr.current_address_lo = static_cast<uint16_t>(address);
}

} // namespace

Engine::Engine(ntdlib::dsp::WiiMemory& memory, ax::System& ax, const ntdlib::rsar::SoundArchive& archive,
               SndBuild build)
    : memory_(memory),
      ax_(ax),
      archive_(archive),
      ax_voices_(*this),
      voices_(*this),
      channels_(ChannelPolicy::kChannelCount),
      build_(build)
{
    std::memset(memory_.At(kZeroBuffer, kZeroBufferSize), 0, kZeroBufferSize);

    ax_.RegisterCallback([this] { OnAxFrame(); });
}

void Engine::OnAxFrame()
{
    // The build of October 2006 ends no ramps after AX's frame. Its envelope ramps land on their targets in the next
    // frame (Voice::CalcAxVe), and its mix doesn't ramp.
    if (!build_.October2006())
    {
        voices_.NotifyVoiceUpdate();
    }

    effects_.OnAxFrame(ax_, players_.empty() ? nullptr : &players_.front()->LocalVariables());
}

Engine::~Engine()
{
    ax_.RegisterCallback(nullptr);
}

SeqPlayer* Engine::StartSequence(uint32_t index, const Arrangement& arrangement)
{
    // SoundArchivePlayer's PrepareSeqImpl: the sequence file and its bank must be in the archive.
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const ntdlib::rsar::SoundInfo& sound = archive_.Sounds()[index];
    if (sound.type != ntdlib::rsar::SoundType::kSequence)
    {
        return nullptr;
    }

    const std::optional<ntdlib::rsar::FileData> file = archive_.File(sound.file_id);
    const LoadedBank* bank = GetBank(sound.bank);
    if (!file || bank == nullptr)
    {
        return nullptr;
    }

    sequences_.push_back(std::make_unique<ntdlib::rsar::Sequence>(
        ntdlib::rsar::Sequence::Parse(file->file, ntdlib::rvl::snd::kRsarOptions)));
    players_.push_back(
        std::make_unique<SeqPlayer>(static_cast<int>(players_.size()), SequenceInfo(sound), sequences_.back()->data));
    player_sends_.push_back(SendsOf(sends_, index));
    update_order_.push_back(players_.back()->Id());
    SeqPlayer* player = players_.back().get();
    player->SetBuild(build_);
    player->Start();

    // What the game sets through the sound's handle before the first tick. The tracks' volumes require the tracks that
    // Start allocates.
    for (const auto& [variable, value] : arrangement.variables)
    {
        player->SetVariable(*this, variable, value);
    }

    player->SetTrackVolume(static_cast<uint16_t>(~arrangement.tracks), 0.0f);
    player->SetTempoRatio(arrangement.tempo_ratio);

    return player;
}

WsdPlayer* Engine::StartWaveSound(uint32_t index)
{
    // PrepareWaveSoundImpl: the wave sound file must be in the archive.
    if (index >= archive_.Sounds().size())
    {
        return nullptr;
    }

    const ntdlib::rsar::SoundInfo& sound = archive_.Sounds()[index];
    if (sound.type != ntdlib::rsar::SoundType::kWaveSound)
    {
        return nullptr;
    }

    const LoadedWaveSoundFile* file = GetWaveSoundFile(sound.file_id);
    if (file == nullptr)
    {
        return nullptr;
    }

    // The callback that gives the player the wave sound's settings, the first note and the note's wave
    // (SoundArchivePlayer::WsdCallback::GetWaveSoundData, 0x80045920). The player ends in its first update without
    // them.
    ntdlib::rsar::WaveSoundFile::WaveSound settings;
    std::optional<WaveData> wave;
    const auto& sounds = file->file.Sounds();
    if (sound.sub_index >= 0 && static_cast<std::size_t>(sound.sub_index) < sounds.size())
    {
        settings = sounds[static_cast<std::size_t>(sound.sub_index)];
        const auto& waves = file->file.Waves();
        if (!settings.notes.empty() && settings.notes.front().wave_index >= 0 &&
            static_cast<std::size_t>(settings.notes.front().wave_index) < waves.size())
        {
            wave = ReadWaveParam(file->wave_data, file->wave_address,
                                 waves[static_cast<std::size_t>(settings.notes.front().wave_index)]);
        }
    }

    const int id = kWavePlayers + static_cast<int>(wave_players_.size());
    wave_players_.push_back(std::make_unique<WsdPlayer>(id, sound, settings, std::move(wave), build_));
    wave_player_sends_.push_back(SendsOf(sends_, index));
    update_order_.push_back(id);
    WsdPlayer* player = wave_players_.back().get();
    player->Start();

    return player;
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    // The sequence's bank was loaded when the sound started.
    if (slot < 0 || static_cast<std::size_t>(slot) >= banks.size())
    {
        return nullptr;
    }

    const LoadedBank* found = GetBank(banks[static_cast<std::size_t>(slot)]);
    if (found == nullptr)
    {
        return nullptr;
    }

    // BankFileReader::ReadInstInfo and ReadWaveParam, then a channel for the wave's channels at the note's priority.
    const LoadedBank& bank = *found;
    const ntdlib::rsar::Region* region = bank.bank.Find(static_cast<int>(info.prg_no), info.key, info.velocity);
    if (region == nullptr || static_cast<std::size_t>(region->wave_index) >= bank.bank.Waves().size())
    {
        return nullptr;
    }

    const WaveData wave = ReadWaveParam(bank.wave_data, bank.wave_address, bank.bank.Waves()[region->wave_index]);
    Channel* channel =
        channels_.AllocChannel(*this, std::min(wave.channel_count, 2), static_cast<uint32_t>(info.priority),
                               info.owner_player, info.owner_track);
    if (channel == nullptr)
    {
        return nullptr;
    }

    channel->key = static_cast<uint8_t>(info.key);
    channel->original_key = static_cast<uint8_t>(region->original_key);

    // The note's volume is the velocity squared times the region's volume. The channel keeps it as its velocity volume.
    // A tie replaces it with the velocity's alone.
    float velocity_volume = static_cast<float>(info.velocity) / 127.0f;
    velocity_volume *= velocity_volume;
    velocity_volume *= static_cast<float>(region->volume) / 127.0f;
    channel->velocity_volume = velocity_volume;

    channel->tune = region->tune;
    channel->envelope.SetAttack(region->attack);
    channel->envelope.SetDecay(region->decay);
    channel->envelope.SetSustain(region->sustain);
    channel->envelope.SetRelease(region->release);
    if (build_.SinceMarch2008())
    {
        // Bank::NoteOn sets the region's hold, note-off type and key group too (Mario Kart Wii, 0x8008d41c and
        // 0x8008d490).
        channel->envelope.SetHold(region->hold);
        channel->ignore_note_off = region->note_off_type == 1;
        channel->key_group = static_cast<uint8_t>(region->key_group);
    }

    // The note's pan is the region's pan plus the track's initial pan.
    float init_pan = static_cast<float>(region->pan - 64) / 63.0f;
    init_pan += static_cast<float>(info.init_pan) / 63.0f;
    channel->init_pan = init_pan;
    channel->init_span = 0.0f;

    channel->Start(*this, wave, info.length);

    return channel;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status)
{
    if (on_channel_event_)
    {
        on_channel_event_(channel, status);
    }

    if (player >= kWavePlayers)
    {
        wave_players_[static_cast<std::size_t>(player - kWavePlayers)]->ChannelEvent(*this, track, channel);

        return;
    }

    players_[static_cast<std::size_t>(player)]->ChannelEvent(*this, track, channel);
}

void Engine::OnVoiceEvent(ChannelId owner, Voice::Status status)
{
    channels_.At(owner).OnVoiceEvent(*this, status);
}

void Engine::FrameProcess()
{
    ax_voices_.FreeAllReservedAxVoice();

    // AxManager::Update (0x800315ac). The master volume and the volume for a reset are both 1. The output volume
    // therefore stays at its full 0x8000.
    ax_.SetMasterVolume(ax::kMaxVolume);

    // The players that have started, in the order they started (SoundThread's player callbacks).
    for (const int owner : update_order_)
    {
        if (owner >= kWavePlayers)
        {
            wave_players_[static_cast<std::size_t>(owner - kWavePlayers)]->Update(*this);
        }
        else
        {
            players_[static_cast<std::size_t>(owner)]->Update(*this);
        }
    }

    channels_.UpdateAllChannel(*this);
    random_.Next();
    voices_.UpdateAllVoices();
}

bool Engine::IsBusy() const
{
    if (channels_.ActiveCount() > 0)
    {
        return true;
    }

    return std::any_of(players_.begin(), players_.end(), [](const auto& player) { return player->IsActive(); }) ||
           std::any_of(wave_players_.begin(), wave_players_.end(),
                       [](const auto& player) { return player->IsActive(); });
}

Engine::Snapshot Engine::Save() const
{
    Snapshot snapshot{.ax_voices = ax_voices_.Save(),
                      .voices = voices_.Save(),
                      .channels = channels_,
                      .random = random_,
                      .global_variables = global_variables_,
                      .players = {},
                      .wave_players = {},
                      .banks = banks_.size(),
                      .wave_sound_files = wave_sound_files_.size(),
                      .effects = effects_};
    for (const std::unique_ptr<SeqPlayer>& player : players_)
    {
        snapshot.players.push_back(*player);
    }
    for (const std::unique_ptr<WsdPlayer>& player : wave_players_)
    {
        snapshot.wave_players.push_back(*player);
    }

    return snapshot;
}

void Engine::Restore(const Snapshot& snapshot)
{
    if (snapshot.players.size() != players_.size() || snapshot.wave_players.size() != wave_players_.size() ||
        snapshot.banks != banks_.size() || snapshot.wave_sound_files != wave_sound_files_.size())
    {
        throw std::runtime_error("the engine has started a sound since the snapshot");
    }

    ax_voices_.Restore(snapshot.ax_voices);
    voices_.Restore(snapshot.voices);
    channels_ = snapshot.channels;
    random_ = snapshot.random;
    global_variables_ = snapshot.global_variables;
    effects_ = snapshot.effects;
    for (std::size_t i = 0; i < players_.size(); i++)
    {
        *players_[i] = snapshot.players[i];
    }
    for (std::size_t i = 0; i < wave_players_.size(); i++)
    {
        *wave_players_[i] = snapshot.wave_players[i];
    }
}

const LoadedBank* Engine::GetBank(uint32_t bank_id)
{
    const auto found = banks_.find(bank_id);
    if (found != banks_.end())
    {
        return found->second.get();
    }

    if (bank_id >= archive_.Banks().size())
    {
        return nullptr;
    }

    const std::optional<ntdlib::rsar::FileData> file = archive_.File(archive_.Banks()[bank_id].file_id);
    if (!file)
    {
        return nullptr;
    }

    auto bank = std::make_unique<LoadedBank>(LoadedBank{
        ntdlib::rsar::Bank::Parse(file->file, file->wave_data, ntdlib::rvl::snd::kRsarOptions), file->wave_data});
    bank->wave_address = memory_.Store(file->wave_data.data(), file->wave_data.size());

    return banks_.emplace(bank_id, std::move(bank)).first->second.get();
}

const LoadedWaveSoundFile* Engine::GetWaveSoundFile(uint32_t file_id)
{
    const auto found = wave_sound_files_.find(file_id);
    if (found != wave_sound_files_.end())
    {
        return found->second.get();
    }

    const std::optional<ntdlib::rsar::FileData> file = archive_.File(file_id);
    if (!file)
    {
        return nullptr;
    }

    auto loaded = std::make_unique<LoadedWaveSoundFile>(LoadedWaveSoundFile{
        ntdlib::rsar::WaveSoundFile::Parse(file->file, file->wave_data, ntdlib::rvl::snd::kRsarOptions),
        file->wave_data});
    loaded->wave_address = memory_.Store(file->wave_data.data(), file->wave_data.size());

    return wave_sound_files_.emplace(file_id, std::move(loaded)).first->second.get();
}

WaveData Engine::ReadWaveParam(std::span<const uint8_t> wave_data, ntdlib::PAddr wave_address,
                               const ntdlib::rsar::Wave& wave) const
{
    // The loop's addresses become sample counts from the wave's start. The end is one past the last sample played.
    const ax::SampleFormat format = ToAxFormat(wave.encoding);
    WaveData data;
    data.format = wave.encoding;
    data.loop = wave.loop;
    data.channel_count = static_cast<int>(wave.channels.size());
    data.sample_rate = wave.sample_rate;
    data.loop_start = AxVoice::SampleByDspAddress(wave.loop_start_address, format);
    data.loop_end = AxVoice::SampleByDspAddress(wave.loop_end_address, format) + 1;
    for (std::size_t i = 0; i < std::min<std::size_t>(wave.channels.size(), data.channels.size()); i++)
    {
        const ntdlib::rsar::WaveChannel& channel = wave.channels[i];
        data.channels[i].data = wave_address + static_cast<ntdlib::PAddr>(channel.data.data() - wave_data.data());
        data.channels[i].adpcm = channel.adpcm.value_or(ntdlib::rsar::DspAdpcmInfo{});
    }

    return data;
}

bool ArchiveModel::Start(const ntdlib::Microcode& microcode, const ntdlib::rsar::SoundArchive& archive)
{
    // AXInit boots the DSP with AX's microcode. Then NW4R SND's AxManager sets up its side. The microcode identifies
    // the build of AX that it comes with. AX of October 2006 comes with a build of NW4R SND that has no banner.
    const ax::Build ax_build = ntdlib::IdentifyBuild(microcode);
    ax.Init(ax_build);
    if (!link.Start(microcode.words, microcode.address))
    {
        return false;
    }

    SndBuild snd_build = build;
    snd_build.october_2006 = ax_build == ax::Build::kOctober2006;
    engine.emplace(memory, ax, archive, snd_build);

    return true;
}

void ArchiveModel::StartWithRenderer(const ntdlib::rsar::SoundArchive& archive)
{
    // The build of NW4R SND that comes with AX of October 2006 runs on that build of AX.
    const ax::Build ax_build = build.october_2006 ? ax::Build::kOctober2006 : ax::Build::kDecember2006;
    ax.Init(ax_build);
    renderer = ax::Renderer(ax_build);
    renders = true;
    engine.emplace(memory, ax, archive, build);
}

bool ArchiveModel::RunFrame(std::vector<int16_t>& output)
{
    // The renderer measures the voices of the metered player's tracks, and the aux buses' returns.
    meters.pbs.clear();
    if (renders && metered_player >= 0)
    {
        for (const auto& [index, track] : engine->TrackVoices(metered_player))
        {
            meters.pbs.emplace_back(ax.PbAddress(index), track);
        }
    }

    // __AXOutNewFrame: the PBs and the command list go to the microcode. The microcode mixes the frame into an output
    // buffer.
    const ax::Frame frame = ax.StartFrame();
    const bool mixed = renders ? renderer.RunFrame(memory, frame.command_list, ax::layout::kCommandListSize, &meters)
                               : link.RunFrame(frame.command_list, ax::layout::kCommandListSize);
    if (!mixed)
    {
        return false;
    }

    // The microcode writes each stereo sample right first. A voice that only mixes into AX's left bus comes out in the
    // second half.
    const uint8_t* samples = memory.At(frame.output, ax::layout::kOutputBufferSize);
    for (int i = 0; i < ax::kSamplesPerFrame; i++)
    {
        output.push_back(static_cast<int16_t>(xsflib::LoadBe16(samples + i * 4 + 2)));
        output.push_back(static_cast<int16_t>(xsflib::LoadBe16(samples + i * 4)));
    }

    // The rest of AX's frame, then NW4R SND's sound thread.
    ax.FinishFrame();
    engine->FrameProcess();

    return true;
}

void ArchiveModel::StartWithoutDsp(const ntdlib::rsar::SoundArchive& archive)
{
    ax.Init();
    engine.emplace(memory, ax, archive, build);
}

void ArchiveModel::RunFrameWithoutDsp()
{
    // AX's frame. In place of the microcode, only its work on the voices' positions is done.
    ax.StartFrame();
    for (uint32_t i = 0; i < ax.VoiceCount(); i++)
    {
        ax::Pb pb = ax.ReadPb(i);
        if (pb.state == static_cast<uint16_t>(ax::VoiceState::kRun))
        {
            AdvanceWithoutDsp(pb);
            ax.WritePb(i, pb);
        }
    }

    ax.FinishFrame();
    engine->FrameProcess();
}

std::vector<std::pair<uint32_t, int>> Engine::TrackVoices(int player) const
{
    std::vector<std::pair<uint32_t, int>> voices;
    for (const auto place : channels_.InUse())
    {
        const Channel& channel = channels_.At(place);
        if (channel.voice == kNoVoice || channel.source_player != player || channel.source_track < 0 ||
            channel.source_track >= ntdlib::nwsnd::kTrackCount)
        {
            continue;
        }

        for (const uint32_t index : voices_.At(channel.voice).AxVoiceIndices())
        {
            voices.emplace_back(index, channel.source_track);
        }
    }

    return voices;
}

ArchiveModel::Snapshot ArchiveModel::Save(const Snapshot* previous) const
{
    return {
        memory.Save(previous ? &previous->memory : nullptr), dsp, renderer, ax, engine->Save(), meters.aux_surround};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    engine->Restore(snapshot.engine);
    memory.Restore(snapshot.memory);
    dsp = snapshot.dsp;
    renderer = snapshot.renderer;
    ax = snapshot.ax;
    meters.aux_surround = snapshot.aux_surround;
}

void ArchiveModel::MeterTracks(int player)
{
    metered_player = player;
    meters.peaks.assign(player >= 0 ? ntdlib::nwsnd::kTrackCount : 0, {0, 0});
}

std::array<int32_t, 2> ArchiveModel::TrackPeaks(int track) const
{
    if (track < 0 || static_cast<std::size_t>(track) >= meters.peaks.size())
    {
        return {0, 0};
    }

    return meters.peaks[static_cast<std::size_t>(track)];
}

std::array<int32_t, 2> ArchiveModel::AuxReturnPeaks(int bus) const
{
    if (!renders || bus < 0 || bus >= ax::kAuxBusCount)
    {
        return {0, 0};
    }

    return meters.aux_peaks[static_cast<std::size_t>(bus)];
}

} // namespace ntdlib::rvl::snd

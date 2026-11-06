// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/archive_player.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/heap.h"

namespace ntdlib::ntr
{

namespace
{

// SOUNDBIAS, at the level that the BIOS gives it before a game starts.
constexpr uint32_t kSoundBias = 0x04000504;
constexpr uint32_t kSoundBiasMiddle = 0x200;

// The captures' control registers, both of whose busy bits a 16-bit read gives, and the channels and the captures of
// the SDK's reverb.
constexpr uint32_t kCaptureControl = 0x04000508;
constexpr uint32_t kCapturesBusy = 0x8080;
constexpr uint32_t kReverbChannels = 0xA;
constexpr uint32_t kReverbCaptures = 3;

// The places of a player's channel priority and volume, for the command that writes a player's field.
constexpr uint32_t kPlayerChannelPriority = 4;
constexpr uint32_t kPlayerVolume = 6;
constexpr uint32_t kAllTracks = 0xFFFF;

// A volume's level in the SDK's player: the sum of the levels of the sound handle's volume, the sequence's volume,
// the player's volume and the fader. A new sequence has 127 for all of them but its volume, whose level adds 0. The
// table of levels is the driver's kDecibel. A volume above 127 reads past the end of the ARM9's table, from data that
// differs by game. The player takes 127's level for it.
int32_t SequenceLevel(uint8_t volume)
{
    return std::clamp<int32_t>(snd::tables::kDecibel[std::min<uint8_t>(volume, 127)], -0x8000, 0x7FFF);
}

// Throws std::out_of_range for a player or a track that the driver doesn't have.
void CheckPlayer(int player)
{
    if (player < 0 || player >= snd::kPlayerCount)
    {
        throw std::out_of_range("no player " + std::to_string(player));
    }
}

void CheckTrack(int track)
{
    if (track < 0 || track >= snd::kTracksPerPlayer)
    {
        throw std::out_of_range("no track " + std::to_string(track));
    }
}

template <typename Info>
const Info& Record(const std::vector<std::optional<Info>>& records, uint32_t index, const char* kind)
{
    if (index >= records.size() || !records[index])
    {
        throw xsflib::FormatError(std::string("the archive has no ") + kind + " " + std::to_string(index));
    }
    return *records[index];
}

} // namespace

snd::CommandList ReverbCommands(const Reverb& reverb, uint32_t buffer)
{
    // SND_SetupChannelPcm's and SND_SetupCapture's arguments, as the driver's commands hold them.
    constexpr uint32_t kTimerClock = 0xFFB0FF;
    constexpr uint32_t kLoopForEver = 1;
    constexpr int kMaximumPan = 127;

    const uint32_t half = reverb.size / 2;
    const uint32_t words = half / 4;
    const auto timer = static_cast<uint32_t>(static_cast<int32_t>(kTimerClock) / reverb.sample_rate);
    // The channels' format is 0 for PCM8 and 1 for PCM16. The captures' is 1 for PCM8 and 0 for PCM16.
    const uint32_t format = reverb.pcm8 ? 0 : 1;
    const uint32_t format_bit = reverb.pcm8 ? 1 : 0;
    const auto volume = static_cast<uint32_t>(reverb.volume);
    snd::CommandList list = {{snd::CommandId::kLockChannels, {kReverbChannels, 0, 0, 0}}};
    for (uint32_t capture = 0; capture < 2; capture++)
    {
        const uint32_t channel = capture * 2 + 1;
        const uint32_t address = buffer + capture * half;
        const auto pan = static_cast<uint32_t>(capture * kMaximumPan);
        list.push_back(
            {snd::CommandId::kSetUpPcmChannel,
             {channel | timer << 16, address, words | volume << 24, format << 24 | kLoopForEver << 26 | pan << 16}});
        list.push_back(
            {snd::CommandId::kSetUpCapture, {address, words, capture << 31 | format_bit << 30 | 1u << 29, 0}});
    }
    list.push_back({snd::CommandId::kStartTimer, {kReverbChannels, kReverbCaptures, 0, 0}});
    return list;
}

snd::CommandList StartCommands(sdat::SoundHeap& heap, const sdat::Archive& archive, const SequenceRef& sequence,
                               const GameSettings& settings, int player)
{
    uint32_t data = 0;
    uint32_t offset = 0;
    uint32_t bank = 0;
    uint8_t volume = 0;
    uint8_t channel_priority = 0;
    uint8_t player_number = 0;
    if (!sequence.archived)
    {
        const sdat::SequenceInfo& info = Record(archive.sequences, sequence.index, "sequence");
        bank = heap.LoadBank(archive, info.bank);
        data = heap.LoadFile(archive, info.file_id) + sdat::Sequence::Parse(archive.File(info.file_id)).data_offset;
        volume = info.volume;
        channel_priority = info.channel_priority;
        player_number = info.player;
    }
    else
    {
        const sdat::SequenceArchiveInfo& info = Record(archive.sequence_archives, sequence.index, "sequence archive");
        const uint32_t file = heap.LoadFile(archive, info.file_id);
        const sdat::SequenceArchive parsed = sdat::SequenceArchive::Parse(archive.File(info.file_id));
        const uint32_t index = *sequence.archived;
        if (index >= parsed.sequences.size() || parsed.sequences[index].offset == 0xFFFFFFFF)
        {
            throw xsflib::FormatError("sequence archive " + std::to_string(sequence.index) + " has no sequence " +
                                      std::to_string(index));
        }

        const sdat::ArchivedSequence& entry = parsed.sequences[index];
        bank = heap.LoadBank(archive, entry.bank);
        data = file + parsed.data_offset;
        offset = entry.offset;
        volume = entry.volume;
        channel_priority = entry.channel_priority;
        player_number = entry.player;
    }

    const auto id = static_cast<uint32_t>(player);
    snd::CommandList list = {{snd::CommandId::kPrepareSequence, {id, data, offset, bank}}};
    uint16_t player_channels = 0;
    if (const auto set = settings.channels.find(player_number); set != settings.channels.end())
    {
        player_channels = set->second;
    }
    else if (player_number < archive.players.size() && archive.players[player_number])
    {
        player_channels = archive.players[player_number]->channels;
    }
    if (player_channels != 0)
    {
        list.push_back({snd::CommandId::kAllocatableChannels, {id, kAllTracks, player_channels, 0}});
    }
    list.push_back({snd::CommandId::kPlayerParameter, {id, kPlayerChannelPriority, channel_priority, 1}});
    const snd::CommandList set = VariableCommands(settings.variables, player);
    list.insert(list.end(), set.begin(), set.end());
    if (const int32_t level = SequenceLevel(volume); level != 0)
    {
        list.push_back({snd::CommandId::kPlayerParameter, {id, kPlayerVolume, static_cast<uint32_t>(level), 2}});
    }
    list.push_back({snd::CommandId::kStartPreparedSequence, {id, 0, 0, 0}});
    for (const TrackChannels& tracks : settings.track_channels)
    {
        list.push_back({snd::CommandId::kAllocatableChannels, {id, tracks.tracks, tracks.channels, 0}});
    }
    return list;
}

std::string Label(const sdat::Archive& archive, const SequenceRef& sequence)
{
    const auto name_or_number = [](const std::string& name, const char* kind, uint32_t index)
    {
        return name.empty() ? kind + std::to_string(index) : name;
    };
    if (!sequence.archived)
    {
        const bool known = sequence.index < archive.sequences.size() && archive.sequences[sequence.index];
        return name_or_number(known ? archive.sequences[sequence.index]->name : std::string(), "SEQ_", sequence.index);
    }

    const bool known = sequence.index < archive.sequence_archives.size() && archive.sequence_archives[sequence.index];
    const uint32_t entry = *sequence.archived;
    if (known)
    {
        const sdat::SequenceArchiveInfo& info = *archive.sequence_archives[sequence.index];
        if (entry < info.sequence_names.size() && !info.sequence_names[entry].empty())
        {
            return info.sequence_names[entry];
        }
    }

    return name_or_number(known ? archive.sequence_archives[sequence.index]->name : std::string(), "SEQARC_",
                          sequence.index) +
           "_" + std::to_string(entry);
}

std::vector<SequenceRef> Sequences(const sdat::Archive& archive)
{
    std::vector<SequenceRef> sequences;
    for (std::size_t i = 0; i < archive.sequences.size(); i++)
    {
        if (archive.sequences[i])
        {
            sequences.push_back({static_cast<uint32_t>(i)});
        }
    }
    for (std::size_t i = 0; i < archive.sequence_archives.size(); i++)
    {
        if (!archive.sequence_archives[i] || archive.sequence_archives[i]->file_id >= archive.files.size())
        {
            continue;
        }

        try
        {
            const sdat::SequenceArchive parsed =
                sdat::SequenceArchive::Parse(archive.File(archive.sequence_archives[i]->file_id));
            for (std::size_t j = 0; j < parsed.sequences.size(); j++)
            {
                if (parsed.sequences[j].offset != 0xFFFFFFFF)
                {
                    sequences.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(j)});
                }
            }
        }
        catch (const xsflib::FormatError&)
        {
        }
    }

    return sequences;
}

snd::CommandList VariableCommands(const Variables& variables, int player)
{
    const auto id = static_cast<uint32_t>(player);
    snd::CommandList list;
    for (uint32_t i = 0; i < variables.local.size(); i++)
    {
        if (variables.local[i])
        {
            list.push_back(
                {snd::CommandId::kPlayerLocalVariable, {id, i, static_cast<uint16_t>(*variables.local[i]), 0}});
        }
    }
    for (uint32_t i = 0; i < variables.global.size(); i++)
    {
        if (variables.global[i])
        {
            list.push_back({snd::CommandId::kGlobalVariable, {i, static_cast<uint16_t>(*variables.global[i]), 0, 0}});
        }
    }
    return list;
}

ArchivePlayer::UnitRegisters::UnitRegisters(ArchivePlayer& player) : player_(player)
{
}

uint32_t ArchivePlayer::UnitRegisters::Read(uint32_t address, int size)
{
    const uint32_t value = player_.unit_.Read(address, size);
    if (player_.on_sound_access)
    {
        player_.on_sound_access({address, size, value, false});
    }
    return value;
}

void ArchivePlayer::UnitRegisters::Write(uint32_t address, int size, uint32_t value)
{
    if (player_.on_sound_access)
    {
        player_.on_sound_access({address, size, value, true});
    }
    player_.unit_.Write(address, size, value);
}

ArchivePlayer::ArchivePlayer(const sdat::Archive& archive, const ArchivePlayerOptions& options)
    : archive_(archive),
      memory_(kMainMemorySize),
      unit_(memory_),
      registers_(*this),
      driver_(memory_, registers_, options.build),
      heap_(memory_, kMainMemory, kFiles, kMainMemory + kMainMemorySize),
      clock_(options.clock)
{
    driver_.on_note = [this](const snd::NoteEvent& note)
    {
        OnNote(note);
    };
    driver_.on_tick = [this](int player)
    {
        if (on_tick)
        {
            on_tick(player);
        }
    };
    unit_.Write(kSoundBias, 2, kSoundBiasMiddle);
    driver_.Start();
    driver_.SetRandomState(options.random_state);
    for (uint64_t frame = 0; frame < options.frames; frame++)
    {
        driver_.Frame(true, {});
    }

    snd::InitSharedWork(std::span(memory_).subspan(kSharedWork - kMainMemory, snd::kSharedWorkSize));
    const std::vector<snd::CommandList> shared_work = {{{snd::CommandId::kSetSharedWork, {kSharedWork, 0, 0, 0}}}};
    driver_.Frame(true, shared_work);

    // The sound unit starts with the system clock. Its samples before the frame that takes the shared work are
    // silent.
    std::vector<SoundOutput> silent;
    unit_.Run(clock_ / 2, silent);
    if (options.enhanced)
    {
        driver_.GrowChannels(true);
        unit_.StartEnhanced();
    }
}

void ArchivePlayer::Start(const SequenceRef& sequence, const GameSettings& settings, int player)
{
    CheckPlayer(player);
    snd::CommandList list;
    if (settings.reverb)
    {
        const Reverb& reverb = *settings.reverb;
        if (reverb.size > kFiles - kReverbBuffer || reverb.size % 8 != 0 || reverb.sample_rate <= 0)
        {
            throw std::invalid_argument("a reverb's buffer of " + std::to_string(reverb.size) + " bytes at " +
                                        std::to_string(reverb.sample_rate) + " Hz doesn't fit the player");
        }

        // NNS_SndCaptureStartReverb stops a reverb that runs, as NNSi_SndCaptureStop does, and clears the buffer.
        if (unit_.Read(kCaptureControl, 2) & kCapturesBusy)
        {
            list.push_back({snd::CommandId::kStopTimer, {kReverbChannels, kReverbCaptures, 0, 0}});
            list.push_back({snd::CommandId::kUnlockChannels, {kReverbChannels, 0, 0, 0}});
        }
        std::fill_n(memory_.begin() + (kReverbBuffer - kMainMemory), reverb.size, uint8_t{0});
        const snd::CommandList commands = ReverbCommands(reverb, kReverbBuffer);
        list.insert(list.end(), commands.begin(), commands.end());
    }
    const snd::CommandList start = StartCommands(heap_, archive_, sequence, settings, player);
    list.insert(list.end(), start.begin(), start.end());
    pending_.push_back(std::move(list));
}

void ArchivePlayer::Send(snd::CommandList list)
{
    pending_.push_back(std::move(list));
}

void ArchivePlayer::RunFrame(std::vector<SoundOutput>& out)
{
    if (unit_.Enhanced())
    {
        throw std::logic_error("the player mixes in the enhanced mode");
    }
    Play(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (!unit_.Enhanced())
    {
        throw std::logic_error("the player doesn't mix in the enhanced mode");
    }
    Play(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Play(nullptr, nullptr);
}

void ArchivePlayer::Play(std::vector<SoundOutput>* out, std::vector<float>* enhanced_out)
{
    const std::size_t channels = static_cast<std::size_t>(unit_.ChannelCount());
    channel_tracks_.resize(std::max(channel_tracks_.size(), channels), -1);
    std::vector<uint8_t> muted(channels);
    std::vector<int8_t> meters(channels, -1);
    for (std::size_t channel = 0; channel < channels; channel++)
    {
        const int owner = channel_tracks_[static_cast<std::size_t>(channel)];
        if (owner < 0)
        {
            continue;
        }
        const int player = owner / snd::kTracksPerPlayer;
        const int track = owner % snd::kTracksPerPlayer;
        if (muted_[static_cast<std::size_t>(player)] & (1u << track))
        {
            muted[channel] = 1;
        }
        if (player == metered_player_)
        {
            meters[channel] = static_cast<int8_t>(track);
        }
    }
    unit_.SetMutedChannels(muted);
    unit_.SetMeters(meters);
    if (out)
    {
        unit_.Run(snd::kFrameClocks / 2, *out);
        peaks_ = unit_.MeterPeaks();
    }
    else if (enhanced_out)
    {
        unit_.Run(snd::kFrameClocks / 2, *enhanced_out);
        peaks_ = unit_.MeterPeaks();
    }
    else
    {
        unit_.Run(snd::kFrameClocks / 2);
        peaks_ = {};
    }
    clock_ += snd::kFrameClocks;

    // A channel that the ARM9's command sets up plays no track's note.
    for (const snd::CommandList& list : pending_)
    {
        for (const snd::Command& command : list)
        {
            if (command.id == snd::CommandId::kSetUpPcmChannel || command.id == snd::CommandId::kSetUpPsgChannel ||
                command.id == snd::CommandId::kSetUpNoiseChannel)
            {
                channel_tracks_[command.args[0] & 0xF] = -1;
            }
        }
    }
    driver_.Frame(true, pending_);
    pending_.clear();
}

void ArchivePlayer::OnNote(const snd::NoteEvent& note)
{
    if (note.channel >= 0)
    {
        if (static_cast<std::size_t>(note.channel) >= channel_tracks_.size())
        {
            channel_tracks_.resize(static_cast<std::size_t>(note.channel) + 1, -1);
        }
        channel_tracks_[static_cast<std::size_t>(note.channel)] =
            static_cast<int16_t>(note.player * snd::kTracksPerPlayer + note.track);
    }
    if (on_note)
    {
        on_note(note);
    }
}

void ArchivePlayer::FollowLengths(snd::LengthHooks hooks, std::set<snd::LoopPlace> endless_loops)
{
    driver_.FollowLengths(std::move(hooks), std::move(endless_loops));
}

void ArchivePlayer::MuteTrack(int player, int track, bool mute)
{
    CheckPlayer(player);
    CheckTrack(track);
    uint16_t& muted = muted_[static_cast<std::size_t>(player)];
    muted = static_cast<uint16_t>(mute ? muted | 1u << track : muted & ~(1u << track));
}

bool ArchivePlayer::IsTrackMuted(int player, int track) const
{
    CheckPlayer(player);
    CheckTrack(track);
    return (muted_[static_cast<std::size_t>(player)] & (1u << track)) != 0;
}

void ArchivePlayer::MeterTracks(int player)
{
    if (player != -1)
    {
        CheckPlayer(player);
    }
    metered_player_ = player;
}

std::array<int32_t, 2> ArchivePlayer::TrackPeaks(int track) const
{
    CheckTrack(track);
    return peaks_[static_cast<std::size_t>(track)];
}

bool ArchivePlayer::Busy() const
{
    if (!pending_.empty())
    {
        return true;
    }
    for (int player = 0; player < snd::kPlayerCount; player++)
    {
        if (driver_.GetPlayer(player).active)
        {
            return true;
        }
    }
    for (int channel = 0; channel < unit_.ChannelCount(); channel++)
    {
        if (unit_.Busy(channel))
        {
            return true;
        }
    }
    return false;
}

uint64_t ArchivePlayer::Clock() const
{
    return clock_;
}

const snd::Driver& ArchivePlayer::Driver() const
{
    return driver_;
}

const SoundUnit& ArchivePlayer::Unit() const
{
    return unit_;
}

std::span<const uint8_t> ArchivePlayer::MainMemory() const
{
    return memory_;
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot* previous) const
{
    if (previous != nullptr && previous->owner_ != this)
    {
        previous = nullptr;
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.pages_.reserve(memory_.size() / Snapshot::kPageSize);
    for (std::size_t at = 0; at < memory_.size(); at += Snapshot::kPageSize)
    {
        const std::size_t page = at / Snapshot::kPageSize;
        if (previous != nullptr && std::memcmp(previous->pages_[page]->data(), &memory_[at], Snapshot::kPageSize) == 0)
        {
            snapshot.pages_.push_back(previous->pages_[page]);
            continue;
        }

        auto copy = std::make_shared<Snapshot::Page>();
        std::memcpy(copy->data(), &memory_[at], Snapshot::kPageSize);
        snapshot.pages_.push_back(std::move(copy));
    }
    snapshot.driver_.emplace(driver_);
    snapshot.unit_.emplace(unit_);
    snapshot.heap_.emplace(heap_);
    snapshot.clock_ = clock_;
    snapshot.pending_ = pending_;
    snapshot.channel_tracks_ = channel_tracks_;
    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    for (std::size_t page = 0; page < snapshot.pages_.size(); page++)
    {
        std::memcpy(&memory_[page * Snapshot::kPageSize], snapshot.pages_[page]->data(), Snapshot::kPageSize);
    }
    driver_ = *snapshot.driver_;
    unit_ = *snapshot.unit_;
    heap_ = *snapshot.heap_;
    clock_ = snapshot.clock_;
    pending_ = snapshot.pending_;
    channel_tracks_ = snapshot.channel_tracks_;
}

} // namespace ntdlib::ntr

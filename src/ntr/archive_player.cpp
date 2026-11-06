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
// the SDK's reverb and output effect.
constexpr uint32_t kCaptureControl = 0x04000508;
constexpr uint32_t kCapturesBusy = 0x8080;
constexpr uint32_t kReverbChannels = 0xA;
constexpr uint32_t kReverbCaptures = 3;

// The arguments of SND_SetupChannelPcm and SND_SetupCapture in the driver's commands.
constexpr int32_t kTimerClock = 0xFFB0FF;
constexpr uint32_t kLoopForEver = 1;
constexpr uint32_t kMaximumVolume = 127;
constexpr uint32_t kMaximumPan = 127;

// The output effect's alarm: the first that the sound library of the SDK allocates. The output selector's settings:
// channel 1 on the left and channel 3 on the right, neither in the mixer. The surround decay that the games' setter
// gives the surround.
constexpr uint32_t kEffectAlarm = 0;
constexpr std::array<uint32_t, 4> kEffectOutputs = {1, 2, 1, 1};
constexpr uint32_t kSurroundDecay = 0x3000;

// A tick of the OS timer, in system clocks. The alarms count in ticks.
constexpr uint64_t kTickClocks = 64;

// The function of OutputMode::kCrossfeed keeps a quarter for each of 24 samples.
constexpr std::size_t kCrossfeedDelay = 24;

// What NNS_SndCaptureStartEffect works out for an output effect.
struct EffectLayout
{
    uint32_t timer = 0;      // the channels' and the captures' timer
    uint32_t half = 0;       // the bytes of each half
    uint32_t part_bytes = 0; // the bytes of each part of a half
    uint32_t period = 0;     // the alarm's period, in ticks
    uint32_t first = 0;      // the alarm's first tick after the start
};

EffectLayout LayoutOf(const OutputEffect& effect)
{
    const auto mode = static_cast<int>(effect.mode);
    if (mode < static_cast<int>(OutputMode::kNone) || mode > static_cast<int>(OutputMode::kMono))
    {
        throw std::invalid_argument("no output mode " + std::to_string(mode));
    }

    const auto refuse = [&effect]
    {
        throw std::invalid_argument("the SDK can't lay out an output effect of " + std::to_string(effect.size) +
                                    " bytes in " + std::to_string(effect.interval) + " parts at " +
                                    std::to_string(effect.sample_rate) + " Hz");
    };
    if (effect.sample_rate <= 0 || effect.interval <= 0 || effect.size % 2 != 0)
    {
        refuse();
    }

    // The timer is a multiple of 32: a sample then takes a whole number of the alarm's ticks.
    EffectLayout layout;
    layout.timer = static_cast<uint32_t>((kTimerClock / effect.sample_rate + 16) & ~31);
    layout.half = effect.size / 2;
    const auto interval = static_cast<uint32_t>(effect.interval);
    layout.part_bytes = layout.half / interval;
    if (layout.timer < 32 || layout.timer > 0xFFE0 || layout.half % interval != 0 || layout.part_bytes == 0 ||
        layout.part_bytes % 4 != 0)
    {
        refuse();
    }

    // The alarm's period is a part's samples. Its first tick comes a part and 32 bytes of samples after the start.
    const uint32_t samples = effect.pcm8 ? layout.half : layout.half / 2;
    const uint32_t sample_ticks = layout.timer / 32;
    layout.period = samples / interval * sample_ticks;
    layout.first = layout.period + sample_ticks * (effect.pcm8 ? 32 : 16);
    return layout;
}

int16_t Clamp16(int32_t value)
{
    return static_cast<int16_t>(std::clamp<int32_t>(value, -0x8000, 0x7FFF));
}

// OutputMode::kSurround: the steps of the function at 0x020608A0 in New Super Mario Bros. (USA).
void Surround(std::span<int16_t> left, std::span<int16_t> right, OutputHistory& history)
{
    // The differences of the part's last two samples, before their change, for the next part.
    const std::size_t count = left.size();
    std::array<int32_t, 2> last = {};
    for (std::size_t i = 0; i < last.size(); i++)
    {
        last[i] = Clamp16(left[count - 2 + i] - right[count - 2 + i]);
    }

    // From the end down: the samples two places earlier keep their values until the loop reaches them.
    for (std::size_t i = count; i-- > 2;)
    {
        const int32_t difference = left[i - 2] - right[i - 2];
        left[i] = Clamp16(left[i] + difference);
        right[i] = Clamp16(right[i] - difference);
    }
    for (std::size_t i = 0; i < last.size(); i++)
    {
        left[i] = Clamp16(left[i] + history[i]);
        right[i] = Clamp16(right[i] - history[i]);
        history[i] = last[i];
    }
}

// OutputMode::kCrossfeed: the steps of the function at 0x020606A8 in New Super Mario Bros. (USA). Pair k of the history
// holds the quarters that the samples of place k of the next 24 add: the left side's, then the right side's.
void Crossfeed(std::span<int16_t> left, std::span<int16_t> right, OutputHistory& history)
{
    // Whole runs of 24 samples while more than 24 remain. The quarters come from the sums before the stop.
    const std::size_t count = left.size();
    std::size_t base = 0;
    while (count > base + kCrossfeedDelay)
    {
        for (std::size_t k = 0; k < kCrossfeedDelay; k++)
        {
            const int32_t sum_left = left[base + k] + history[2 * k];
            const int32_t sum_right = right[base + k] + history[2 * k + 1];
            left[base + k] = Clamp16(sum_left);
            right[base + k] = Clamp16(sum_right);
            history[2 * k] = (sum_right + 1) >> 2;
            history[2 * k + 1] = (sum_left + 1) >> 2;
        }
        base += kCrossfeedDelay;
    }

    // The last 1 to 24 samples take the first quarters. The unused quarters move to the front. The samples after the
    // stop give the quarters after them.
    const std::size_t rest = count - base;
    for (std::size_t j = 0; j < rest; j++)
    {
        left[base + j] = Clamp16(left[base + j] + history[2 * j]);
        right[base + j] = Clamp16(right[base + j] + history[2 * j + 1]);
    }
    std::copy(history.begin() + static_cast<std::ptrdiff_t>(2 * rest), history.end(), history.begin());
    for (std::size_t j = 0; j < rest; j++)
    {
        const std::size_t k = kCrossfeedDelay - rest + j;
        history[2 * k] = (right[base + j] + 1) >> 2;
        history[2 * k + 1] = (left[base + j] + 1) >> 2;
    }
}

// OutputMode::kMono: the steps of the function at 0x02060660 in New Super Mario Bros. (USA).
void Mono(std::span<int16_t> left, std::span<int16_t> right)
{
    for (std::size_t i = 0; i < left.size(); i++)
    {
        left[i] = static_cast<int16_t>((left[i] + right[i] + 1) >> 1);
    }
    std::copy(left.begin(), left.end(), right.begin());
}

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
    const uint32_t half = reverb.size / 2;
    const uint32_t words = half / 4;
    const auto timer = static_cast<uint32_t>(kTimerClock / reverb.sample_rate);
    // The channels' format is 0 for PCM8 and 1 for PCM16. The captures' is 1 for PCM8 and 0 for PCM16.
    const uint32_t format = reverb.pcm8 ? 0 : 1;
    const uint32_t format_bit = reverb.pcm8 ? 1 : 0;
    const auto volume = static_cast<uint32_t>(reverb.volume);
    snd::CommandList list = {{snd::CommandId::kLockChannels, {kReverbChannels, 0, 0, 0}}};
    for (uint32_t capture = 0; capture < 2; capture++)
    {
        const uint32_t channel = capture * 2 + 1;
        const uint32_t address = buffer + capture * half;
        const uint32_t pan = capture * kMaximumPan;
        list.push_back(
            {snd::CommandId::kSetUpPcmChannel,
             {channel | timer << 16, address, words | volume << 24, format << 24 | kLoopForEver << 26 | pan << 16}});
        list.push_back(
            {snd::CommandId::kSetUpCapture, {address, words, capture << 31 | format_bit << 30 | 1u << 29, 0}});
    }
    list.push_back({snd::CommandId::kStartTimer, {kReverbChannels, kReverbCaptures, 0, 0}});
    return list;
}

void ApplyOutputMode(OutputMode mode, std::span<int16_t> left, std::span<int16_t> right, OutputHistory& history)
{
    switch (mode)
    {
    case OutputMode::kSurround:
        Surround(left, right, history);
        break;
    case OutputMode::kCrossfeed:
        Crossfeed(left, right, history);
        break;
    case OutputMode::kMono:
        Mono(left, right);
        break;
    case OutputMode::kNone:
        break;
    }
}

snd::CommandList OutputModeCommands(OutputMode from, OutputMode to)
{
    if (from == to)
    {
        return {};
    }
    if (from == OutputMode::kSurround)
    {
        return {{snd::CommandId::kSurroundDecay, {0, 0, 0, 0}}};
    }
    if (to == OutputMode::kSurround)
    {
        return {{snd::CommandId::kSurroundDecay, {kSurroundDecay, 0, 0, 0}}};
    }
    return {};
}

snd::CommandList OutputEffectCommands(const OutputEffect& effect, uint32_t buffer, uint8_t alarm_id)
{
    const EffectLayout layout = LayoutOf(effect);
    const uint32_t words = layout.half / 4;

    // The channels' format is 0 for PCM8 and 1 for PCM16. The captures' is 1 for PCM8 and 0 for PCM16.
    const uint32_t format = effect.pcm8 ? 0 : 1;
    const uint32_t format_bit = effect.pcm8 ? 1 : 0;
    snd::CommandList list = {{snd::CommandId::kLockChannels, {kReverbChannels, 0, 0, 0}}};
    for (uint32_t capture = 0; capture < 2; capture++)
    {
        const uint32_t channel = capture * 2 + 1;
        const uint32_t address = buffer + capture * layout.half;
        const uint32_t pan = capture * kMaximumPan;
        list.push_back({snd::CommandId::kSetUpPcmChannel,
                        {channel | layout.timer << 16, address, words | kMaximumVolume << 24,
                         format << 24 | kLoopForEver << 26 | pan << 16}});
        list.push_back(
            {snd::CommandId::kSetUpCapture, {address, words, capture << 31 | format_bit << 30 | 1u << 29, 0}});
    }
    list.push_back({snd::CommandId::kSetUpAlarm, {kEffectAlarm, layout.first, layout.period, alarm_id}});
    list.push_back({snd::CommandId::kOutputSelector, kEffectOutputs});
    list.push_back({snd::CommandId::kStartTimer, {kReverbChannels, kReverbCaptures, 1u << kEffectAlarm, 0}});
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
    if (settings.reverb && settings.effect)
    {
        throw std::invalid_argument("the SDK runs a reverb or an output effect, not both");
    }

    snd::CommandList list;
    if (settings.reverb)
    {
        if (effect_)
        {
            throw std::invalid_argument("an output effect runs: the SDK starts no reverb");
        }

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

    // The output effect's commands and state wait until the sequence's commands are known.
    std::optional<EffectState> started;
    if (settings.effect)
    {
        const OutputEffect& effect = *settings.effect;
        const EffectLayout layout = LayoutOf(effect);
        if (effect.size > kFiles - kEffectBuffer)
        {
            throw std::invalid_argument("an output effect's buffer of " + std::to_string(effect.size) +
                                        " bytes doesn't fit the player");
        }
        if (!effect_ && (unit_.Read(kCaptureControl, 2) & kCapturesBusy))
        {
            throw std::invalid_argument("a reverb runs: the SDK starts no output effect");
        }

        if (!unit_.Enhanced())
        {
            // The games' setter of the mode comes first. NNS_SndCaptureStartEffect then stops an effect that runs, as
            // NNSi_SndCaptureStop does. The stop sends the mixer to the output again.
            const snd::CommandList mode = OutputModeCommands(output_mode_, effect.mode);
            list.insert(list.end(), mode.begin(), mode.end());
            if (effect_)
            {
                list.push_back({snd::CommandId::kStopTimer, {kReverbChannels, kReverbCaptures, 1u << kEffectAlarm, 0}});
                list.push_back({snd::CommandId::kUnlockChannels, {kReverbChannels, 0, 0, 0}});
                list.push_back({snd::CommandId::kOutputSelector, {0, 0, 0, 0}});
            }
            const snd::CommandList commands =
                OutputEffectCommands(effect, kEffectBuffer, static_cast<uint8_t>(alarm_ids_ + 1));
            list.insert(list.end(), commands.begin(), commands.end());
            started = EffectState{.size = effect.size,
                                  .part_bytes = layout.part_bytes,
                                  .interval = static_cast<uint32_t>(effect.interval),
                                  .part = 0,
                                  .next_alarm = clock_ + snd::kFrameClocks + layout.first * kTickClocks,
                                  .period = layout.period * kTickClocks};
        }
    }

    const snd::CommandList start = StartCommands(heap_, archive_, sequence, settings, player);
    list.insert(list.end(), start.begin(), start.end());
    if (started)
    {
        // The setter clears the history as it changes the mode. NNS_SndCaptureStartEffect clears the buffer.
        if (settings.effect->mode != output_mode_)
        {
            history_ = {};
            output_mode_ = settings.effect->mode;
        }
        alarm_ids_++;
        std::fill_n(memory_.begin() + (kEffectBuffer - kMainMemory), started->size, uint8_t{0});
        effect_ = started;
    }
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

    // The output effect's alarms split the frame. The ARM9 changes a part at each.
    if (effect_)
    {
        effect_->sounded = EffectSounds();
    }
    const uint64_t end = clock_ + snd::kFrameClocks;
    uint64_t at = clock_;
    const auto play_to = [this, out, enhanced_out, &at](uint64_t until)
    {
        const uint64_t ticks = (until - at) / 2;
        if (out)
        {
            unit_.Run(ticks, *out);
        }
        else if (enhanced_out)
        {
            unit_.Run(ticks, *enhanced_out);
        }
        else
        {
            unit_.Run(ticks);
        }
        at = until;
    };
    while (effect_ && effect_->next_alarm < end)
    {
        play_to(effect_->next_alarm);
        RunEffect();
    }
    play_to(end);
    if (out || enhanced_out)
    {
        peaks_ = unit_.MeterPeaks();
    }
    else
    {
        peaks_ = {};
    }
    clock_ = end;

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

void ArchivePlayer::RunEffect()
{
    // The alarm handler of the SDK passes part `part` of each half to the callback of the effect. The callback runs the
    // function of the mode.
    EffectState& effect = *effect_;
    const std::size_t count = effect.part_bytes / 2;
    const uint32_t left_at = kEffectBuffer - kMainMemory + effect.part * effect.part_bytes;
    const uint32_t right_at = left_at + effect.size / 2;
    std::vector<int16_t> left(count);
    std::vector<int16_t> right(count);
    for (std::size_t i = 0; i < count; i++)
    {
        left[i] = static_cast<int16_t>(memory_[left_at + 2 * i] | memory_[left_at + 2 * i + 1] << 8);
        right[i] = static_cast<int16_t>(memory_[right_at + 2 * i] | memory_[right_at + 2 * i + 1] << 8);
    }

    ApplyOutputMode(output_mode_, left, right, history_);

    for (std::size_t i = 0; i < count; i++)
    {
        memory_[left_at + 2 * i] = static_cast<uint8_t>(left[i]);
        memory_[left_at + 2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(left[i]) >> 8);
        memory_[right_at + 2 * i] = static_cast<uint8_t>(right[i]);
        memory_[right_at + 2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(right[i]) >> 8);
    }
    effect.part = effect.part + 1 < effect.interval ? effect.part + 1 : 0;
    effect.next_alarm += effect.period;
}

bool ArchivePlayer::EffectSounds() const
{
    const auto buffer = std::span(memory_).subspan(kEffectBuffer - kMainMemory, effect_->size);
    const auto nonzero = [](auto value)
    {
        return value != 0;
    };
    return std::ranges::any_of(buffer, nonzero) || std::ranges::any_of(history_, nonzero);
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
        if (effect_ && (channel == 1 || channel == 3))
        {
            continue;
        }
        if (unit_.Busy(channel))
        {
            return true;
        }
    }
    return effect_ && (effect_->sounded || EffectSounds());
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
    snapshot.effect_ = effect_;
    snapshot.output_mode_ = output_mode_;
    snapshot.history_ = history_;
    snapshot.alarm_ids_ = alarm_ids_;
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
    effect_ = snapshot.effect_;
    output_mode_ = snapshot.output_mode_;
    history_ = snapshot.history_;
    alarm_ids_ = snapshot.alarm_ids_;
}

} // namespace ntdlib::ntr

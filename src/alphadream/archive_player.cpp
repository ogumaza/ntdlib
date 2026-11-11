// SPDX-License-Identifier: MIT

#include "ntdlib/alphadream/archive_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "enhanced_mix.h"
#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/output_peaks.h"
#include "ntdlib/alphadream/commands.h"

namespace ntdlib::alphadream
{
namespace
{

constexpr uint32_t kRegFifoVolumes = 0x04000082; // SOUNDCNT_H's low byte
constexpr uint32_t kRegSoundCntHigh = 0x04000083;

// A channel's record: +0 its status, +1 its envelope's level, +2 its sample, +4 its place in 1024ths of a point, +8
// its right volume, +9 its left, +0xA its fine tune and +0xB its key.
constexpr uint32_t kChannelStatus = 0;
constexpr uint32_t kChannelLevel = 1;
constexpr uint32_t kChannelSample = 2;
constexpr uint32_t kChannelPlace = 4;
constexpr uint32_t kChannelVolumes = 8;
constexpr uint32_t kChannelFine = 0xA;
constexpr uint32_t kChannelKey = 0xB;
constexpr uint32_t kFixedPitch = 0x8000;

// A track's record: +0 bit 0 is set while the track plays.
constexpr uint32_t kTrackPlays = 1;

// The frames between a channel's mixing and its playing: the mix fills the half that plays after the next swap.
constexpr size_t kMeterFrames = 1;

// The audition's song, at the end of EWRAM: the song's header, then the track's bytes.
constexpr uint32_t kAuditionSong = agb::kEwramBase + agb::kEwramSize - 0x40;
constexpr uint32_t kAuditionTrack = 4;

// The commands of the audition's track.
constexpr uint8_t kProgram = 0xF0;
constexpr uint8_t kVolume = 0xF1;
constexpr uint8_t kPan = 0xF2;
constexpr uint8_t kCentre = 0x7F;
constexpr uint8_t kJump = 0xF8;
constexpr uint8_t kTempo = 0xF9;
constexpr uint8_t kEnd = 0xFF;

// The fastest tempo: the track's ticks pass several times a frame, and the note's release comes in the next frame.
constexpr uint8_t kFastest = 0xFF;

GameRoutines Routines(const Rom& rom, const DriverInfo& info)
{
    const std::optional<GameRoutines> routines = FindGameRoutines(rom, info);
    if (!routines)
    {
        throw std::invalid_argument("the driver's code isn't laid out as the known versions lay it out");
    }

    return *routines;
}

uint32_t RecordWord(const std::array<uint8_t, kChannelBytes>& record, size_t at)
{
    return uint32_t(record[at]) | uint32_t(record[at + 1]) << 8 | uint32_t(record[at + 2]) << 16 |
           uint32_t(record[at + 3]) << 24;
}

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info)
    : rom_(rom), info_(info), routines_(Routines(rom, info)), driver_(rom, routines_)
{
    // The init's stores, then one frame: its swap marks half 1, and its mix fills nothing. The next swap marks half 0
    // and starts the DMAs again.
    init_writes_ = driver_.SoundWrites();
    driver_.Swap();
    init_writes_.insert(init_writes_.end(), driver_.SoundWrites().begin(), driver_.SoundWrites().end());
    driver_.Frame();
    init_writes_.insert(init_writes_.end(), driver_.SoundWrites().begin(), driver_.SoundWrites().end());
    init_ = driver_.Gba().Save();
    period_ = driver_.TimerPeriod();
    driver_.before_mix = [this]()
    {
        BeforeMix();
    };
}

ArchivePlayer::~ArchivePlayer() = default;

void ArchivePlayer::Begin(const ArchivePlayerOptions& options)
{
    if (options.store_cycle >= agb::kFrameCycles)
    {
        throw std::invalid_argument("the routine's cycle lies past the frame");
    }
    if (options.fifo_bytes <= kTransferBytes || options.fifo_bytes > 2 * kTransferBytes)
    {
        throw std::invalid_argument("a FIFO holds 17 to 32 bytes when its DMA starts again");
    }

    // SetSequencer() checks the phase before the driver changes.
    agb::Apu apu;
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);
    driver_.Gba().Restore(init_);
    started_ = false;
    audition_ = false;
}

void ArchivePlayer::Arm(const ArchivePlayerOptions& options)
{
    agb::Apu apu;
    const std::array<const std::vector<agb::RegisterWrite>*, 2> stores = {&init_writes_, &driver_.SoundWrites()};
    for (const std::vector<agb::RegisterWrite>* writes : stores)
    {
        for (const agb::RegisterWrite& write : *writes)
        {
            apu.Store(write.address, write.size, write.value);
        }
    }

    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);
    apu.SetTimer(apu.Register(kRegSoundCntHigh) >> 2 & 1, period_, period_);

    // The enhanced mode's first mix plays where the console's does: after the FIFOs' bytes and the first half.
    enhanced_.reset();
    if (options.enhanced)
    {
        apu.StartEnhanced();
        enhanced_ = std::make_shared<EnhancedMix>(rom_, period_, uint64_t(options.fifo_bytes + kFrameSamples));
        const std::vector<float> silence(enhanced_->Lead(), 0.0f);
        apu.QueueEnhanced(0, silence);
        apu.QueueEnhanced(1, silence);
    }

    apu_ = std::move(apu);

    // The FIFOs' bytes from before the start are silent. The first swap starts the DMAs again at the buffers' start.
    queued_ = 0;
    last_sound_.reset();
    fifo_bytes_ = options.fifo_bytes;
    const std::vector<int8_t> silence(size_t(fifo_bytes_), 0);
    Queue({silence, silence});
    sources_ = {driver_.Half(0, 0), driver_.Half(1, 0)};

    store_cycle_ = options.store_cycle;
    frames_ = 0;
    samples_ = 0;
    sounding_ = true;
    started_ = true;
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    Song read;
    if (song < 0 || song >= info_.song_count || !ReadSong(rom_, info_, song, read))
    {
        throw std::invalid_argument("entry " + std::to_string(song) + " of the song table holds no song");
    }

    Begin(options);
    driver_.Start(kMusicPlayer, song);
    Arm(options);
}

void ArchivePlayer::StartNote(int track, int instrument, uint8_t key, uint8_t velocity,
                              const ArchivePlayerOptions& options)
{
    if (track < 0 || track >= kTracks)
    {
        throw std::invalid_argument("a player has " + std::to_string(kTracks) + " tracks");
    }
    if (instrument < 0 || instrument >= std::min(info_.instrument_count, 256))
    {
        throw std::invalid_argument("instrument " + std::to_string(instrument) + " lies outside the table");
    }
    if (key > 127 || velocity > 127)
    {
        throw std::invalid_argument("a key or a velocity lies past 127");
    }

    Begin(options);

    // The song: the header with the one track, then the tempo, the instrument, the volume and the pan. A loop of tied
    // notes with a wait of a tick holds the note.
    const uint8_t volume = uint8_t((255 * velocity + 63) / 127);
    const bool note_volume = info_.version != Version::kSuperstarSaga;
    const uint16_t mask = uint16_t(1u << track);
    std::vector<uint8_t> song = {uint8_t(mask), uint8_t(mask >> 8), uint8_t(kAuditionTrack), 0};
    song.insert(song.end(), {kTempo, kFastest, kProgram, uint8_t(instrument), kPan, kCentre});
    if (!note_volume)
    {
        song.insert(song.end(), {kVolume, volume});
    }

    const uint32_t loop = uint32_t(song.size()) - kAuditionTrack;
    song.insert(song.end(), {0, uint8_t(key | kRestFlag)});
    if (note_volume)
    {
        song.push_back(volume);
    }
    song.push_back(1);

    const uint32_t jump = uint32_t(song.size()) - kAuditionTrack;
    const uint16_t back = uint16_t(loop - (jump + 3));
    song.insert(song.end(), {kJump, uint8_t(back), uint8_t(back >> 8), kEnd, kEnd});

    // The note-off: a note of a tick that the tie leads to, then the track's end.
    release_ = kAuditionSong + kAuditionTrack + jump;
    release_bytes_ = {1, key};
    if (note_volume)
    {
        release_bytes_.push_back(volume);
    }
    release_bytes_.push_back(kEnd);

    agb::System& gba = driver_.Gba();
    for (size_t i = 0; i < song.size(); i++)
    {
        gba.Store(kAuditionSong + uint32_t(i), 1, song[i]);
    }

    // The start reads the song table's first entry. The cartridge stays out of the snapshots, and the entry goes back
    // at once.
    const uint32_t entry = gba.Load(info_.song_table, 4);
    gba.Store(info_.song_table, 4, kAuditionSong);
    driver_.Start(kMusicPlayer, 0);
    gba.Store(info_.song_table, 4, entry);
    Arm(options);
    audition_ = true;
}

void ArchivePlayer::ReleaseNote()
{
    if (!started_ || !audition_)
    {
        throw std::logic_error("no audition has started");
    }

    for (size_t i = 0; i < release_bytes_.size(); i++)
    {
        driver_.Gba().Store(release_ + uint32_t(i), 1, release_bytes_[i]);
    }
}

GameDriver& ArchivePlayer::Driver()
{
    return driver_;
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the song plays in the enhanced mode");
    }

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (started_ && !enhanced_)
    {
        throw std::logic_error("the song doesn't play in the enhanced mode");
    }

    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

uint32_t ArchivePlayer::SilentTracks() const
{
    return muted_ | (soloed_ ? ~soloed_ : 0);
}

void ArchivePlayer::ApplyPsgMutes()
{
    apu_.SetMuted(uint8_t(SilentTracks() >> kSampleTracks & 0xF));
}

void ArchivePlayer::BeforeMix()
{
    silenced_.clear();
    const uint32_t silent = SilentTracks();
    agb::System& gba = driver_.Gba();
    for (int c = 0; c < kSampleTracks; c++)
    {
        if (metering_ || enhanced_)
        {
            before_[size_t(c)] = driver_.ChannelBytes(c);
        }

        // A silenced channel stays on with volumes of 0. Its envelope and its place go on.
        const uint32_t volumes = routines_.channels + uint32_t(kChannelBytes * c) + kChannelVolumes;
        if (silent >> c & 1)
        {
            silenced_.emplace_back(volumes, uint16_t(gba.Load(volumes, 2)));
            gba.Store(volumes, 2, 0);
        }
    }
}

void ArchivePlayer::AfterMix()
{
    for (const auto& [at, volumes] : silenced_)
    {
        driver_.Gba().Store(at, 2, volumes);
    }

    mixed_channels_.clear();
    if (!metering_ && !enhanced_)
    {
        return;
    }

    // The channels that the mix played. A release that has ended before the mix leaves the place as it was.
    const uint32_t silent = SilentTracks();
    for (int c = 0; c < kSampleTracks; c++)
    {
        const std::array<uint8_t, kChannelBytes>& b = before_[size_t(c)];
        const std::array<uint8_t, kChannelBytes> a = driver_.ChannelBytes(c);
        const uint32_t place = RecordWord(b, kChannelPlace);
        const uint32_t next = RecordWord(a, kChannelPlace);
        const uint32_t number = uint32_t(b[kChannelSample]) | uint32_t(b[kChannelSample + 1]) << 8;
        MixChannel channel;
        if (b[kChannelStatus] == 0 || (a[kChannelStatus] == 0 && place == next) ||
            !ReadSample(rom_, info_, int(number & ~kFixedPitch), channel.sample) || channel.sample.end == 0)
        {
            continue;
        }

        const bool fixed = number & kFixedPitch;
        channel.channel = c;
        channel.place = place;
        channel.next = next;
        channel.step = MixStep(rom_, info_, routines_, channel.sample.rate, fixed ? kRootKey : b[kChannelKey],
                               fixed ? 0 : b[kChannelFine]);
        channel.level = a[kChannelLevel];
        channel.volumes = {b[kChannelVolumes + 1], b[kChannelVolumes]};
        channel.silenced = silent >> c & 1;
        mixed_channels_.push_back(channel);
    }
}

void ArchivePlayer::RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (out)
    {
        apu_.Run(cycles, *out);
    }
    else if (enhanced_out)
    {
        apu_.Run(cycles, *enhanced_out);
    }
    else
    {
        apu_.Run(cycles);
    }
}

void ArchivePlayer::RunTransfers(uint64_t until)
{
    // The timer's samples come every period_ cycles from the start. After each one, a FIFO of 16 bytes or fewer asks
    // its DMA for 16 more, from memory as the last call of the game's code left it.
    const uint64_t samples = until / period_;
    agb::System& gba = driver_.Gba();
    for (; samples_ < samples; samples_++)
    {
        fifo_bytes_ = std::max(fifo_bytes_ - 1, 0);
        if (fifo_bytes_ > kTransferBytes)
        {
            continue;
        }

        std::array<std::vector<int8_t>, 2> bytes;
        for (size_t f = 0; f < bytes.size(); f++)
        {
            for (uint32_t i = 0; i < uint32_t(kTransferBytes); i++)
            {
                bytes[f].push_back(int8_t(gba.Load(sources_[f] + i, 1)));
            }
            sources_[f] += uint32_t(kTransferBytes);
        }
        Queue(bytes);
        fifo_bytes_ += kTransferBytes;
    }
}

void ArchivePlayer::Queue(const std::array<std::vector<int8_t>, 2>& bytes)
{
    const std::vector<int8_t>& a = bytes[0];
    const std::vector<int8_t>& b = bytes[1];
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
    {
        if (a[i] || b[i])
        {
            last_sound_ = queued_ + i;
        }
    }

    apu_.Queue(0, a);
    apu_.Queue(1, b);
    queued_ += a.size();
}

bool ArchivePlayer::Sounding()
{
    agb::System& gba = driver_.Gba();
    for (int t = 0; t < kTracks; t++)
    {
        if (gba.Load(driver_.TrackRecord(kMusicPlayer, t), 2) & kTrackPlays)
        {
            return true;
        }
    }

    for (int c = 0; c < kSampleTracks; c++)
    {
        if (gba.Load(routines_.channels + uint32_t(kChannelBytes * c) + kChannelStatus, 1))
        {
            return true;
        }
    }

    // A FIFO's output holds the byte that the timer took last. The last byte that wasn't 0 sounds until the timer takes
    // the byte after it.
    const uint64_t taken = queued_ - apu_.Queued(0);

    return !apu_.PsgSilent() || (last_sound_ && taken <= *last_sound_ + 1);
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!started_)
    {
        throw std::logic_error("no song has started");
    }

    metering_ = metered_ && (out || enhanced_out);
    const size_t first_sample = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    apu_.MeterChannels(metering_);
    apu_.ClearPeaks();
    ApplyPsgMutes();

    // VBlank's swap at the frame's start. The swap that marks the first half starts the DMAs again.
    const uint64_t start = apu_.Cycle();
    driver_.Swap();
    for (const agb::RegisterWrite& write : driver_.SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }
    if (driver_.DmaRestarted())
    {
        sources_ = {driver_.Half(0, 0), driver_.Half(1, 0)};
    }
    RunTransfers(start + store_cycle_);
    RunHardware(store_cycle_, out, enhanced_out);

    driver_.Frame();
    AfterMix();
    for (const agb::RegisterWrite& write : driver_.SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }
    if (enhanced_)
    {
        enhanced_->Frame(mixed_channels_, enhanced_out != nullptr);
        apu_.QueueEnhanced(0, enhanced_->Out()[0]);
        apu_.QueueEnhanced(1, enhanced_->Out()[1]);
    }
    RunTransfers(start + agb::kFrameCycles);
    RunHardware(agb::kFrameCycles - store_cycle_, out, enhanced_out);
    frames_++;
    sounding_ = Sounding();

    peaks_ = {};
    output_peaks_ = {};
    if (!metering_)
    {
        mixed_.clear();
        return;
    }

    mixed_.push_back(enhanced_ ? EnhancedParts() : ChannelParts());
    if (mixed_.size() > kMeterFrames)
    {
        TakePeaks(mixed_.front());
        mixed_.erase(mixed_.begin());
    }

    agb::KeepOutputPeaks(output_peaks_, out, enhanced_out, first_sample);
}

ArchivePlayer::TrackParts ArchivePlayer::EnhancedParts() const
{
    // A FIFO's sample counts 4 or 2 times in the output's 10 bits, and 64 times that on the scale of 16-bit samples.
    const uint8_t volumes = apu_.Register(kRegFifoVolumes);
    const uint8_t sides = apu_.Register(kRegSoundCntHigh);
    TrackParts parts = {};
    for (const MixChannel& channel : mixed_channels_)
    {
        if (channel.silenced)
        {
            continue;
        }

        std::array<int32_t, 2>& part = parts[size_t(channel.channel)];
        for (int f = 0; f < 2; f++)
        {
            const float value =
                enhanced_->Parts()[size_t(channel.channel)][size_t(f)] * float(volumes >> (2 + f) & 1 ? 4 : 2) * 64.0f;
            const int32_t rounded = int32_t(std::lround(value));
            if (sides >> (4 * f) & 1)
            {
                part[1] = std::max(part[1], rounded);
            }
            if (sides >> (4 * f + 1) & 1)
            {
                part[0] = std::max(part[0], rounded);
            }
        }
    }

    return parts;
}

ArchivePlayer::TrackParts ArchivePlayer::ChannelParts() const
{
    // A channel adds (point - 128) * volume to the mix on each side, where the volume is the side's volume times the
    // envelope's level over 256. FIFO A takes the left side and FIFO B the right. The output's byte takes the mix's
    // bits from bit 8 on, and plays at 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50%.
    // SOUNDCNT_H's high byte puts each FIFO on its sides, the right in the low bit.
    const uint8_t volumes = apu_.Register(kRegFifoVolumes);
    const uint8_t sides = apu_.Register(kRegSoundCntHigh);
    TrackParts parts = {};
    for (const MixChannel& channel : mixed_channels_)
    {
        if (channel.silenced)
        {
            continue;
        }

        // The points that the mix read: the place's whole part, a step for each sample, round the loop or up to the
        // end.
        const SampleHeader& sample = channel.sample;
        const uint64_t end = uint64_t(sample.end) << 10;
        const uint64_t back = uint64_t(sample.end - std::min(sample.loop_start, sample.end)) << 10;
        uint64_t place = channel.place;
        int32_t peak = 0;
        for (int i = 0; i < kFrameSamples && place < end; i++)
        {
            peak = std::max(peak, std::abs(int32_t(rom_.U8(sample.data + uint32_t(place >> 10))) - 128));
            place += channel.step;
            while (place >= end && sample.loops && back)
            {
                place -= back;
            }
        }

        std::array<int32_t, 2>& part = parts[size_t(channel.channel)];
        for (int f = 0; f < 2; f++)
        {
            const int32_t volume = channel.volumes[size_t(f)] * channel.level >> 8;
            const int32_t value = peak * volume * (volumes >> (2 + f) & 1 ? 2 : 1) / 2;
            if (sides >> (4 * f) & 1)
            {
                part[1] = std::max(part[1], value);
            }
            if (sides >> (4 * f + 1) & 1)
            {
                part[0] = std::max(part[0], value);
            }
        }
    }

    return parts;
}

void ArchivePlayer::TakePeaks(const TrackParts& parts)
{
    peaks_ = parts;
    const uint32_t silent = SilentTracks();
    for (int c = 0; c < kTracks - kSampleTracks; c++)
    {
        const int track = kSampleTracks + c;
        if (!(silent >> track & 1))
        {
            peaks_[size_t(track)] = apu_.ChannelPeaks(c);
        }
    }
}

bool ArchivePlayer::Busy() const
{
    return started_ && sounding_;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kTracks) + " tracks");
    }

    muted_ = mute ? muted_ | 1u << track : muted_ & ~(1u << track);
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kTracks) + " tracks");
    }

    soloed_ = solo ? soloed_ | 1u << track : soloed_ & ~(1u << track);
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    if (!on)
    {
        mixed_.clear();
        peaks_ = {};
        output_peaks_ = {};
    }
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot* previous) const
{
    if (!started_)
    {
        throw std::logic_error("no song has started");
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.system_ = driver_.Gba().Save(previous && previous->owner_ == this ? &previous->system_ : nullptr);
    snapshot.apu_ = apu_;
    snapshot.store_cycle_ = store_cycle_;
    snapshot.frames_ = frames_;
    snapshot.samples_ = samples_;
    snapshot.fifo_bytes_ = fifo_bytes_;
    snapshot.sources_ = sources_;
    snapshot.queued_ = queued_;
    snapshot.last_sound_ = last_sound_;
    snapshot.sounding_ = sounding_;
    snapshot.audition_ = audition_;
    snapshot.release_ = release_;
    snapshot.release_bytes_ = release_bytes_;
    if (enhanced_)
    {
        snapshot.enhanced_ = std::make_shared<const EnhancedMix>(*enhanced_);
    }

    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    driver_.Gba().Restore(snapshot.system_);
    apu_ = snapshot.apu_;
    store_cycle_ = snapshot.store_cycle_;
    frames_ = snapshot.frames_;
    samples_ = snapshot.samples_;
    fifo_bytes_ = snapshot.fifo_bytes_;
    sources_ = snapshot.sources_;
    queued_ = snapshot.queued_;
    last_sound_ = snapshot.last_sound_;
    sounding_ = snapshot.sounding_;
    audition_ = snapshot.audition_;
    release_ = snapshot.release_;
    release_bytes_ = snapshot.release_bytes_;
    enhanced_ = snapshot.enhanced_ ? std::make_shared<EnhancedMix>(*snapshot.enhanced_) : nullptr;
    started_ = true;
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::alphadream

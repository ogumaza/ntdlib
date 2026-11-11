// SPDX-License-Identifier: MIT

#include "ntdlib/rd1/archive_player.h"

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
#include "ntdlib/rd1/sequencer.h"
#include "table_bytes.h"

namespace ntdlib::rd1
{
namespace
{

constexpr uint32_t kRegFifoVolumes = 0x04000082; // SOUNDCNT_H's low byte
constexpr uint32_t kRegSoundCntHigh = 0x04000083;

// A player's entry in the player setup table: +4 its channel records, +12 its track records.
constexpr uint32_t kSetupChannels = 4;
constexpr uint32_t kSetupTracks = 12;

// A track's record: +0 bit 0 is set while the track plays, and +0xC holds the 256ths of a tick to its next event.
constexpr uint32_t kTrackBytes = 32;
constexpr uint32_t kTrackWait = 0xC;

// A voice's record: +0xC holds the address of the channel record that the voice plays.
constexpr uint32_t kVoiceChannel = 0xC;

// The frames between a voice's mixing and its playing: the lead of the mixer in the rings.
constexpr size_t kMeterFrames = 2;

// The audition's song record and MIDI file, at the end of EWRAM. The driver keeps its state in IWRAM.
constexpr uint32_t kAuditionRecord = agb::kEwramBase + agb::kEwramSize - 0x100;
constexpr uint32_t kAuditionMidi = kAuditionRecord + 0x20;

// The audition note's wait before its note-off: 0x7FFFFF ticks, the longest that the track's wait of 256ths of a tick
// holds.
constexpr std::array<uint8_t, 4> kLongestDelta = {0x83, 0xFF, 0xFF, 0x7F};

GameRoutines Routines(const Rom& rom, const DriverInfo& info)
{
    const std::optional<GameRoutines> routines = FindGameRoutines(rom, info);
    if (!routines)
    {
        throw std::invalid_argument("the driver's code isn't laid out as the known revisions lay it out");
    }

    return *routines;
}

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info)
    : rom_(rom), info_(info), routines_(Routines(rom, info)), driver_(rom, routines_)
{
    init_writes_ = driver_.SoundWrites();
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

    // SetSequencer() checks the phase before the driver changes.
    agb::Apu apu;
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);
    driver_.Gba().Restore(init_);
    started_ = false;
    audition_ = false;
}

void ArchivePlayer::Arm(int player, const ArchivePlayerOptions& options)
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
    const std::vector<int8_t> pending_a = driver_.PendingA();
    enhanced_.reset();
    if (options.enhanced)
    {
        apu.StartEnhanced();
        enhanced_ = std::make_shared<EnhancedMix>(rom_, period_, pending_a.size());
        const std::vector<float> silence(enhanced_->Lead(), 0.0f);
        apu.QueueEnhanced(0, silence);
        apu.QueueEnhanced(1, silence);
    }

    apu_ = std::move(apu);
    queued_ = 0;
    last_sound_.reset();
    Queue(pending_a, driver_.PendingB());

    const uint32_t setup = info_.player_setup + kSetupBytes * uint32_t(player);
    channels_ = rom_.U32(setup + kSetupChannels);
    track_records_ = rom_.U32(setup + kSetupTracks);
    tracks_ = info_.players[size_t(player)].channels;
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    samples_ = 0;
    fifo_bytes_ = 0;
    sounding_ = true;
    started_ = true;
    psg_tracks_.fill(-1);
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    SongRecord record;
    if (song < 0 || song >= info_.song_count || !ReadSongRecord(rom_, info_, song, record))
    {
        throw std::invalid_argument("entry " + std::to_string(song) + " of the song table holds no song");
    }

    Begin(options);
    driver_.Play(song);
    Arm(record.player, options);
}

void ArchivePlayer::StartNote(int song, uint8_t program, uint8_t key, uint8_t velocity,
                              const ArchivePlayerOptions& options)
{
    if (program > 127 || key > 127 || velocity > 127)
    {
        throw std::invalid_argument("a program, a key or a velocity lies past 127");
    }

    SongRecord record;
    if (song < 0 || song >= info_.song_count || !ReadSongRecord(rom_, info_, song, record))
    {
        throw std::invalid_argument("entry " + std::to_string(song) + " of the song table holds no song");
    }

    Begin(options);

    // The song's record with the audition's MIDI file: one track with the program, the note, a wait and the note-off.
    agb::System& gba = driver_.Gba();
    for (uint32_t i = 0; i < kSongRecordBytes; i++)
    {
        gba.Store(kAuditionRecord + i, 1, rom_.U8(record.address + i));
    }
    gba.Store(kAuditionRecord, 4, kAuditionMidi);

    std::vector<uint8_t> track = {0x00, uint8_t(0xC0), program, 0x00, uint8_t(0x90), key, velocity};
    track.insert(track.end(), kLongestDelta.begin(), kLongestDelta.end());
    track.insert(track.end(), {uint8_t(0x80), key, 0x00, 0x00, 0xFF, 0x2F, 0x00});

    // The header: format 0, one track and the song's division. Then the track's chunk.
    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1};
    file.push_back(rom_.U8(record.midi + 12));
    file.push_back(rom_.U8(record.midi + 13));
    file.insert(file.end(), {'M', 'T', 'r', 'k', 0, 0, 0, uint8_t(track.size())});
    file.insert(file.end(), track.begin(), track.end());
    for (size_t i = 0; i < file.size(); i++)
    {
        gba.Store(kAuditionMidi + uint32_t(i), 1, file[i]);
    }

    driver_.Start(record.player, kAuditionRecord);
    Arm(record.player, options);
    audition_ = true;
}

void ArchivePlayer::ReleaseNote()
{
    if (!started_ || !audition_)
    {
        throw std::logic_error("no audition has started");
    }

    driver_.Gba().Store(track_records_ + kTrackWait, 4, 0);
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

int ArchivePlayer::VoiceTrack(uint32_t record)
{
    const uint32_t channel = driver_.Gba().Load(record + kVoiceChannel, 4);
    if (channel < channels_ || (channel - channels_) % kChannelSize)
    {
        return -1;
    }

    const uint32_t track = (channel - channels_) / kChannelSize;
    return track < uint32_t(tracks_) ? int(track) : -1;
}

void ArchivePlayer::ApplyPsgMutes()
{
    const uint32_t silent = SilentTracks();
    uint8_t mask = 0;
    for (int c = 0; c < kGamePsgVoices; c++)
    {
        const int track = VoiceTrack(routines_.psg_voices + uint32_t(c * kVoiceRecordBytes));
        psg_tracks_[size_t(c)] = track;
        if (track >= 0 && (silent >> track & 1))
        {
            mask = uint8_t(mask | 1u << c);
        }
    }

    apu_.SetMuted(mask);
}

void ArchivePlayer::BeforeMix()
{
    silenced_.clear();
    mixed_voices_.clear();
    const uint32_t silent = SilentTracks();
    const bool keep = metering_ || enhanced_;
    if (!silent && !keep)
    {
        return;
    }

    if (enhanced_)
    {
        settings_ = driver_.Settings();
    }

    agb::System& gba = driver_.Gba();
    const uint32_t mixer = gba.Load(routines_.mixer_voices, 4);
    const uint32_t samples = gba.Load(routines_.sample_voices, 4);
    const uint32_t voices = gba.Load(routines_.mixer_voice_count, 2);
    for (uint32_t v = 0; v < voices; v++)
    {
        const uint32_t at = mixer + v * kVoiceRecordBytes;
        const uint32_t flags = gba.Load(at, 1);
        if (!(flags & 1))
        {
            continue;
        }

        // A silenced voice stays on with gains of 0. The console's mixer and the enhanced mix still see it. The
        // enhanced mode's filter of CC72 then runs in the same frames whatever the mutes.
        const int track = VoiceTrack(samples + v * kVoiceRecordBytes);
        if (track >= 0 && (silent >> track & 1))
        {
            silenced_.emplace_back(at, uint16_t(gba.Load(at + 2, 2)));
            gba.Store(at + 2, 2, 0);
        }
        if (keep)
        {
            MixVoice voice;
            voice.voice = int(v);
            voice.track = track;
            voice.filtered = (flags & 8) || settings_.filter_all;
            for (uint32_t i = 0; i < voice.record.size(); i++)
            {
                voice.record[i] = uint8_t(gba.Load(at + i, 1));
            }
            mixed_voices_.push_back(voice);
        }
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

void ArchivePlayer::RunTransfers()
{
    // The timer's first sample comes period_ cycles after the start. After each sample, a FIFO of 16 bytes or fewer
    // asks its DMA for 16 more.
    const uint64_t cycle = apu_.Cycle();
    const uint64_t samples = cycle >= period_ ? (cycle - period_) / period_ + 1 : 0;
    for (; samples_ < samples; samples_++)
    {
        fifo_bytes_ = std::max(fifo_bytes_ - 1, 0);
        if (fifo_bytes_ <= kTransferBytes)
        {
            driver_.Transfer();
            fifo_bytes_ += kTransferBytes;
        }
    }
}

void ArchivePlayer::Queue(const std::vector<int8_t>& a, const std::vector<int8_t>& b)
{
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
    for (int t = 0; t < tracks_; t++)
    {
        if (gba.Load(track_records_ + uint32_t(t) * kTrackBytes, 1) & 1)
        {
            return true;
        }
    }

    const uint32_t mixer = gba.Load(routines_.mixer_voices, 4);
    const uint32_t voices = gba.Load(routines_.mixer_voice_count, 2);
    for (uint32_t v = 0; v < voices; v++)
    {
        if (gba.Load(mixer + v * kVoiceRecordBytes, 1) & 1)
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
    RunHardware(store_cycle_, out, enhanced_out);
    RunTransfers();

    driver_.Frame();
    for (const auto& [at, gains] : silenced_)
    {
        driver_.Gba().Store(at + 2, 2, gains);
    }
    for (const agb::RegisterWrite& write : driver_.SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }

    // The stores can give a channel to another track.
    ApplyPsgMutes();
    Queue(driver_.FifoA(), driver_.FifoB());
    if (enhanced_)
    {
        enhanced_->Frame(mixed_voices_, settings_, driver_.FifoA().size(), enhanced_out != nullptr);
        apu_.QueueEnhanced(0, enhanced_->Out()[0]);
        apu_.QueueEnhanced(1, enhanced_->Out()[1]);
    }

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

    mixed_.push_back(enhanced_ ? EnhancedParts() : VoiceParts(driver_.FifoA().size()));
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
    for (size_t t = 0; t < parts.size(); t++)
    {
        for (int f = 0; f < 2; f++)
        {
            const float part = enhanced_->Parts()[t][size_t(f)] * float(volumes >> (2 + f) & 1 ? 4 : 2) * 64.0f;
            const int32_t value = int32_t(std::lround(part));
            if (sides >> (4 * f) & 1)
            {
                parts[t][1] = std::max(parts[t][1], value);
            }
            if (sides >> (4 * f + 1) & 1)
            {
                parts[t][0] = std::max(parts[t][0], value);
            }
        }
    }

    return parts;
}

ArchivePlayer::TrackParts ArchivePlayer::VoiceParts(size_t samples) const
{
    // A voice's sample adds sample * gain to the mix. The output's byte takes the mix's bits from bit 7 on, and plays
    // at 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50%. SOUNDCNT_H's high byte puts each FIFO
    // on its sides, the right in the low bit.
    const uint8_t volumes = apu_.Register(kRegFifoVolumes);
    const uint8_t sides = apu_.Register(kRegSoundCntHigh);
    TrackParts parts = {};
    for (const MixVoice& voice : mixed_voices_)
    {
        if (voice.track < 0)
        {
            continue;
        }

        const std::array<uint8_t, kVoiceRecordBytes>& r = voice.record;
        const int32_t level = r[1];
        const std::array<int32_t, 2> gains = {int32_t(int8_t(r[3])) * level >> 7, int32_t(int8_t(r[2])) * level >> 7};
        const uint32_t base = RecordWord(r, 4);
        uint32_t place = RecordWord(r, 0xC);
        const uint32_t loop = RecordWord(r, 0x10);
        const uint32_t end = RecordWord(r, 0x14);
        const uint32_t step = r[0] & 2 ? RecordWord(r, 0x18) : 1u << 14;
        const bool interpolate = (r[0] & 6) == 2;

        // The voice's samples as the mixer reads them: by its whole place, or between two samples by bits 6 to 13 of
        // the place's fraction. A voice without a loop stops at its end.
        int32_t peak = 0;
        for (size_t i = 0; i < samples; i++)
        {
            const uint32_t at = base + (place >> 14);
            int32_t sample = rom_.S8(at);
            if (interpolate)
            {
                sample += (rom_.S8(at + 1) - sample) * int32_t(place & 0x3FC0) >> 14;
            }
            peak = std::max(peak, std::abs(sample));
            place += step;
            if (place >= end)
            {
                if (loop == end)
                {
                    break;
                }

                while (place >= end)
                {
                    place -= end - loop;
                }
            }
        }

        std::array<int32_t, 2>& part = parts[size_t(voice.track)];
        for (int f = 0; f < 2; f++)
        {
            const int32_t value = peak * std::abs(gains[size_t(f)]) * (volumes >> (2 + f) & 1 ? 2 : 1);
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
    for (int c = 0; c < kGamePsgVoices; c++)
    {
        // A frame's stores can give a channel to a muted track. The channel has played up to then. A muted track's
        // meter stays at 0.
        const int track = psg_tracks_[size_t(c)];
        if (track < 0 || (silent >> track & 1))
        {
            continue;
        }

        const std::array<int32_t, 2> part = apu_.ChannelPeaks(c);
        std::array<int32_t, 2>& peak = peaks_[size_t(track)];
        peak[0] = std::max(peak[0], part[0]);
        peak[1] = std::max(peak[1], part[1]);
    }
}

bool ArchivePlayer::Busy() const
{
    return started_ && sounding_;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kMaxChannels)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kMaxChannels) + " tracks");
    }

    muted_ = mute ? muted_ | 1u << track : muted_ & ~(1u << track);
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kMaxChannels)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kMaxChannels) + " tracks");
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
    snapshot.queued_ = queued_;
    snapshot.last_sound_ = last_sound_;
    snapshot.sounding_ = sounding_;
    snapshot.tracks_ = tracks_;
    snapshot.channels_ = channels_;
    snapshot.track_records_ = track_records_;
    snapshot.audition_ = audition_;
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
    queued_ = snapshot.queued_;
    last_sound_ = snapshot.last_sound_;
    sounding_ = snapshot.sounding_;
    tracks_ = snapshot.tracks_;
    channels_ = snapshot.channels_;
    track_records_ = snapshot.track_records_;
    audition_ = snapshot.audition_;
    enhanced_ = snapshot.enhanced_ ? std::make_shared<EnhancedMix>(*snapshot.enhanced_) : nullptr;
    started_ = true;
    psg_tracks_.fill(-1);
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::rd1

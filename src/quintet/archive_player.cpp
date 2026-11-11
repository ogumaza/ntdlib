// SPDX-License-Identifier: MIT

#include "ntdlib/quintet/archive_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ntdlib::quintet
{
namespace
{

constexpr uint32_t kSoundEnd = agb::kRegFifoA;
constexpr uint32_t kFifoBytes = 4;
constexpr uint32_t kTimer0 = 0x04000100;
constexpr uint32_t kTimerBytes = 4;
constexpr uint32_t kDma1 = 0x040000BC;
constexpr uint32_t kDmaBytes = 12;
constexpr uint32_t kSoundCntHHigh = 0x04000083;
constexpr uint32_t kSoundCntX = 0x04000084;

// The frames that the player counts as busy after the channels have ended, while the driver's last stores take effect.
constexpr int kTailFrames = 2;

// A timer's prescaler, from bits 0 and 1 of its control.
constexpr uint32_t kPrescale[4] = {1, 64, 256, 1024};
constexpr uint8_t kTimerOn = 0x80;
constexpr uint8_t kDmaOn = 0x80; // bit 15 of the control, in its high byte

// The bytes that a note's start stores in its FIFO, and the bytes in a FIFO at which its DMA moves more.
constexpr uint32_t kFifoFill = 32;
constexpr size_t kDmaLevel = 16;
constexpr uint32_t kDmaBurst = 16;

uint32_t Le(std::span<const uint8_t> bytes, size_t at, int size)
{
    uint32_t value = 0;
    for (int b = size - 1; b >= 0; b--)
    {
        value = value << 8 | bytes[at + size_t(b)];
    }

    return value;
}

} // namespace

std::vector<uint8_t> InitRegisters(Revision revision)
{
    // The sound init switches the sound on, clears the PSG's registers, sends every channel to both sides, empties the
    // FIFOs and sets the bias's resolution. SOUNDCNT_H's resets read back as 0.
    std::vector<uint8_t> registers(agb::kSoundRegisterBytes, 0);
    const auto set = [&](uint32_t address, int size, uint32_t value)
    {
        for (int b = 0; b < size; b++)
        {
            registers[address - agb::kRegSound1CntL + uint32_t(b)] = uint8_t(value >> (8 * b));
        }
    };
    set(0x04000080, 2, 0xFF77);
    set(0x04000082, 2, 0x730E);
    set(kSoundCntX, 1, 0x80);
    set(0x04000088, 2, 0x0200 | uint32_t(TraitsOf(revision).soundbias_resolution) << 14);

    return registers;
}

SoundBus::SoundBus(const agb::Rom& rom, std::span<const uint8_t> registers, bool fills)
    : rom_(&rom), fills_(fills), apu_(registers)
{
}

void SoundBus::Store(const agb::RegisterWrite& write)
{
    const uint32_t address = write.address;
    if (address >= agb::kRegSound1CntL && address < kSoundEnd)
    {
        ResetFifos(write);
        apu_.Store(address, write.size, write.value);
    }
    else if (address >= agb::kRegFifoA && address < agb::kRegFifoA + 2 * kFifoBytes)
    {
        const int f = int((address - agb::kRegFifoA) / kFifoBytes);
        for (uint32_t b = 0; b < write.size; b++)
        {
            const int8_t byte = int8_t(uint8_t(write.value >> (8 * b)));
            fifos_[size_t(f)].bytes.push_back(byte);
            moved_[size_t(f)].push_back(byte);
        }
    }
    else if (address >= kTimer0 && address < kTimer0 + 2 * kTimerBytes)
    {
        const uint32_t offset = address - kTimer0;
        StoreTimer(int(offset / kTimerBytes), offset % kTimerBytes, write.size, write.value);
    }
    else if (address >= kDma1 && address < kDma1 + 2 * kDmaBytes)
    {
        const uint32_t offset = address - kDma1;
        StoreDma(int(offset / kDmaBytes), offset % kDmaBytes, write.size, write.value);
    }
}

void SoundBus::ResetFifos(const agb::RegisterWrite& write)
{
    const uint32_t address = write.address;
    for (uint32_t b = 0; b < write.size; b++)
    {
        if (address + b == kSoundCntHHigh)
        {
            const uint8_t high = uint8_t(write.value >> (8 * b));
            for (int f = 0; f < 2; f++)
            {
                if (high & (0x08 << (4 * f)))
                {
                    fifos_[size_t(f)].bytes.clear();
                    fifos_[size_t(f)].output = 0;
                }
            }
        }
    }
}

void SoundBus::StoreTimer(int timer, uint32_t offset, int size, uint32_t value)
{
    Timer& t = timers_[size_t(timer)];
    const bool was_on = t.registers[3] & kTimerOn;
    for (int b = 0; b < size && offset + uint32_t(b) < kTimerBytes; b++)
    {
        t.registers[offset + uint32_t(b)] = uint8_t(value >> (8 * b));
    }

    // A timer that starts loads its reload. A running timer takes a new reload at its next overflow.
    const bool on = t.registers[3] & kTimerOn;
    const uint32_t period = (0x10000 - Le(t.registers, 0, 2)) * kPrescale[t.registers[2] & 3];
    if (!on)
    {
        t.period = 0;
        apu_.SetTimer(timer, 0, 0);
    }
    else if (!was_on)
    {
        t.period = period;
        t.next = period;
        apu_.SetTimer(timer, period, period);
    }
    else if (period != t.period)
    {
        t.period = period;
        apu_.SetTimer(timer, period, uint32_t(t.next));
    }
}

void SoundBus::StoreDma(int fifo, uint32_t offset, int size, uint32_t value)
{
    Fifo& f = fifos_[size_t(fifo)];
    const bool was_on = f.registers[11] & kDmaOn;
    for (int b = 0; b < size && offset + uint32_t(b) < kDmaBytes; b++)
    {
        f.registers[offset + uint32_t(b)] = uint8_t(value >> (8 * b));
    }

    // A DMA takes its source as it starts.
    const bool on = f.registers[11] & kDmaOn;
    if (on && !was_on)
    {
        f.source = Le(f.registers, 0, 4);
        if (fills_)
        {
            Fill(fifo, f.source - kFifoFill, kFifoFill);
        }
    }
    f.dma = on;
}

void SoundBus::Fill(int fifo, uint32_t address, uint32_t count)
{
    Fifo& f = fifos_[size_t(fifo)];
    for (uint32_t i = 0; i < count; i++)
    {
        const int8_t byte = rom_->S8(address + i);
        f.bytes.push_back(byte);
        moved_[size_t(fifo)].push_back(byte);
    }
}

int SoundBus::TimerOf(int fifo) const
{
    return apu_.Register(kSoundCntHHigh) >> (2 + 4 * fifo) & 1;
}

bool SoundBus::FifoRunning(int fifo) const
{
    return timers_[size_t(TimerOf(fifo))].period != 0 && fifos_[size_t(fifo)].dma;
}

void SoundBus::StartEnhanced()
{
    apu_.StartEnhanced();
    enhanced_ = true;
}

void SoundBus::PlayFifo(int f, std::vector<float>& enhanced)
{
    Fifo& fifo = fifos_[size_t(f)];
    const Timer& timer = timers_[size_t(TimerOf(f))];
    struct Played
    {
        uint64_t at = 0;
        int8_t byte = 0;
    };
    std::vector<Played> played;
    const int8_t start = fifo.output;
    for (uint64_t at = timer.next; timer.period && at < agb::kFrameCycles; at += timer.period)
    {
        if (!fifo.bytes.empty())
        {
            fifo.output = fifo.bytes.front();
            fifo.bytes.pop_front();
        }
        played.push_back({at, fifo.output});
        if (fifo.dma && fifo.bytes.size() <= kDmaLevel)
        {
            Fill(f, fifo.source, kDmaBurst);
            fifo.source += kDmaBurst;
        }
    }

    // In the enhanced mode, each output sample lies between the byte that plays and the next, at its place between
    // the two overflows. A stopped timer holds the byte.
    if (enhanced_)
    {
        const int8_t next = fifo.bytes.empty() ? fifo.output : fifo.bytes.front();
        for (uint32_t i = 0; i < agb::kEnhancedFrameSamples; i++)
        {
            if (!timer.period)
            {
                enhanced.push_back(float(fifo.output));
                continue;
            }

            const double at = double(i) * double(agb::kFrameCycles) / agb::kEnhancedFrameSamples;
            const auto after = std::upper_bound(played.begin(), played.end(), at,
                                                [](double t, const Played& p) { return t < double(p.at); });
            const int8_t to = after == played.end() ? next : after->byte;
            const bool before = after == played.begin();
            const int8_t from = before ? start : (after - 1)->byte;
            const double from_at = before ? double(after == played.end() ? timer.next : after->at) - timer.period
                                          : double((after - 1)->at);
            const double weight = std::clamp((at - from_at) / timer.period, 0.0, 1.0);
            enhanced.push_back(float(from + (to - from) * weight));
        }
    }
}

void SoundBus::RunFrame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    // Each overflow of a FIFO's timer plays the FIFO's next byte. The DMA moves 16 more once 16 or fewer are left.
    std::array<std::vector<float>, 2> enhanced;
    for (int f = 0; f < 2; f++)
    {
        PlayFifo(f, enhanced[size_t(f)]);
    }

    for (int f = 0; f < 2; f++)
    {
        apu_.Queue(f, moved_[size_t(f)]);
        moved_[size_t(f)].clear();
        if (enhanced_)
        {
            apu_.QueueEnhanced(f, enhanced[size_t(f)]);
        }
    }

    for (Timer& t : timers_)
    {
        while (t.period && t.next < agb::kFrameCycles)
        {
            t.next += t.period;
        }
        t.next -= t.period ? agb::kFrameCycles : 0;
    }

    if (out)
    {
        apu_.Run(agb::kFrameCycles, *out);
    }
    else if (enhanced_out)
    {
        apu_.Run(agb::kFrameCycles, *enhanced_out);
    }
    else
    {
        apu_.Run(agb::kFrameCycles);
    }
}

ArchivePlayer::ArchivePlayer(const agb::Rom& rom, std::span<const DriverInfo> files)
    : rom_(rom), files_(files.begin(), files.end()), bus_(rom, InitRegisters(Revision::kA), true)
{
    if (files_.empty())
    {
        throw std::invalid_argument("the game has no music file");
    }
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    Begin(song, Load(song), options);
}

void ArchivePlayer::StartNote(int song, int channel, uint8_t instrument, int index, uint8_t volume,
                              const ArchivePlayerOptions& options)
{
    Sequencer sequencer = Load(song);
    sequencer.Audition(channel, instrument, index, volume);
    Begin(song, sequencer, options);
}

void ArchivePlayer::ReleaseNote()
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }

    sequencer_->ReleaseAudition();
}

Sequencer ArchivePlayer::Load(int song) const
{
    const DriverInfo& info = files_[FileOfSong(files_, song)];
    const int index = song - info.first_song;
    if (index < 0 || index >= SongCount(info))
    {
        throw std::invalid_argument("the game has no song " + std::to_string(song));
    }

    Sequencer sequencer(rom_, info, index);
    if (!sequencer.Valid())
    {
        throw std::invalid_argument("song " + std::to_string(song) + "'s header can't be read");
    }

    return sequencer;
}

void ArchivePlayer::Begin(int song, const Sequencer& sequencer, const ArchivePlayerOptions& options)
{
    file_ = FileOfSong(files_, song);
    const DriverInfo& info = files_[file_];
    sequencer_.emplace(sequencer);
    bus_ = SoundBus(rom_, InitRegisters(info.revision), true);
    enhanced_ = options.enhanced;
    if (enhanced_)
    {
        bus_.StartEnhanced();
    }
    for (const agb::RegisterWrite& write : sequencer_->StartWrites())
    {
        bus_.Store(write);
    }
    frames_ = 0;
    tail_ = kTailFrames;
    peaks_ = {};
    output_peaks_ = {};
}

const DriverInfo& ArchivePlayer::Info() const
{
    return files_[file_];
}

const Sequencer& ArchivePlayer::Driver() const
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }

    return *sequencer_;
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the player is in the enhanced mode");
    }

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (!enhanced_)
    {
        throw std::logic_error("the player isn't in the enhanced mode");
    }

    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

void ArchivePlayer::ApplyMutes()
{
    uint8_t mask = 0;
    for (int t = 0; t < kPlayerTracks; t++)
    {
        if ((muted_ >> t & 1) || (soloed_ && !(soloed_ >> t & 1)))
        {
            mask = uint8_t(mask | 1u << t);
        }
    }
    bus_.Apu().SetMuted(mask);
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }

    for (const agb::RegisterWrite& write : sequencer_->Step())
    {
        bus_.Store(write);
    }

    const bool metering = metered_ && (out || enhanced_out);
    const size_t first = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    ApplyMutes();
    bus_.Apu().MeterChannels(metering);
    bus_.Apu().ClearPeaks();
    bus_.RunFrame(out, enhanced_out);
    frames_++;
    tail_ = sequencer_->Ended() ? std::max(tail_ - 1, 0) : kTailFrames;

    peaks_ = {};
    output_peaks_ = {};
    if (!metering)
    {
        return;
    }

    for (int t = 0; t < kPlayerTracks; t++)
    {
        peaks_[size_t(t)] = bus_.Apu().ChannelPeaks(t);
    }
    const size_t end = out ? out->size() : enhanced_out->size();
    for (size_t i = first; i < end; i++)
    {
        const int32_t sample = out ? std::abs(int32_t((*out)[i])) : int32_t(std::abs((*enhanced_out)[i]) * 32768.0f);
        output_peaks_[(i - first) % 2] = std::max(output_peaks_[(i - first) % 2], sample);
    }
}

bool ArchivePlayer::Busy() const
{
    if (!sequencer_)
    {
        return false;
    }

    return !sequencer_->Ended() || tail_ > 0 || bus_.FifoRunning(0) || bus_.FifoRunning(1);
}

uint64_t ArchivePlayer::Tick() const
{
    return sequencer_ ? sequencer_->Tick() : 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("no such track");
    }

    muted_ = mute ? muted_ | 1u << track : muted_ & ~(1u << track);
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("no such track");
    }

    soloed_ = solo ? soloed_ | 1u << track : soloed_ & ~(1u << track);
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
}

std::array<int32_t, 2> ArchivePlayer::TrackPeaks(int track) const
{
    if (track < 0 || track >= kPlayerTracks)
    {
        return {};
    }

    return peaks_[size_t(track)];
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }

    Snapshot s;
    s.owner_ = this;
    s.sequencer_ = std::make_shared<const Sequencer>(*sequencer_);
    s.bus_ = std::make_shared<const SoundBus>(bus_);
    s.file_ = file_;
    s.frames_ = frames_;
    s.tail_ = tail_;
    s.enhanced_ = enhanced_;

    return s;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this || !snapshot.sequencer_)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    file_ = snapshot.file_;
    sequencer_.emplace(*snapshot.sequencer_);
    bus_ = *snapshot.bus_;
    frames_ = snapshot.frames_;
    tail_ = snapshot.tail_;
    enhanced_ = snapshot.enhanced_;
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::quintet

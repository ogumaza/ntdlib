// SPDX-License-Identifier: MIT

// The audio lists in the model of the early EAD engine: the commands of each update for the voices, the reverb and
// headset mode, and the frame that runs the updates.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ead_tables.h"
#include "ntdlib/n64/ead_early.h"
#include "ntdlib/rounding.h"

namespace ntdlib::n64
{

namespace
{

namespace tables = ead_tables;

// DMEM's buffers in the engine's commands.
constexpr uint32_t kTemp = 0x0;
constexpr uint32_t kResampled = 0x20;
constexpr uint32_t kResampled2 = 0x160;
constexpr uint32_t kDecoded = 0x180;
constexpr uint32_t kPanTemp = 0x200;
constexpr uint32_t kStrongDry = 0x200;
constexpr uint32_t kStrongWet = 0x340;
constexpr uint32_t kCompressed = 0x3F0;
constexpr uint32_t kLeft = 0x4C0;
constexpr uint32_t kRight = 0x600;
constexpr uint32_t kWetLeft = 0x740;
constexpr uint32_t kWetRight = 0x880;
constexpr uint32_t kOneChannel = 0x140;
constexpr uint32_t kTwoChannels = 0x280;

constexpr uint8_t kFlagInit = 0x01;
constexpr uint8_t kFlagLoop = 0x02;
constexpr uint8_t kFlagLeft = 0x02;
constexpr uint8_t kFlagVolume = 0x04;
constexpr uint8_t kFlagAux = 0x08;

float RampValue(uint16_t source, uint16_t target, int samples)
{
    const std::size_t from = source >> 8;
    const std::size_t to = target >> 8;
    switch (samples)
    {
    case 128:
        return tables::kRampTarget128[to & 127] * tables::kRampSource128[from & 127];
    case 144:
        return tables::kRampTarget144[to & 127] * tables::kRampSource144[from & 127];
    default:
        return tables::kRampTarget136[to & 127] * tables::kRampSource136[from & 127];
    }
}

// One pass of the ADPCM decoder: the frames that it loads, and the samples that it skips at the start and at the
// end. A pass that reaches the loop end restarts at the loop, or finishes a sample without a loop.
struct AdpcmChunk
{
    int skip = 0;
    int first_part = 0;
    int frames = 0;
    int frame_samples = 0;
    int overrun = 0;
    bool finished = false;
    bool restart = false;
};

// Plans the pass that decodes `to_process` samples from `position`. `end_position` is the loop end of the sample.
AdpcmChunk PlanChunk(int to_process, int32_t position, bool restarting, int32_t end_position, bool loops)
{
    AdpcmChunk chunk;
    chunk.skip = position & 0xF;
    const int remaining = end_position - position;
    if (chunk.skip == 0 && !restarting)
    {
        chunk.skip = 16;
    }

    chunk.first_part = 16 - chunk.skip;
    if (to_process < remaining)
    {
        chunk.frames = (to_process - chunk.first_part + 0xF) / 16;
        chunk.frame_samples = chunk.frames * 16;
        chunk.overrun = chunk.first_part + chunk.frame_samples - to_process;
    }
    else
    {
        chunk.frame_samples = remaining + chunk.skip - 0x10;
        chunk.overrun = 0;
        if (chunk.frame_samples <= 0)
        {
            chunk.frame_samples = 0;
            chunk.first_part = remaining;
        }

        chunk.frames = (chunk.frame_samples + 0xF) / 16;
        if (loops)
        {
            chunk.restart = true;
        }
        else
        {
            chunk.finished = true;
        }
    }

    return chunk;
}

// Loads the compressed frames of a pass into DMEM. The load starts at their address rounded down to 16 bytes. Returns
// the misalignment of the address.
uint32_t LoadFrames(AudioList& list, uint32_t sample_address, int32_t position, AdpcmChunk& chunk)
{
    if (chunk.frames == 0)
    {
        chunk.frame_samples = 0;

        return 0;
    }

    const int frame = (position - chunk.skip + 0x10) / 16;
    const uint32_t address = sample_address + static_cast<uint32_t>(frame) * 9;
    const uint32_t misaligned = address & 0xF;
    list.SetBuffer(0, kCompressed, 0, static_cast<uint32_t>(chunk.frames) * 9 + misaligned);
    list.LoadBuffer(address - misaligned);

    return misaligned;
}

} // namespace

void EadEarlyPlayer::RunUpdate()
{
    updates_++;
    for (int i = 0; i < kPlayers; i++)
    {
        if (players_[static_cast<std::size_t>(i)].enabled)
        {
            RunPlayer(i);
            PlayerSound(i);
        }
    }

    ReclaimVoices();
    ProcessVoices();
}

bool EadEarlyPlayer::Busy() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return player.enabled; }) ||
           std::ranges::any_of(voices_, [](const Voice& voice) { return voice.enabled; });
}

void EadEarlyPlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    build_memory_ = &memory;
    list.Segment(0, 0);
    int left = samples;
    uint32_t place = output;
    for (int remaining = updates_per_frame_; remaining > 0; remaining--)
    {
        int chunk = left;
        if (remaining != 1)
        {
            const int share = left / remaining;
            chunk = share - (share & 7);
            if ((share & 7) >= 4)
            {
                chunk += 8;
            }
        }

        RunUpdate();
        Update(list, memory, place, chunk);
        left -= chunk;
        place += static_cast<uint32_t>(chunk) * 4;
    }

    build_memory_ = nullptr;
}

void EadEarlyPlayer::Update(AudioList& list, AudioMemory& /*memory*/, uint32_t output, int samples)
{
    if (config_.reverb_window == 0)
    {
        list.ClearBuffer(kLeft, kTwoChannels);
        BuildVoices(list, output, samples);
        return;
    }

    // The ring buffer's place for this update's samples, in two parts where it wraps.
    if (samples + reverb_position_ - config_.reverb_window < 0)
    {
        reverb_length_a_ = samples * 2;
        reverb_length_b_ = 0;
        reverb_start_ = reverb_position_;
        reverb_position_ += samples;
    }
    else
    {
        const int excess = samples + reverb_position_ - config_.reverb_window;
        reverb_length_a_ = (samples - excess) * 2;
        reverb_length_b_ = excess * 2;
        reverb_start_ = reverb_position_;
        reverb_position_ = excess;
    }

    BuildReverbStart(list, samples);
    BuildVoices(list, output, samples);
    BuildReverbEnd(list);
}

void EadEarlyPlayer::BuildReverbStart(AudioList& list, int /*samples*/)
{
    const auto load_pair = [this, &list](uint32_t into, uint32_t from)
    {
        list.SetBuffer(0, into + kWetLeft, 0, kOneChannel - into);
        list.LoadBuffer(reverb_left_ + from * 2);
        list.SetBuffer(0, into + kWetRight, 0, kOneChannel - into);
        list.LoadBuffer(reverb_right_ + from * 2);
    };

    load_pair(0, static_cast<uint32_t>(reverb_start_));
    if (reverb_length_b_ != 0)
    {
        load_pair(static_cast<uint32_t>(reverb_length_a_), 0);
    }

    list.DmemMove(kWetLeft, kLeft, kTwoChannels);
    list.SetBuffer(0, 0, 0, kTwoChannels);
    list.Mixer(0, static_cast<uint32_t>(0x8000 + config_.reverb_gain) & 0xFFFF, kWetLeft, kWetLeft);
}

void EadEarlyPlayer::BuildReverbEnd(AudioList& list)
{
    const auto save_pair = [this, &list](uint32_t from, uint32_t count, uint32_t into)
    {
        list.SetBuffer(0, 0, from + kWetLeft, count);
        list.SaveBuffer(reverb_left_ + into * 2);
        list.SetBuffer(0, 0, from + kWetRight, count);
        list.SaveBuffer(reverb_right_ + into * 2);
    };

    save_pair(0, static_cast<uint32_t>(reverb_length_a_), static_cast<uint32_t>(reverb_start_));
    if (reverb_length_b_ != 0)
    {
        save_pair(static_cast<uint32_t>(reverb_length_a_), static_cast<uint32_t>(reverb_length_b_), 0);
    }
}

void EadEarlyPlayer::BuildVoices(AudioList& list, uint32_t output, int samples)
{
    const BankBook* loaded_book = nullptr;
    for (Voice& voice : voices_)
    {
        if (voice.enabled)
        {
            BuildVoice(list, voice, samples, loaded_book);
        }
    }

    list.MarkChunkEnd();
    auto length = static_cast<uint32_t>(samples) * 2;
    list.SetBuffer(0, 0, kTemp, length);
    list.Interleave(kLeft, kRight);
    length *= 2;
    list.SetBuffer(0, 0, kTemp, length);
    list.SaveBuffer(output);
}

// Appends the commands of one voice for an update: its samples, the resampling to the pitch, the envelope and the
// headset delay.
void EadEarlyPlayer::BuildVoice(AudioList& list, Voice& voice, int samples, const BankBook*& loaded_book)
{
    list.MarkVoice(voice.track);
    int flags = 0;
    if (voice.needs_init)
    {
        flags = kFlagInit;
        voice.position = 0;
        voice.position_fraction = 0;
    }

    // A region without a sample plays nothing. The games' data never reaches one, but a hand-made file can.
    if (voice.sound != nullptr && voice.sound->sample == kNoObject)
    {
        return;
    }

    int parts = 1;
    float rate = 0.0F;
    if (voice.frequency < 2.0F)
    {
        if (voice.frequency > 1.99996F)
        {
            voice.frequency = 1.99996F;
        }

        rate = voice.frequency;
    }
    else
    {
        parts = 2;
        if (voice.frequency >= 3.99993F)
        {
            voice.frequency = 3.99993F;
        }

        rate = voice.frequency * 0.5F;
    }

    const auto pitch = static_cast<uint16_t>(static_cast<int32_t>(rate * 32768.0F));
    const uint32_t advance = uint32_t{voice.position_fraction} + static_cast<uint32_t>(int32_t{pitch} * samples) * 2;
    voice.position_fraction = static_cast<uint16_t>(advance);
    const auto whole_samples = static_cast<int32_t>(advance >> 16);
    uint32_t input = 0;
    if (voice.sound == nullptr)
    {
        input = LoadWave(list, voice, whole_samples);
    }
    else
    {
        input = DecodeSample(list, voice, parts, whole_samples, flags, loaded_book);
    }

    flags = 0;
    if (voice.needs_init)
    {
        flags = kFlagInit;
        voice.needs_init = false;
    }

    list.SetBuffer(0, input, kTemp, static_cast<uint32_t>(samples) * 2);
    list.Resample(static_cast<uint8_t>(flags), pitch, voice.buffers.final_resample);

    int headset = 0;
    if (voice.headset_right != 0 || voice.previous_headset_right != 0)
    {
        headset = 1;
    }
    else if (voice.headset_left != 0 || voice.previous_headset_left != 0)
    {
        headset = 2;
    }

    BuildEnvelope(list, voice, samples, headset);
    if (voice.headset_effects)
    {
        BuildHeadset(list, voice, samples * 2, flags, headset);
    }
}

// Loads the samples of a wave voice into DMEM, with enough copies for the samples of the update. Returns the DMEM
// address of the first sample.
uint32_t EadEarlyPlayer::LoadWave(AudioList& list, Voice& voice, int32_t whole_samples)
{
    // A wave: its samples, repeated in DMEM.
    list.SetBuffer(0, kDecoded, 0, 128);
    list.LoadBuffer(voice.buffers.samples);
    voice.position &= voice.sample_count - 1;
    const int left_in_copy = 64 - voice.position;
    if (left_in_copy < whole_samples)
    {
        for (int i = 0; i <= (whole_samples - left_in_copy + 63) / 64 - 1; i++)
        {
            list.DmemMove(kDecoded, kDecoded + static_cast<uint32_t>(1 + i) * 128, 128);
        }
    }

    const uint32_t input = kDecoded + static_cast<uint32_t>(voice.position) * 2;
    voice.position += whole_samples;

    return input;
}

// Decodes the ADPCM samples of a voice for the update, in one part, or in two for a pitch of 2 or more. Returns the
// DMEM address of the samples for the final resampling.
uint32_t EadEarlyPlayer::DecodeSample(AudioList& list, Voice& voice, int parts, int32_t whole_samples, int flags,
                                      const BankBook*& loaded_book)
{
    const EadBank& bank = files_->banks[voice.bank];
    const EadSample& sample = bank.samples[static_cast<std::size_t>(voice.sound->sample)];
    const BankBook& book = bank.books[static_cast<std::size_t>(sample.book)];
    uint32_t input = 0;
    int resampled_length = 0;
    int first_offset = 0;
    for (int part = 0; part < parts; part++)
    {
        int wanted = whole_samples;
        if (parts != 1 && (whole_samples & 1) != 0)
        {
            wanted = (whole_samples & ~1) + part * 2;
        }

        if (loaded_book != &book)
        {
            loaded_book = &book;
            const uint32_t size = static_cast<uint32_t>(book.book.order * book.book.predictor_count) * 16;
            list.LoadAdpcm(size, book_addresses_[voice.bank][static_cast<std::size_t>(sample.book)]);
        }

        DecodeAdpcm(list, voice, sample, wanted, flags, first_offset);
        if (parts == 1)
        {
            input = kDecoded + static_cast<uint32_t>(first_offset);
        }
        else if (part == 0)
        {
            list.SetBuffer(0, kDecoded + static_cast<uint32_t>(first_offset), kResampled,
                           static_cast<uint32_t>(wanted) + 4);
            list.Resample(kFlagInit, 0xFF60, voice.buffers.dummy_resample);
            resampled_length = wanted + 4;
            input = kResampled + 4;
            if (voice.finished)
            {
                list.ClearBuffer(kResampled + static_cast<uint32_t>(resampled_length),
                                 static_cast<uint32_t>(wanted) + 0x10);
            }
        }
        else
        {
            list.SetBuffer(0, kDecoded + static_cast<uint32_t>(first_offset), kResampled2,
                           static_cast<uint32_t>(wanted) + 8);
            list.Resample(kFlagInit, 0xFF60, voice.buffers.dummy_resample);
            list.DmemMove(kResampled2 + 4, kResampled + static_cast<uint32_t>(resampled_length),
                          static_cast<uint32_t>(wanted) + 4);
        }

        if (voice.finished)
        {
            break;
        }
    }

    return input;
}

// Decodes `wanted` ADPCM samples of a voice into DMEM, a chunk of frames at a time. At the loop end, the decoding
// restarts at the loop start, or ends the voice for a sample without a loop.
void EadEarlyPlayer::DecodeAdpcm(AudioList& list, Voice& voice, const EadSample& sample, int wanted, int& flags,
                                 int& first_offset)
{
    const EadBank& bank = files_->banks[voice.bank];
    const BankLoop& loop = bank.loops[static_cast<std::size_t>(sample.loop)];
    const uint32_t sample_address = AudioMemory::kImageBase + files_->sample_places[voice.bank] + sample.address;
    const auto end_position = static_cast<int32_t>(loop.end);
    int processed = 0;
    int written = 0;
    while (processed != wanted)
    {
        const int to_process = wanted - processed;
        AdpcmChunk chunk = PlanChunk(to_process, voice.position, voice.restart, end_position, loop.count != 0);
        const uint32_t misaligned = LoadFrames(list, sample_address, voice.position, chunk);
        if (voice.restart)
        {
            list.SetLoop(loop_addresses_[voice.bank][static_cast<std::size_t>(sample.loop)]);
            flags = kFlagLoop;
            voice.restart = false;
        }

        const int this_time = chunk.frame_samples + chunk.first_part - chunk.overrun;
        const int rounded = AlignUp(written, 32);
        if (processed == 0)
        {
            list.SetBuffer(0, kCompressed + misaligned, kDecoded, static_cast<uint32_t>(chunk.frame_samples) * 2);
            list.Adpcm(static_cast<uint8_t>(flags), voice.buffers.adpcm_state);
            first_offset = chunk.skip * 2;
        }
        else
        {
            list.SetBuffer(0, kCompressed + misaligned, kDecoded + static_cast<uint32_t>(rounded),
                           static_cast<uint32_t>(chunk.frame_samples) * 2);
            list.Adpcm(static_cast<uint8_t>(flags), voice.buffers.adpcm_state);
            list.DmemMove(kDecoded + static_cast<uint32_t>(rounded) + static_cast<uint32_t>(chunk.skip) * 2,
                          kDecoded + static_cast<uint32_t>(written), static_cast<uint32_t>(this_time) * 2);
        }

        processed += this_time;
        switch (flags)
        {
        case kFlagInit:
            first_offset = 0;
            written = chunk.frame_samples * 2 + written;
            break;
        case kFlagLoop:
            written = this_time * 2 + written;
            break;
        default:
            written = written != 0 ? this_time * 2 + written : (chunk.skip + this_time) * 2;
            break;
        }

        flags = 0;
        if (chunk.finished)
        {
            list.ClearBuffer(kDecoded + static_cast<uint32_t>(written), static_cast<uint32_t>(wanted - processed) * 2);
            voice.position = 0;
            voice.finished = true;
            voice.enabled = false;
            break;
        }

        if (chunk.restart)
        {
            voice.restart = true;
            voice.position = static_cast<int32_t>(loop.start);
        }
        else
        {
            voice.position += to_process;
        }
    }
}

void EadEarlyPlayer::BuildEnvelope(AudioList& list, Voice& voice, int samples, int headset)
{
    const auto source_left = static_cast<uint16_t>(voice.current_left);
    const auto source_right = static_cast<uint16_t>(voice.current_right);
    const uint16_t target_left = voice.target_left;
    const uint16_t target_right = voice.target_right;
    voice.current_left = static_cast<int16_t>(target_left);
    voice.current_right = static_cast<int16_t>(target_right);
    const auto count = static_cast<uint32_t>(samples) * 2;
    if (voice.headset_effects)
    {
        list.ClearBuffer(kPanTemp, kOneChannel);
        switch (headset)
        {
        case 1:
            list.SetBuffer(0, 0, kPanTemp, count);
            list.SetBuffer(kFlagAux, kRight, kWetLeft, kWetRight);
            break;
        case 2:
            list.SetBuffer(0, 0, kLeft, count);
            list.SetBuffer(kFlagAux, kPanTemp, kWetLeft, kWetRight);
            break;
        default:
            list.SetBuffer(0, 0, kLeft, count);
            list.SetBuffer(kFlagAux, kRight, kWetLeft, kWetRight);
            break;
        }
    }
    else if (voice.strong_right)
    {
        list.ClearBuffer(kStrongDry, kTwoChannels);
        list.SetBuffer(0, 0, kStrongDry, count);
        list.SetBuffer(kFlagAux, kRight, kStrongWet, kWetRight);
    }
    else if (voice.strong_left)
    {
        list.ClearBuffer(kStrongDry, kTwoChannels);
        list.SetBuffer(0, 0, kLeft, count);
        list.SetBuffer(kFlagAux, kStrongDry, kWetLeft, kStrongWet);
    }
    else
    {
        list.SetBuffer(0, 0, kLeft, count);
        list.SetBuffer(kFlagAux, kRight, kWetLeft, kWetRight);
    }

    uint8_t mixer_flags = 0;
    if (target_left != source_left || target_right != source_right || voice.mixer_needs_init)
    {
        mixer_flags = kFlagInit;
        const auto ramp_left = static_cast<int32_t>(RampValue(source_left, target_left, samples));
        const auto ramp_right = static_cast<int32_t>(RampValue(source_right, target_right, samples));
        list.SetVolume(kFlagVolume | kFlagLeft, source_left, 0, 0);
        list.SetVolume(kFlagVolume, source_right, 0, 0);
        list.commands.push_back(
            {(static_cast<uint32_t>(kAudioSetVolume) << 24) | (uint32_t{kFlagLeft} << 16) | target_left,
             static_cast<uint32_t>(ramp_left)});
        list.commands.push_back(
            {(static_cast<uint32_t>(kAudioSetVolume) << 24) | target_right, static_cast<uint32_t>(ramp_right)});
        list.SetVolume(kFlagAux, static_cast<uint16_t>(config_.volume), 0, static_cast<uint16_t>(voice.reverb_shifted));
    }

    const bool use_wet = config_.reverb_window != 0 && voice.reverb != 0;
    list.EnvMixer(use_wet ? static_cast<uint8_t>(mixer_flags | kFlagAux) : mixer_flags, voice.buffers.mix_state);
    if (voice.strong_right || voice.strong_left)
    {
        const uint32_t dry_target = voice.strong_right ? kLeft : kRight;
        const uint32_t wet_target = voice.strong_right ? kWetLeft : kWetRight;
        list.SetBuffer(0, 0, 0, count);
        list.Mixer(0, 0x8000, kStrongDry, dry_target);
        if (use_wet)
        {
            list.Mixer(0, 0x8000, kStrongWet, wet_target);
        }
    }
}

void EadEarlyPlayer::BuildHeadset(AudioList& list, Voice& voice, int length, int flags, int headset)
{
    uint32_t target = 0;
    uint32_t shift = 0;
    uint32_t previous_shift = 0;
    switch (headset)
    {
    case 1:
        target = kLeft;
        shift = voice.headset_right;
        voice.previous_headset_left = 0;
        previous_shift = voice.previous_headset_right;
        voice.previous_headset_right = static_cast<uint16_t>(shift);
        break;
    case 2:
        target = kRight;
        shift = voice.headset_left;
        voice.previous_headset_right = 0;
        previous_shift = voice.previous_headset_left;
        voice.previous_headset_left = static_cast<uint16_t>(shift);
        break;
    default:
        return;
    }

    const auto bytes = static_cast<uint32_t>(length);
    if (flags != kFlagInit)
    {
        if (previous_shift == 0)
        {
            list.DmemMove(kPanTemp, kTemp, 8);
            list.ClearBuffer(8, 8);
            list.DmemMove(kPanTemp, kTemp + 0x10, 0x10);
            list.SetBuffer(0, 0, kTemp, 32);
            list.SaveBuffer(voice.buffers.pan_resample);
            const auto pitch = static_cast<uint16_t>((bytes << 15) / (shift + bytes - previous_shift + 8));
            list.SetBuffer(0, kPanTemp + 8, kTemp, shift + bytes - previous_shift);
            list.Resample(0, pitch, voice.buffers.pan_resample);
        }
        else
        {
            const auto pitch = static_cast<uint16_t>(shift == 0 ? (bytes << 15) / (bytes - previous_shift - 4)
                                                                : (bytes << 15) / (bytes + shift - previous_shift));
            list.SetBuffer(0, kPanTemp, kTemp, shift + bytes - previous_shift);
            list.Resample(0, pitch, voice.buffers.pan_resample);
        }

        if (previous_shift != 0)
        {
            list.SetBuffer(0, kPanTemp, 0, previous_shift);
            list.LoadBuffer(voice.buffers.pan_samples);
            list.DmemMove(kTemp, kPanTemp + previous_shift, shift + bytes - previous_shift);
        }
        else
        {
            list.DmemMove(kTemp, kPanTemp, shift + bytes - previous_shift);
        }
    }
    else
    {
        list.DmemMove(kPanTemp, kTemp, bytes);
        list.DmemMove(kTemp, kPanTemp + shift, bytes);
        list.ClearBuffer(kPanTemp, shift);
    }

    if (shift != 0)
    {
        list.SetBuffer(0, 0, kPanTemp + bytes, shift);
        list.SaveBuffer(voice.buffers.pan_samples);
    }

    list.SetBuffer(0, 0, 0, bytes);
    list.Mixer(0, 0x7FFF, kPanTemp, target);
}

} // namespace ntdlib::n64

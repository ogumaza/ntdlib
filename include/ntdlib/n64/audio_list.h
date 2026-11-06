// SPDX-License-Identifier: MIT

// Audio command lists of the Nintendo 64. The CPU builds a list of 64-bit commands for each stretch of output. The
// RSP's audio microcode runs the list on buffers in DMEM. This header holds the commands of libultra's standard
// microcode, in the form of the SDK's public abi.h, and a list that builds them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::n64
{

constexpr uint8_t kAudioNoop = 0;
constexpr uint8_t kAudioAdpcm = 1;
constexpr uint8_t kAudioClearBuffer = 2;
constexpr uint8_t kAudioEnvMixer = 3;
constexpr uint8_t kAudioLoadBuffer = 4;
constexpr uint8_t kAudioResample = 5;
constexpr uint8_t kAudioSaveBuffer = 6;
constexpr uint8_t kAudioSegment = 7;
constexpr uint8_t kAudioSetBuffer = 8;
constexpr uint8_t kAudioSetVolume = 9;
constexpr uint8_t kAudioDmemMove = 10;
constexpr uint8_t kAudioLoadAdpcm = 11;
constexpr uint8_t kAudioMixer = 12;
constexpr uint8_t kAudioInterleave = 13;
constexpr uint8_t kAudioPoleFilter = 14;
constexpr uint8_t kAudioSetLoop = 15;

constexpr uint8_t kAudioFlagInit = 0x01;
constexpr uint8_t kAudioFlagLoop = 0x02;
constexpr uint8_t kAudioFlagLeft = 0x02;
constexpr uint8_t kAudioFlagVolume = 0x04;
constexpr uint8_t kAudioFlagAux = 0x08;

struct AudioCommand
{
    friend bool operator==(const AudioCommand&, const AudioCommand&) = default;

    uint32_t w0 = 0;
    uint32_t w1 = 0;

    uint8_t Opcode() const
    {
        return static_cast<uint8_t>(w0 >> 24);
    }
};

// A place between a list's commands. The microcodes never see a mark. A host's player stops at the marks to mute and
// meter a sequence's tracks (see FramePlayer).
struct AudioMark
{
    static constexpr int kNoTrack = -1;

    std::size_t command = 0; // the mark comes before this command
    int track = kNoTrack;    // whose voice the commands up to the next mark belong to
    bool chunk_end = false;  // all of a chunk's voices have gone by
    bool voice = false;      // the commands up to the next mark belong to a voice, whatever its track
};

// A list that the builders append to.
class AudioList
{
public:
    void Clear()
    {
        commands.clear();
        marks.clear();
    }

    // The commands from here on are a voice's, of track `track`, or kNoTrack.
    void MarkVoice(int track)
    {
        marks.push_back({commands.size(), track, false, true});
    }

    void MarkVoicesEnd()
    {
        marks.push_back({commands.size(), AudioMark::kNoTrack, false, false});
    }

    void MarkChunkEnd()
    {
        marks.push_back({commands.size(), AudioMark::kNoTrack, true, false});
    }

    void Adpcm(uint8_t flags, uint32_t state)
    {
        Add(kAudioAdpcm, flags, 0, state);
    }

    void ClearBuffer(uint32_t dmem, uint32_t count)
    {
        commands.push_back({Word(kAudioClearBuffer) | (dmem & 0xFFFFFF), count});
    }

    void EnvMixer(uint8_t flags, uint32_t state)
    {
        Add(kAudioEnvMixer, flags, 0, state);
    }

    void LoadBuffer(uint32_t address)
    {
        Add(kAudioLoadBuffer, 0, 0, address);
    }

    void Resample(uint8_t flags, uint32_t pitch, uint32_t state)
    {
        Add(kAudioResample, flags, pitch, state);
    }

    void SaveBuffer(uint32_t address)
    {
        Add(kAudioSaveBuffer, 0, 0, address);
    }

    void Segment(uint8_t segment, uint32_t base)
    {
        Add(kAudioSegment, 0, 0, static_cast<uint32_t>(segment) << 24 | (base & 0xFFFFFF));
    }

    void SetBuffer(uint8_t flags, uint32_t in, uint32_t out, uint32_t count)
    {
        Add(kAudioSetBuffer, flags, in, (out << 16) | (count & 0xFFFF));
    }

    void SetVolume(uint8_t flags, uint32_t volume, uint32_t target, uint32_t rate)
    {
        Add(kAudioSetVolume, flags, volume, (target << 16) | (rate & 0xFFFF));
    }

    void SetLoop(uint32_t address)
    {
        Add(kAudioSetLoop, 0, 0, address);
    }

    void DmemMove(uint32_t in, uint32_t out, uint32_t count)
    {
        commands.push_back({Word(kAudioDmemMove) | (in & 0xFFFFFF), (out << 16) | (count & 0xFFFF)});
    }

    void LoadAdpcm(uint32_t count, uint32_t address)
    {
        commands.push_back({Word(kAudioLoadAdpcm) | (count & 0xFFFFFF), address});
    }

    void Mixer(uint8_t flags, uint32_t gain, uint32_t in, uint32_t out)
    {
        Add(kAudioMixer, flags, gain, (in << 16) | (out & 0xFFFF));
    }

    void Interleave(uint32_t left, uint32_t right)
    {
        Add(kAudioInterleave, 0, 0, (left << 16) | (right & 0xFFFF));
    }

    void PoleFilter(uint8_t flags, uint32_t gain, uint32_t state)
    {
        Add(kAudioPoleFilter, flags, gain, state);
    }

    std::vector<AudioCommand> commands;
    std::vector<AudioMark> marks;

private:
    static constexpr uint32_t Word(uint8_t opcode)
    {
        return static_cast<uint32_t>(opcode) << 24;
    }

    void Add(uint8_t opcode, uint8_t flags, uint32_t low, uint32_t w1)
    {
        commands.push_back({Word(opcode) | (static_cast<uint32_t>(flags) << 16) | (low & 0xFFFF), w1});
    }
};

} // namespace ntdlib::n64

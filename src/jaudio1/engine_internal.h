// SPDX-License-Identifier: MIT

// Definitions that the parts of jaudio1's engine share (engine.cpp, engine_effects.cpp and sound_files.cpp): the
// DSP's memory, JAI's numbers of tracks, the port commands' levels, an effect's levels and the model of one piece.
// The three files build as one translation unit. The definitions in the unnamed namespace therefore keep internal
// linkage. Only those files include this header.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ntdlib/jaudio1/calc.h"
#include "ntdlib/jaudio1/channel.h"
#include "ntdlib/jaudio1/conductor.h"
#include "ntdlib/jaudio1/engine.h"
#include "ntdlib/jaudio1/track.h"
#include "ntdlib/jaudio2/dsp.h"
#include "ntdlib/jaudio2/tables.h"

namespace ntdlib::jaudio1
{
namespace
{

// The DSP's memory below the waves: the 64 channels' blocks, the FX lines' settings, the outputs of a frame's 7
// sub-frames, and the FX lines' buffers.
constexpr uint32_t kBlocks = 0x0000;
constexpr uint32_t kFxSettings = 0x6000;
constexpr uint32_t kFxLineBytes = 0x20;
constexpr uint32_t kLeft = 0x6100;
constexpr uint32_t kRight = 0x6800;
constexpr uint32_t kFxBuffers = 0x8000;
constexpr uint32_t kFxBufferBytes = SoundFiles::kMaxFxSubframes * 0xa0;
static_assert(kFxBuffers + 4 * kFxBufferBytes <= SoundFiles::kWaves);

// JAI's number for the root track of a sequence (JAIGlobalParameter's seqTrackMax). JAI numbers a child of the root
// track by its slot.
constexpr uint32_t kRootNumber = 32;
constexpr uint32_t kChildNumbers = 16;

// The levels that a port command sets (the flags of JASPortArgs).
constexpr uint8_t kPortVolume = 0x01;
constexpr uint8_t kPortPitch = 0x02;
constexpr uint8_t kPortPan = 0x04;
constexpr uint8_t kPortFxmix = 0x08;
constexpr uint8_t kPortDolby = 0x10;
constexpr uint8_t kPortTempo = 0x80;

// With this bit in the sound table's format for the sequences, a piece's volume over 127 gives line 1 of its volume
// (JAIBasic::setSeExtParameter).
constexpr uint8_t kFormatVolume = 4;

// The effects' settings at the listener. The engine starts an effect as the game starts one with no place in its world
// (PSSystem::SysIF::playSystemSe). PSGame starts JAI with 2 audio cameras: JAI then takes no distance, Doppler shift or
// distance send for an effect. The effects' default send (seDefaultFx) and the map's settings stay 0. JAI starts an
// effect that has no actor with a default actor. The default actor has no object. The actor's flag marks the effect as
// played with an actor. PSM::SeSound then gives the effect a send of 0.07 (CreaturePrm::cSeFxMix) and a pan of 0.5. The
// scene's send and the camera's distance volume go only to an effect without that flag, or to one with an object. The
// levels move over 5 frames as the distance changes (PSSystem's sDistanceParameterMoveTime).
constexpr float kEffectSend = 0.07f;
constexpr float kListenerPan = 0.5f;
constexpr float kSurroundCenter = 30.0f;
constexpr uint32_t kDistanceTime = 5;

// A level that moves to a target over a number of the game's frames (JAInter::MoveParaSet). A level that takes its
// value at once keeps a move in progress: the move then carries it away again until the move's end.
struct MoveLevel
{
    float value = 1.0f;
    float target = 1.0f;
    float amount = 0.0f;
    uint32_t count = 0;

    // set.
    void Set(float level, uint32_t time)
    {
        if ((count == 0 && value == level) || (count != 0 && target == level))
        {
            return;
        }

        target = level;
        if (time == 0)
        {
            value = level;
            return;
        }
        amount = (value - target) / static_cast<float>(time + 1);
        count = time + 1;
    }

    // move.
    void Move()
    {
        if (count == 0)
        {
            return;
        }

        if (--count != 0)
        {
            value -= amount;
        }
        else
        {
            value = target;
        }
    }
};

// The lines of an effect's levels (JAInter's sound parameters). While line 7 holds -1, a level is the product or the
// sum of lines 0 to 6. Otherwise line 7 overrides them.
using Lines = std::array<MoveLevel, 8>;
constexpr std::size_t kLineDoppler = 1;
constexpr std::size_t kLineMap = 3;
constexpr std::size_t kLineDistance = 4;
constexpr std::size_t kLineOverride = 7;

[[maybe_unused]] Lines MakeLines(float value)
{
    Lines lines;
    for (MoveLevel& line : lines)
    {
        line.value = value;
        line.target = value;
    }
    lines[kLineOverride].value = -1.0f;
    lines[kLineOverride].target = -1.0f;

    return lines;
}

// An effect on the effect sequence (a JAISe of PSM::SeSound's class, without an actor), from the game's call until JAI
// releases it: its sound info, its slot track and the slot's route for the ports, its state in JAInter::SeMgr, its
// levels' lines, and the levels that its slot track last took (SeMgr::TrackUpdate).
struct Effect
{
    enum State : uint8_t
    {
        kStored,
        kLoaded,
        kReady,
        kPlaying,
        kInactive,
    };

    uint32_t id = 0;
    SoundInfo info;
    uint32_t slot = 0;
    uint32_t slots = 0; // the slot tracks of the scene with the most (JAI's seTrackMax)
    uint32_t route = 0;
    State state = kStored;
    Lines volumes = MakeLines(1.0f);
    Lines pans = MakeLines(0.5f);
    Lines pitches = MakeLines(1.0f);
    Lines fxmixes = MakeLines(0.0f);
    Lines dolbys = MakeLines(kSurroundCenter / 127.0f);
    uint8_t pitch_spread = 0;     // mRandPitchModifier
    float distance_offset = 0.0f; // SeSound's mDistanceModifier
    float volume = 1.0f;
    float pitch = 1.0f;
    float fxmix = 0.0f;
    float pan = 0.5f;
    float dolby = 0.0f;
    bool stop = false; // the interrupt switch that releaseSeRegist leaves for the sequence manager

    // JAI's generator (JAInter::Const::random) and JALCalc's (getRandom_0_1). Each starts from its value at the
    // game's start.
    RandomFast jai_random;
    RandomFast jal_random;
};

} // namespace

// The model of one piece: the channels, the tracks and the DSP, and JAI's state for the piece's sequence.
struct Engine::Model
{
    explicit Model(DspMemory& memory) : voices(kBlocks), tracks(voices), dsp(memory, jaudio2::DspTables())
    {
    }

    Voices voices;
    Tracks tracks;
    jaudio2::Dsp dsp;
    uint32_t root = Track::kNone;
    OuterParam root_outer;
    PieceSettings piece;

    // JAI's levels for the piece (JAInter::SeqUpdateData): the product of the volume's lines, and a port command for
    // the root track and for each child by its number. A command waits in JASPortCmd's list for the next sub-frame. A
    // command that is already in the list keeps its place and takes the new levels.
    struct PortArgs
    {
        float volume = 0.0f;
        float pitch = 0.0f;
        float fxmix = 0.0f;
        float pan = 0.0f;
        float dolby = 0.0f;
        float tempo = 0.0f;
        uint8_t flags = 0;
        bool interrupt = false; // with flag 0x40: the command requests the track's stop (the arguments' word 9)
        bool queued = false;
    };
    float volume = 1.0f;
    std::array<PortArgs, kRootNumber + 1> ports{};
    std::vector<uint32_t> port_queue;

    // The children that have asked for their levels (the mask at +4 of SeqUpdateData), and whether JAI has found the
    // piece playing (SOUNDSTATE_Playing).
    uint32_t asked = 0;
    bool started = false;

    // The sub-frames between the game's frames, and the sub-frame at which the next frame falls due.
    double frame_subframes = 0.0;
    double next_frame = 0.0;

    // The root track's tempo when its object took the track (SeqTrackRoot's standard tempo), and whether the root
    // track has asked for an odd number of beats since then (bit 0 of BeatMgr's flags).
    uint16_t standard_tempo = 120;
    bool odd_beat = false;

    // An AutoBgm piece's conductor.
    std::unique_ptr<Conductor> conductor;

    // The effect on the effect sequence, or null for a piece of music.
    std::unique_ptr<Effect> effect;

    // Each horn's place in its tune (Rappa's table index).
    std::array<uint16_t, 2> horn_places{};

    uint32_t unmodelled = 0;
    uint64_t subframe = 0;
};

} // namespace ntdlib::jaudio1

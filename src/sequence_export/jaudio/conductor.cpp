// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/jaudio/conductor.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "xsflib/binary.h"

namespace ntdlib::sequence_export::jaudio
{

namespace
{

// The bytes that the game skips after each set of parameters (JADUtility::PrmSetBase::load, 0x800ba610): 17 after the
// conductor's (0x8033d5c4), 3 before and after each track's (0x80339718 and 0x80339760), and 21 after each module's
// (0x8033a378).
constexpr std::size_t kAfterConductor = 17;
constexpr std::size_t kAroundTrack = 3;
constexpr std::size_t kAfterModule = 21;

// The bytes of a track's parameters: the count of its modules, its start (16 bits), its half beat, its name of 8
// bytes, the track that it follows and its effect send.
constexpr std::size_t kTrackBytes = 14;

// The bytes of a module's parameters (see ConductorModule).
constexpr std::size_t kModuleBytes = 19;

} // namespace

Conductor ReadConductor(std::span<const uint8_t> cnd)
{
    // Each parameter is a byte or a 16-bit value, and a name is 8 bytes. The conductor's parameters come in the order
    // of its list: the poll, the tempo, then the count of its tracks (JADUtility::PrmSetRc::load, 0x8033cf38).
    const xsflib::Reader r(cnd, true);
    Conductor conductor;
    conductor.poll = r.U8(0);
    conductor.tempo = r.U8(1);
    const uint8_t tracks = r.U8(2);
    std::size_t at = 3 + kAfterConductor;
    for (uint8_t t = 0; t < tracks; t++)
    {
        at += kAroundTrack;
        ConductorTrack track;
        const uint8_t modules = r.U8(at);
        track.start_beats = r.S16(at + 1);
        track.half_beat = r.U8(at + 3);
        const std::span<const uint8_t> name = r.Sub(at + 4, 8);
        for (const uint8_t c : name)
        {
            if (c == 0)
            {
                break;
            }
            track.name += static_cast<char>(c);
        }
        track.follows = r.U8(at + 12);
        track.send = r.U8(at + 13);
        at += kTrackBytes + kAroundTrack;

        for (uint8_t m = 0; m < modules; m++)
        {
            ConductorModule module;
            module.start_beats = r.S16(at);
            module.beats = r.S16(at + 2);
            module.in_order = r.U8(at + 5);
            module.bank = r.U8(at + 6);
            module.program = r.U8(at + 7);
            module.phrase_list = r.S16(at + 8);
            module.length_list = r.S16(at + 10);
            module.phrases = r.U8(at + 12);
            module.rests = r.U8(at + 13);
            module.transpose = r.S16(at + 14);
            module.volume = r.U8(at + 16);
            module.pan = r.U8(at + 17);
            module.half_beat = r.U8(at + 18);
            track.modules.push_back(module);
            at += kModuleBytes + kAfterModule;
        }
        conductor.tracks.push_back(std::move(track));
    }

    return conductor;
}

} // namespace ntdlib::sequence_export::jaudio

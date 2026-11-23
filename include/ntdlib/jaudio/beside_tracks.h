// SPDX-License-Identifier: MIT

// The tracks that the length analysis of a JAudio game's sound leaves out (game_lengths.h): those of a sound that a
// rip's driver plays beside the ripped one, and those of the effects that play beside a chord layer's music. The
// analysis watches the game's sequence parser on a CPU. SkippingCpu hides from it the parser's events at the places
// that a test picks. ParsedTrack and IsTrackBeside make that test. `word(address)` reads a word of the game's memory.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/game_lengths.h"

namespace ntdlib::jaudio
{

// The most levels of parents that the tests of a track climb.
inline constexpr int kMaxTrackDepth = 16;

// A CPU as the analysis reads it: the registers, and the watch of the game's code. The watch leaves out the events at
// the places for which `skip` returns true. `Cpu` is a CPU such as ntdlib::ppc::Cpu: its registers are `state_`, and
// Watch(pcs, on_watch) calls `on_watch` at each of `pcs`. The registers keep that name. The analysis reads them by it.
template <typename Cpu>
class SkippingCpu
{
public:
    SkippingCpu(Cpu& cpu, std::function<bool(uint32_t pc)> skip) : state_(cpu.state_), cpu_(cpu), skip_(std::move(skip))
    {
    }

    // Watches `pcs` as the CPU does. An empty `on_watch` clears the watch.
    void Watch(std::vector<uint32_t> pcs, std::function<void(uint32_t pc)> on_watch)
    {
        if (!on_watch)
        {
            cpu_.Watch(std::move(pcs), nullptr);
            return;
        }

        cpu_.Watch(std::move(pcs),
                   [this, callback = std::move(on_watch)](uint32_t pc)
                   {
                       if (!skip_(pc))
                       {
                           callback(pc);
                       }
                   });
    }

    decltype(std::declval<Cpu&>().state_)& state_;

private:
    Cpu& cpu_;
    std::function<bool(uint32_t pc)> skip_;
};

// The track that the watched instruction at `pc` parses, from the registers `gpr`: at the parser's command, at its
// jump, or at JAudio2's conditional jump, in the register that the watch names for each. The analysis reads the same
// registers. Returns nothing at any other `pc`.
template <typename Registers>
std::optional<uint32_t> ParsedTrack(const GameWatch& watch, uint32_t pc, const Registers& gpr)
{
    const bool conditional = watch.conditional_jump != 0 && pc == watch.conditional_jump;
    if (pc != watch.parse_command && pc != watch.jump_handler && !conditional)
    {
        return std::nullopt;
    }

    const uint32_t reg = pc == watch.parse_command ? watch.command_track_register
                         : conditional             ? watch.conditional_track_register
                                                   : watch.jump_track_register;
    return gpr[reg];
}

// The root of `track`: the track that its parents lead to, each parent at `parent_offset` in its child. Returns 0 if
// the parents go deeper than kMaxTrackDepth levels.
template <typename Word>
uint32_t RootTrackOf(uint32_t track, uint32_t parent_offset, const Word& word)
{
    for (int level = 0; track != 0 && level < kMaxTrackDepth; level++)
    {
        const uint32_t parent = word(track + parent_offset);
        if (parent == 0)
        {
            return track;
        }

        track = parent;
    }

    return 0;
}

// Whether `track` belongs to an effect: its root track is an effect's, in a JAISound with the effects' vtable.
template <typename Word>
bool IsEffectTrack(const GameWatch& watch, uint32_t track, uint32_t parent_offset, const Word& word)
{
    const uint32_t root = RootTrackOf(track, parent_offset, word);
    return root != 0 && word(root - watch.effect_track_offset + watch.vtable_offset) == watch.effect_vtable;
}

// Whether `track` belongs to the sound beside the ripped one, whose JAISound is at `sound` (0 for none): the
// sequence's root track, or a track whose parents lead to it within kMaxTrackDepth levels. While `sound` isn't a
// playing sequence, a track of an effect belongs to it when `effects` is true, as a chord layer's effects do.
template <typename Word>
bool IsTrackBeside(const GameWatch& watch, uint32_t sound, uint32_t track, uint32_t parent_offset, bool effects,
                   const Word& word)
{
    if (sound == 0 || word(sound + watch.vtable_offset) != watch.sequence_vtable)
    {
        return effects && IsEffectTrack(watch, track, parent_offset, word);
    }

    const uint32_t root = sound + watch.sequence_track_offset;
    uint32_t t = track;
    for (int level = 0; t != 0 && level < kMaxTrackDepth; level++)
    {
        if (t == root)
        {
            return true;
        }

        t = word(t + parent_offset);
    }

    return false;
}

} // namespace ntdlib::jaudio

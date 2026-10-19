// SPDX-License-Identifier: MIT

// The length analysis of a JAudio2 or JAudio1 game's sound in game mode, on a machine that runs the game's code on
// ntdlib's PowerPC CPU with a driver that has started the sound. The analysis watches the game's sequence parser
// (LoopFinder) and the output. It can also find the waves that the sound's notes look up, for a game that loads its
// wave groups as its scenes require them (WaveGroups then picks the groups).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/loop_finder.h"
#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/rip/length.h"

namespace ntdlib::jaudio
{

// Where a game's JAudio keeps what the length analysis reads: functions of the game's code, and offsets in its objects.
// A game's profile holds them. A JAudio1 game also sets the fields at the end.
struct GameWatch
{
    // The sequence parser's functions. `parse_command` runs once for each command of each track, with the track in
    // register `command_track_register`. `jump_handler` runs as the parser takes a jump, with the track in register
    // `jump_track_register`. In JAudio2 they're JASSeqParser::parse and the jump command (0xC7). There, register
    // `jump_target_register` points at the list of the jump's arguments, and the jump's target is the first argument.
    uint32_t parse_command = 0;
    uint32_t jump_handler = 0;
    uint32_t command_track_register = 4;
    uint32_t jump_track_register = 4;
    uint32_t jump_target_register = 5;

    // JAudio2's conditional jump (0xC8) takes its jump at `conditional_jump`. There, register
    // `conditional_track_register` holds the track, and register `conditional_arguments_register` points at the list of
    // the jump's arguments: the condition, then the target. With 0 there, the analysis doesn't watch conditional jumps.
    // A track that loops through one then never loops for the analysis. Each time round, its commands at the loop's
    // start count as moves, and they hold back the loops of the tracks after it. JAudio1 leaves these 0: its jump
    // command has a condition, and the watch at `jump_handler` already covers those jumps.
    uint32_t conditional_jump = 0;
    uint32_t conditional_track_register = 30;
    uint32_t conditional_arguments_register = 31;

    // A track's sequence reader is at `reader_offset` in the track. The reader starts with the data's start (+0) and
    // the position (+4). With `relative_positions`, the position is an offset from the data's start, as in an earlier
    // JAudio1 such as Luigi's Mansion's. Otherwise it's an address. The track's stack depth is at `depth_offset`.
    //
    // A JAudio1 track can run an interrupt routine. Pikmin 2's root tracks do. While a track is in the routine, a word
    // of the track holds the position to return to. Otherwise the word is 0. Two fields can give the word's offset.
    // They set different rules for the routine's commands. With `interrupt_offset`, the analysis counts them one level
    // deeper. The commands in a call count the same way. With `interrupt_return_offset` (below), the analysis leaves
    // them out. A field set to 0 turns its rule off.
    uint32_t reader_offset = 0;
    uint32_t depth_offset = 8;
    bool relative_positions = false;
    uint32_t interrupt_offset = 0;

    // With `idle_in_calls`, a track can idle in a loop inside a call (see LoopFinder).
    bool idle_in_calls = false;

    // A sequence's and an effect's JAISound, by their vtables, and where each holds its root track. The handle points
    // at the JAISound. Its vtable pointer is at `vtable_offset`: 0 where the compiler puts it first, after the members
    // where it puts it last.
    uint32_t vtable_offset = 0;
    uint32_t sequence_vtable = 0;
    uint32_t sequence_track_offset = 0;
    uint32_t effect_vtable = 0;
    uint32_t effect_track_offset = 0;

    // Where a track's sequence control holds the ticks left to wait, and whether the track waits for its notes.
    uint32_t wait_offset = 0;
    uint32_t note_wait_offset = 0;

    // For wave discovery: the functions with which a note looks up its wave, with the wave bank in r3 and the wave's id
    // in r4. Wave bank n is at word(word(wave_bank_table) + wave_bank_table_offset + 4 * n). The table's object holds
    // the number of banks at `wave_bank_count_offset`. With 0 there, the walk reads kMaxWaveBanks entries. A game whose
    // driver loads every wave bank leaves them all 0.
    uint32_t wave_handle_functions[2] = {};
    uint32_t wave_bank_table = 0;
    uint32_t wave_bank_table_offset = 0;
    uint32_t wave_bank_count_offset = 0;

    // A game whose sound library is JAudio1 sets `jaudio1`. The parser's functions are then the loop over a track's
    // commands and the place in the jump command (0xC8) where the parser takes the jump. There, register
    // `jump_target_register` holds the jump's target itself. A command from 0xb0 to 0xbf runs the command after it. A
    // track that waits for its notes waits -1 ticks. The analysis doesn't read the JAISounds' vtables or
    // `note_wait_offset`. A track with at least `endless_wait` ticks left to wait counts as a track that waits for its
    // notes. Luigi's Mansion's effects hold a note that way until the game stops them. With 0 there, every wait counts
    // down to a move.
    //
    // A JAudio1 sound's root track comes from its ID. A sequence's JAISound points to its parameters at
    // `parameters_offset`. In The Wind Waker, the root track lies in the parameters at `parameters_track_offset`. An
    // earlier JAudio1, such as Luigi's Mansion's, takes its tracks from a pool. A table of `root_track_count` words at
    // `root_tracks` then holds the root track of each sequence that plays. The parameters hold the number of the
    // sequence's root track in that table, as a word at `parameters_track_offset`. With 0 at `root_tracks`, the root
    // track lies in the parameters. An effect plays in a track of the effects sequence. The word at `effects_sequence`
    // points to that sequence's JAISound. A game whose rips play no effects can leave `effects_sequence` 0. The
    // effect's JAISound holds the effect's slot in its byte at `effect_slot_offset`: 0xff until the sound library gives
    // the effect a slot. The slot's high nibble picks a child of the effects sequence's root track, and its low nibble
    // picks a child of that track. A track holds its parent at `parent_offset` and its 16 children from
    // `children_offset`. The effects sequence plays all along. The analysis takes only the events of the sound's
    // tracks, by each track's parents.
    //
    // A later JAudio1, such as Pikmin 2's, keeps a sequence's parameters in the sequence's JAISound. With
    // `parameters_inline`, the parameters start at `parameters_offset` in the JAISound. The root track then lies in
    // them at `parameters_track_offset`.
    bool jaudio1 = false;
    uint32_t parameters_offset = 0;
    bool parameters_inline = false;
    uint32_t parameters_track_offset = 0;
    uint32_t root_tracks = 0;
    uint32_t root_track_count = 0;
    uint32_t effects_sequence = 0;
    uint32_t effect_slot_offset = 0;
    uint32_t parent_offset = 0;
    uint32_t children_offset = 0;
    uint32_t endless_wait = 0;

    // The other rule for a JAudio1 track's interrupt routine (see `interrupt_offset`). With the offset of the word that
    // holds the return position here, the analysis leaves out the routine's commands and jumps. They aren't the
    // track's moves. In Super Mario Sunshine, an effect holds a note while a timer routine polls the game every 5
    // ticks. With 0 there, the routine's commands count as the track's.
    uint32_t interrupt_return_offset = 0;
};

// The analysis's rules, in the output's frames and in milliseconds. A sample louder than 8 of 32,767 (-72 dB) is sound
// (xsflib::rip::Audible). A sound plays for at most `limit_ms`. One that stays busy without a sound for `silence_ms`
// ended at its last sound. A sound that ends plays until `tail_ms` after its last sound. A loop gets two passes and a
// fade of `loop_fade_ms`. A sound that holds loops there if it has sounded in the last `holding_ms`.
struct GameLengthRules
{
    uint32_t sample_rate = 32028;  // the output's frames a second
    std::size_t block_frames = 32; // the frames that the analysis renders at a time
    long long limit_ms = 600'000;
    long long silence_ms = 180'000;
    long long tail_ms = 500;
    long long loop_fade_ms = 10'000;
    long long holding_ms = 1'000;
};

// The wave banks that wave discovery searches the table for.
inline constexpr uint32_t kMaxWaveBanks = 256;

// The analysis's reads of the game's memory, through the machine's adapter (see AnalyzeGameSound).
namespace detail
{

// Whether `pc` is the place where JAudio2's conditional jump takes its jump.
inline bool IsConditionalJump(const GameWatch& watch, uint32_t pc)
{
    return watch.conditional_jump != 0 && pc == watch.conditional_jump;
}

// What a track did at the sequence parser's watched place `pc`, in frame `frame`: it started a command, or it took a
// jump. Returns nothing for a JAudio1 track in its interrupt routine with `interrupt_return_offset`. With
// `interrupt_offset`, the event of a track in its interrupt routine is one level deeper than the track's depth.
template <typename Machine>
std::optional<TrackEvent> ParserEvent(Machine& machine, const GameWatch& watch, uint32_t pc, uint64_t frame)
{
    const auto& s = machine.Cpu().state_;
    const bool conditional = IsConditionalJump(watch, pc);
    const auto kind = pc == watch.parse_command ? TrackEvent::Kind::kCommand : TrackEvent::Kind::kJump;
    const uint32_t track = s.gpr[kind == TrackEvent::Kind::kCommand ? watch.command_track_register
                                 : conditional                      ? watch.conditional_track_register
                                                                    : watch.jump_track_register];
    if (watch.jaudio1 && watch.interrupt_return_offset && machine.Word(track + watch.interrupt_return_offset) != 0)
    {
        return std::nullopt;
    }

    const uint32_t reader = track + watch.reader_offset;
    const uint32_t start = machine.Word(reader);
    const uint32_t position = machine.Word(reader + 4) + (watch.relative_positions ? start : 0);
    uint8_t command = machine.Byte(position).value_or(0);
    if (watch.jaudio1 && command >= 0xb0 && command <= 0xbf)
    {
        command = machine.Byte(position + 1).value_or(0);
    }

    uint32_t to = 0;
    if (conditional)
    {
        to = machine.Word(s.gpr[watch.conditional_arguments_register] + 4);
    }
    else if (kind == TrackEvent::Kind::kJump)
    {
        const uint32_t target = s.gpr[watch.jump_target_register];
        to = watch.jaudio1 ? target : machine.Word(target);
    }

    TrackEvent event = {kind, track, position - start, to, machine.Word(track + watch.depth_offset), command, frame};
    if (watch.interrupt_offset && machine.Word(track + watch.interrupt_offset))
    {
        event.depth++;
    }

    return event;
}

// A JAudio1 track's parents, nearest first, up to four levels. The list ends with 0s at a track without a parent.
template <typename Machine>
std::array<uint32_t, 4> Parents(Machine& machine, const GameWatch& watch, uint32_t track)
{
    std::array<uint32_t, 4> up = {};
    uint32_t at = track;
    for (std::size_t level = 0; level < up.size() && at; level++)
    {
        at = up[level] = machine.Word(at + watch.parent_offset);
    }

    return up;
}

// The number of the wave bank at `bank` in the table of wave banks, or nothing for a bank that the table doesn't hold.
template <typename Machine>
std::optional<uint16_t> WaveBankNumber(Machine& machine, const GameWatch& watch, uint32_t bank)
{
    const uint32_t base = machine.Word(watch.wave_bank_table);
    const uint32_t table = base + watch.wave_bank_table_offset;
    const uint32_t banks = watch.wave_bank_count_offset
                               ? std::min(machine.Word(base + watch.wave_bank_count_offset), kMaxWaveBanks)
                               : kMaxWaveBanks;
    for (uint32_t n = 0; n < banks; n++)
    {
        if (machine.Word(table + 4 * n) == bank)
        {
            return static_cast<uint16_t>(n);
        }
    }

    return std::nullopt;
}

// What a track waits for, from its sequence control.
template <typename Machine>
TrackWait ReadWait(Machine& machine, const GameWatch& watch, uint32_t track)
{
    const auto ticks = static_cast<int32_t>(machine.Word(track + watch.wait_offset));
    if (watch.jaudio1)
    {
        const bool endless =
            watch.endless_wait != 0 && ticks >= 0 && static_cast<uint32_t>(ticks) >= watch.endless_wait;
        return {ticks, ticks < 0 || endless};
    }

    return {ticks, machine.Byte(track + watch.note_wait_offset).value_or(0) != 0};
}

// A JAudio1 sequence's root track, from the parameters of its JAISound at `sequence`, or 0 while it has none.
template <typename Machine>
uint32_t SequenceRootTrack(Machine& machine, const GameWatch& watch, uint32_t sequence)
{
    if (!sequence)
    {
        return 0;
    }

    const uint32_t field = sequence + watch.parameters_offset;
    const uint32_t parameters = watch.parameters_inline ? field : machine.Word(field);
    if (!parameters)
    {
        return 0;
    }
    if (!watch.root_tracks)
    {
        return parameters + watch.parameters_track_offset;
    }

    const uint32_t number = machine.Word(parameters + watch.parameters_track_offset);
    return number < watch.root_track_count ? machine.Word(watch.root_tracks + 4 * number) : 0;
}

// A JAudio1 sound's root track, or 0 while the sound has none. A sequence's parameters give its root track. An
// effect's root track is the track of the effects sequence that the effect's slot picks.
template <typename Machine>
uint32_t JAudio1RootTrack(Machine& machine, const GameWatch& watch, uint32_t handle, uint32_t sound)
{
    // A sequence's root track is never the effects sequence's. In a table of root tracks, a sequence's parameters
    // hold number 0, the effects sequence's, until the sound library has given the sequence its tracks. A game
    // without an effects sequence leaves `effects_sequence` 0.
    const uint32_t effects =
        watch.effects_sequence ? SequenceRootTrack(machine, watch, machine.Word(watch.effects_sequence)) : 0;
    if ((sound & kAafTypeMask) == kAafSequence)
    {
        const uint32_t root = SequenceRootTrack(machine, watch, handle);
        return root != effects ? root : 0;
    }

    constexpr uint8_t kNoSlot = 0xff;
    const uint8_t slot = machine.Byte(handle + watch.effect_slot_offset).value_or(kNoSlot);
    const auto child = static_cast<uint32_t>(slot >> 4);
    const auto grandchild = static_cast<uint32_t>(slot & 0xf);
    const uint32_t parent = slot != kNoSlot && effects ? machine.Word(effects + watch.children_offset + 4 * child) : 0;
    return parent ? machine.Word(parent + watch.children_offset + 4 * grandchild) : 0;
}

// The root track of the sound whose JAISound is at `handle`, or nothing while the sound has none. A JAudio1 sound's
// ID gives the root track. In JAudio2, the vtable of the sound's JAISound distinguishes a sequence from an effect. The
// two hold the root track at different offsets.
template <typename Machine>
std::optional<uint32_t> RootTrack(Machine& machine, const GameWatch& watch, uint32_t handle, uint32_t sound)
{
    if (watch.jaudio1)
    {
        const uint32_t root = JAudio1RootTrack(machine, watch, handle, sound);
        return root ? std::optional<uint32_t>(root) : std::nullopt;
    }

    const uint32_t vtable = machine.Word(handle + watch.vtable_offset);
    const uint32_t offset = vtable == watch.sequence_vtable ? watch.sequence_track_offset
                            : vtable == watch.effect_vtable ? watch.effect_track_offset
                                                            : 0;
    return offset ? std::optional<uint32_t>(handle + offset) : std::nullopt;
}

} // namespace detail

// Plays the sound that the driver has started and works out its length. The machine's adapter, `machine`, provides:
//
// - `std::size_t Render(int16_t* out, std::size_t frames)`: renders stereo frames and returns how many it rendered;
// - `uint32_t Word(uint32_t address)`: the word at a CPU address, or 0 outside memory;
// - `std::optional<uint8_t> Byte(uint32_t address)`: the byte at a CPU address, or nothing outside memory;
// - `Cpu()`: the CPU, with ppc::Cpu's Watch and state_.
//
// `handle_address` is the driver's word that holds the sound's JAISound, 0 once the game has released the sound. The
// waves that the notes look up go in `found` if it isn't null. With `lookups_sound`, each lookup counts as a sound:
// wave discovery plays the sound with no wave group loaded. `sound` is the sound's ID. A JAudio1 game's analysis
// requires it to distinguish a sequence from an effect. Returns nothing when a render fails.
//
// A sound that loops gets two loops and a fade. A sound loops when one of its tracks jumps back to a command it has
// already played, twice to the same command (see LoopFinder). A sound holds when a track idles, every track has
// settled, and nothing has moved for 10 s. It loops there if it still sounds. Any other sound runs until the game
// releases it or until it falls silent for good, for at most the limit. It then ends `tail_ms` after its last sound. A
// sound that never makes a sound is marked silent.
template <typename Machine>
std::optional<xsflib::rip::Timing> AnalyzeGameSound(Machine& machine, uint32_t handle_address, const GameWatch& watch,
                                                    const GameLengthRules& rules, std::set<WaveUse>* found = nullptr,
                                                    bool lookups_sound = false, uint32_t sound = 0)
{
    using xsflib::rip::Timing;
    const auto ms = [&rules](uint64_t frames)
    {
        return FramesToMs(frames, rules.sample_rate);
    };

    // What the sequence parser does, with the frame it happens in: the start of every command of every track, and every
    // jump. A note's lookup of its wave goes in `found`, and counts as a sound for wave discovery. In JAudio1, each
    // event also has its track's parents, nearest first. They reach a sound's root track from tracks up to four levels
    // below it.
    std::vector<TrackEvent> events;
    std::vector<std::array<uint32_t, 4>> parents;
    uint64_t frames = 0;
    std::optional<uint64_t> first_sound;
    uint64_t last_sound = 0;
    const auto on_wave = [&](uint32_t bank, uint32_t id)
    {
        if (const std::optional<uint16_t> number = detail::WaveBankNumber(machine, watch, bank))
        {
            found->insert({*number, static_cast<uint16_t>(id)});
        }

        if (lookups_sound)
        {
            last_sound = frames;
            first_sound = first_sound.value_or(frames);
        }
    };

    const auto on_watch = [&](uint32_t pc)
    {
        if (pc != watch.parse_command && pc != watch.jump_handler && !detail::IsConditionalJump(watch, pc))
        {
            const auto& s = machine.Cpu().state_;
            on_wave(s.gpr[3], s.gpr[4]);
            return;
        }

        const std::optional<TrackEvent> event = detail::ParserEvent(machine, watch, pc, frames);
        if (!event)
        {
            return;
        }

        events.push_back(*event);
        if (watch.jaudio1)
        {
            parents.push_back(detail::Parents(machine, watch, event->track));
        }
    };

    std::vector<uint32_t> watched = {watch.parse_command, watch.jump_handler};
    if (watch.conditional_jump)
    {
        watched.push_back(watch.conditional_jump);
    }

    for (const uint32_t function : watch.wave_handle_functions)
    {
        if (found && function)
        {
            watched.push_back(function);
        }
    }

    machine.Cpu().Watch(std::move(watched), on_watch);

    // Play until the sound ends, loops twice, holds, falls silent for good or reaches the limit.
    std::vector<int16_t> block(rules.block_frames * 2);
    std::optional<LoopFinder> finder;
    uint32_t root = 0;                // the sound's root track, once the finder follows it
    std::optional<uint64_t> released; // the frame at which the game released the sound
    const auto ended = [&]() -> Timing
    {
        if (!first_sound)
        {
            return {rules.tail_ms, 0, true};
        }

        return {ms(last_sound + 1) + rules.tail_ms, 0, false};
    };

    while (ms(frames) < rules.limit_ms)
    {
        events.clear();
        parents.clear();
        if (machine.Render(block.data(), rules.block_frames) != rules.block_frames)
        {
            return std::nullopt;
        }

        for (std::size_t i = 0; i < rules.block_frames; i++)
        {
            if (xsflib::rip::Audible(std::span<const int16_t>(&block[2 * i], 2)))
            {
                last_sound = frames + i;
                first_sound = first_sound.value_or(last_sound);
            }
        }

        // The sound's root track, once the sound exists.
        const uint32_t handle = machine.Word(handle_address);
        if (handle && !finder)
        {
            if (const std::optional<uint32_t> track = detail::RootTrack(machine, watch, handle, sound))
            {
                root = *track;
                finder.emplace(root, rules.sample_rate, watch.jaudio1 ? JAudio1ChangesSound : ChangesSound,
                               watch.idle_in_calls);
            }
        }

        // The tracks' loops. Loops before the first sound don't count.
        const Sounded sounded = {first_sound.has_value(), last_sound};
        for (std::size_t i = 0; i < events.size(); i++)
        {
            const TrackEvent& event = events[i];
            if (watch.jaudio1 && event.track != root &&
                std::find(parents[i].begin(), parents[i].end(), root) == parents[i].end())
            {
                continue;
            }

            const LoopFinder::Verdict verdict = finder ? finder->Take(event, sounded) : LoopFinder::Verdict::kNone;
            if (verdict == LoopFinder::Verdict::kLoops)
            {
                return Timing{ms(event.frame), rules.loop_fade_ms, false};
            }

            if (verdict == LoopFinder::Verdict::kEnded)
            {
                return ended();
            }
        }

        frames += rules.block_frames;
        if (finder)
        {
            finder->TakeWaits([&](uint32_t track) { return detail::ReadWait(machine, watch, track); });
        }

        // The game releases the sound when its tracks have ended. Its notes can still sound after that: the output runs
        // behind the game, and a release fades on. The sound ends once `tail_ms` has passed without a sound, or 10 s
        // after the release.
        if (!handle && finder && !released)
        {
            released = frames;
        }

        if (released &&
            (ms(frames - std::max(*released, last_sound)) >= rules.tail_ms || ms(frames - *released) >= kStillMs))
        {
            return ended();
        }

        // A sound that holds loops where it holds, if it still sounds.
        if (first_sound && finder && finder->Holds(frames))
        {
            if (ms(frames - last_sound) <= rules.holding_ms)
            {
                return Timing{ms(frames), rules.loop_fade_ms, false};
            }

            return ended();
        }

        if (first_sound && ms(frames - last_sound) >= rules.silence_ms)
        {
            return ended();
        }
    }

    if (!first_sound)
    {
        return ended();
    }

    return Timing{rules.limit_ms, rules.loop_fade_ms, false};
}

} // namespace ntdlib::jaudio

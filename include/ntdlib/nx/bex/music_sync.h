// SPDX-License-Identifier: MIT

// Bezel Engine's music sync on the Switch: the marker system of its sound library (libbexsound), which follows the
// beat and the chords of a stream as the stream plays, and the user procedures through which sequences read them.
// Each function's comment gives the address of the routine it follows in Super Mario Party Jamboree's main.
//
// A stream file can hold regions and markers beside its samples (sar/stream_file.h), in the blocks REGN and MRKR. A
// region has a start, an end and a name. A marker has a position and a name. The engine's tools name the markers that
// the music sync reads MRK_BEAT#n/d#t (a beat of n/d time at t beats a minute from there on) and MRK_CHORD#c (chord c
// from there on). The engine plays such a stream a region at a time. Its jump setting lists the stream's region
// sequences: each gives its regions in order and the sequence that follows it.
//
// A sound whose first user parameter has bit 7 set attaches the music sync as it starts (fn_c2e6c, fn_c4fb4). From then
// on, each frame of the game reads the stream's position and updates the sync: the beat that the position has reached,
// eight counters of the bar's subdivisions, and the chord. User procedures 2 to 4 read them for a sequence.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ntdlib::nx::bex
{

// A region of a stream: its first sample, the sample after its last, and its name.
struct StreamRegion
{
    friend bool operator==(const StreamRegion&, const StreamRegion&) = default;

    uint32_t start = 0;
    uint32_t end = 0;
    std::string name;
};

// A marker of a stream: its sample and its name.
struct StreamMarker
{
    friend bool operator==(const StreamMarker&, const StreamMarker&) = default;

    uint32_t position = 0;
    std::string name;
};

// What a stream file holds for the music sync: its sample rate, its regions and its markers, in the file's order.
struct StreamCues
{
    uint32_t sample_rate = 0;
    std::vector<StreamRegion> regions;
    std::vector<StreamMarker> markers;
};

// The regions and markers of the Switch's stream file `file` (FSTM). REGN holds a table of entries of 0x100 bytes from
// its 0x20th byte: each entry's start and end, then its name of up to 64 bytes at 0xC0. MRKR holds a count at its 8th
// byte, then entries of 0x48 bytes: each entry's position, 4 bytes more, then its name of up to 64 bytes. A file
// without either block has none of its items. Throws xsflib::FormatError for a file that isn't a stream file of the
// Switch, or a block that runs past the file's end.
StreamCues ReadStreamCues(std::span<const uint8_t> file);

// A region sequence of a stream's jump setting: its label, the label of the sequence that follows, and its regions'
// names in order.
struct RegionSequence
{
    friend bool operator==(const RegionSequence&, const RegionSequence&) = default;

    std::string label;
    std::string next;
    std::vector<std::string> regions;
};

// The region sequences of the jump setting `file`, in its order. The engine keeps a stream's jump setting as
// MessagePack: a map from the stream's label to an array of maps, each with the keys "label", "param1" (the next
// sequence), "param2", and "param3" on for the regions, where an empty string ends the list. The key "system" holds the
// setting's sizes. Throws xsflib::FormatError for data that isn't such a setting.
std::vector<RegionSequence> ReadJumpSetting(std::span<const uint8_t> file);

// The regions of a stream in the order that the engine plays them, from the first sequence's first region, until a
// sequence comes round again: indices into the cues' regions. `loop` is the place in `regions` where that sequence's
// first region is. The stream loops from the end of the last region back there.
struct RegionOrder
{
    std::vector<std::size_t> regions;
    std::size_t loop = 0;
};

// Throws xsflib::FormatError for a sequence or a region that the setting or the cues lack, or a setting without a
// sequence.
RegionOrder PlayOrder(const StreamCues& cues, const std::vector<RegionSequence>& sequences);

// The music sync's state after a frame: the eight counters that the procedures read (fn_cbdf4), the chord (fn_cbe18),
// and the beat's tempo (fn_cbc80), with whether a beat has started.
struct SyncFrame
{
    std::array<int32_t, 8> counters{};
    uint16_t chord = 0xFFFF;
    bool has_beat = false;
    int16_t tempo = 0;
};

// The music sync of a stream that plays its regions in `order`, from the start of the first region, a frame of
// `frame_samples` samples at a time. The order runs through `passes` times its loop: once for 1, and the loop once more
// for 2. The game attaches the sync as the stream starts (fn_c9870), and takes the stream's settings in its first frame
// (fn_cb0b0). Each later frame takes the stream's position (fn_cb250). A region whose next region doesn't start where
// it ends reports the jump in its first frame (fn_cd2d0, fn_cbe28). The result holds a state for each frame.
std::vector<SyncFrame> RunMusicSync(const StreamCues& cues, const RegionOrder& order, uint32_t frame_samples,
                                    int passes = 2);

// A write of a sequence's variable by a user procedure: the frame from which it waits for the procedure's next call,
// the procedure, and the variable (0 to 15 for the sound's) and its value.
struct ProcedureWrite
{
    friend bool operator==(const ProcedureWrite&, const ProcedureWrite&) = default;

    uint32_t frame = 0;
    uint16_t procedure = 0;
    uint16_t variable = 0;
    int16_t value = 0;
};

// The writes of user procedures 2 to 4 (fn_103b80, with the variables that fn_1039c0 gives them) for a sequence that
// calls procedure 4, then 3, as it starts, and procedure 2 on every tick, each time with variable 14 set to `index`.
// Procedure 3 writes the counter that the table at 0x15d6798 gives for `index` to variable 13. Procedure 2 compares
// that counter with variable 13. When they differ, it writes -1 to variable 14, the counter to 13, and the chord to 12.
// Procedure 4 writes the tempo to variable 15. The writes of procedures 3 and 4 come at frame 0. Procedure 2's come at
// each frame whose counter differs from the last that it wrote, and procedure 4's again wherever the tempo changes.
// Before a beat, the counters read as 0xFFFF and the tempo as 0. Throws std::invalid_argument for an `index` that the
// table has no counter for.
std::vector<ProcedureWrite> UserProcedureWrites(const std::vector<SyncFrame>& frames, int16_t index = 4);

// A stream that attaches the music sync: the stream sound's label, its file's path among the game's files, and the path
// of its jump setting.
struct SyncedStream
{
    std::string_view label;
    std::string_view stream_path;
    std::string_view jump_setting_path;
};

// A sequence that follows the music sync of the stream under it: its label, the value that it writes to variable 14
// before each call of procedure 2 (UserProcedureWrites' `index`), and the streams that it can play over.
struct SyncedPiece
{
    std::string_view label;
    int16_t index = 4;
    std::span<const SyncedStream> streams;
};

// The synced pieces of the sound archive `archive`, which a table recognizes by the archive's size and SHA-1. None for
// any other archive.
std::span<const SyncedPiece> SyncedPieces(std::span<const uint8_t> archive);

// A play of a synced piece over a stream: the writes of the piece's user procedures, from the frame at which the piece
// and the stream start together, and the frames of one pass over the stream's regions and its loop once more. The
// frames are those of nn::atk's sound thread, 5 ms each.
struct SyncedPlay
{
    std::vector<ProcedureWrite> writes;
    uint32_t frames = 0;
};

// The play of `piece` over the stream file `stream` with the jump setting `jump_setting`. The game updates the sync
// once in each of its frames, 60 a second. bex::sound::SoundModule's set-up (fn_bd884) registers its update fn_bfa3c,
// which reaches fn_cb18c through fn_fb730, for phase 22 of the engine's frame (fn_984a78). The game's boot.nbinit fixes
// the frame rate at 60 (FixedFrameRate_Fps60). The game sets a temporary rate of 0 or 1 only (fn_383aa0), and
// bex::MainModule::GetTimePerFrame gives 1/60 s for both. A frame of the game takes the stream on by a 60th of its
// sample rate. The writes that it makes reach the sequence from the first frame of 5 ms that starts at or after it. The
// writes go on for `extra` frames of 5 ms past the pass and the loop, while the stream's loop plays again. Throws
// xsflib::FormatError for files that ReadStreamCues, ReadJumpSetting or PlayOrder refuse, or a stream whose sample rate
// isn't a multiple of 60.
SyncedPlay PlaySynced(const SyncedPiece& piece, std::span<const uint8_t> stream, std::span<const uint8_t> jump_setting,
                      uint32_t extra);

} // namespace ntdlib::nx::bex

// SPDX-License-Identifier: MIT

#include "ntdlib/konami/commands.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"

namespace ntdlib::konami
{
namespace
{

using agb::kRomBase;
using Item = commands::Command;
using commands::FromUnit;
using commands::Kind;
using commands::kOne;
using commands::ToUnit;

// The first sample track, for the commands that read otherwise on a sample track.
constexpr int kSampleTrack = kPsgTracks;

// The bytes that the longest command or delay takes up.
constexpr std::size_t kLongestItem = 8;

// Whether revision `r`'s tracks keep the return position after a call's last command, and go back there every 256
// commands after it.
bool KeepsReturn(Revision r)
{
    return r == Revision::kEternalDuelist || r == Revision::kDungeonDiceMonsters;
}

// Returns command `c` as an item of the common form, in the console's values. It holds the arguments that a command of
// kind kOther or kLoopStart keeps. A call's target and the kept bytes stay out.
Item RawItem(const Rom& rom, const konami::Command& c)
{
    Item item;
    const auto set = [&item](Kind kind, int64_t value = 0, uint8_t index = 0)
    {
        item.kind = kind;
        item.value.value = value;
        item.index = index;
    };
    const auto rest = [&]
    {
        for (uint32_t i = 1; i < c.length; i++)
        {
            item.bytes.push_back(rom.U8(c.addr + i));
        }
    };

    switch (c.op)
    {
    case Op::kDelay:
        set(Kind::kWait, c.delay);
        break;

    case Op::kDuty:
        set(Kind::kProgram, c.value, 0);
        break;

    case Op::kWave:
        set(Kind::kProgram, c.value, 1);
        break;

    case Op::kInstrument:
        set(Kind::kProgram, c.value, 2);
        break;

    case Op::kNote:
    case Op::kNextNote:
        set(Kind::kNote, 0, c.op == Op::kNextNote);
        item.sample = uint16_t(c.sample);
        item.key = uint8_t(c.semitone);
        item.velocity = c.vol;
        break;

    case Op::kPsgNote:
    case Op::kSampleAtNote:
        set(Kind::kNote, 0, c.op == Op::kSampleAtNote);
        item.key = uint8_t(c.note);
        item.velocity = c.vol;
        break;

    case Op::kVolume:
        set(Kind::kVolume, c.vol, 0);
        break;

    case Op::kSetVolume:
        set(Kind::kVolume, c.vol, 1);
        break;

    case Op::kPairVolumes:
        set(Kind::kVolume, c.value & 15, 2);
        item.velocity = c.value >> 4;
        break;

    case Op::kRest:
        set(Kind::kNoteOff);
        break;

    case Op::kRelease:
        set(Kind::kRelease, 0, 0);
        break;

    case Op::kFade:
        set(Kind::kRelease, 0, c.opcode & 1 ? 2 : 1);
        break;

    case Op::kPan:
        set(Kind::kPan, c.value);
        break;

    case Op::kPanLevels:
        set(Kind::kPan, (c.value & 15) - (c.value >> 4));
        item.velocity = std::max(c.value & 15, c.value >> 4);
        break;

    case Op::kVibrato:
        set(Kind::kLfoDepth, c.value);
        break;

    case Op::kPitchBend:
        set(Kind::kPitchBend, c.value, 0);
        break;

    case Op::kSampleBend:
        set(Kind::kPitchBend, c.value, c.opcode & 1 ? 2 : 1);
        break;

    case Op::kLoopPoint:
        set(Kind::kLoopStart);
        rest();
        break;

    case Op::kEchoFeedback:
        set(Kind::kEchoVolume, c.value);
        break;

    case Op::kEchoDelay:
        set(Kind::kEchoLength, c.value);
        break;

    case Op::kEndTrack:
        set(Kind::kEnd, 0, 0);
        break;

    case Op::kEndTrackHard:
        set(Kind::kEnd, 0, 1);
        break;

    case Op::kJump:
        if (c.value != 0)
        {
            set(Kind::kLoopEnd);
        }
        else
        {
            set(Kind::kEnd, 0, 2);
        }
        break;

    case Op::kAttack:
        set(Kind::kAttack, c.value);
        break;

    case Op::kDecay:
        set(Kind::kDecay, c.value);
        break;

    case Op::kHoldOutput:
        set(Kind::kMute, c.value);
        break;

    case Op::kSongVolume:
        set(Kind::kMainVolume, c.value);
        break;

    case Op::kCall:
        set(Kind::kCall, c.count);
        item.key = c.opcode;
        break;

    case Op::kNop:
    case Op::kEchoRoute:
    case Op::kVolumeScale:
    case Op::kPsgPan:
    case Op::kPsgVolume:
    case Op::kUnknown:
        set(Kind::kOther);
        item.encoding.opcode = c.opcode;
        rest();
        break;
    }

    return item;
}

// Whether two items in the console's values mean the same. A call's target and the kept bytes don't count. Each kind's
// arguments do: the bytes of a loop point or of a command of kOther are its meaning.
bool SameMeaning(const Item& a, const Item& b, bool arguments)
{
    const auto fields = [](const Item& c)
    {
        return std::tie(c.kind, c.key, c.velocity, c.index, c.value, c.sample);
    };
    if (fields(a) != fields(b))
    {
        return false;
    }

    return !arguments || (a.bytes == b.bytes && a.encoding.opcode == b.encoding.opcode);
}

bool KeepsArguments(Kind kind)
{
    return kind == Kind::kOther || kind == Kind::kLoopStart;
}

// The item's values in the console's units.
Item ToConsole(const Item& item, Revision revision)
{
    Item raw = item;
    raw.velocity = FromUnit(item.velocity, commands::Scale{});
    raw.value.value = FromUnit(item.value.value, ScaleOf(item.kind, revision));
    return raw;
}

// The item's values in the units.
void ToUnits(Item& item, Revision revision)
{
    item.velocity = ToUnit(item.velocity, commands::Scale{});
    item.value.value = ToUnit(item.value.value, ScaleOf(item.kind, revision));
}

// The level of a new pan command, in the revisions whose pan holds a level for each side.
constexpr int64_t kNewPanLevel = 15;

// A value's byte. Throws std::invalid_argument past `high`.
uint8_t ByteOf(int64_t value, int64_t low, int64_t high, Kind kind)
{
    if (value < low || value > high)
    {
        throw std::invalid_argument("a value of a " + std::string(commands::Name(kind)) +
                                    " command doesn't fit in the driver's bytes");
    }

    return uint8_t(value);
}

// The usual bytes of a delay of `delay` frames.
std::vector<uint8_t> UsualDelay(int64_t delay, Revision r)
{
    const auto too_long = []
    {
        return std::invalid_argument("a wait is longer than the driver's delays");
    };
    if (delay < 0)
    {
        throw too_long();
    }

    const bool short_up_to_ef = EternalDuelistLayout(r) || r == Revision::kDungeonDiceMonsters;
    const int64_t one_byte = short_up_to_ef ? 0xEF : 0xDF;
    if (delay <= one_byte)
    {
        return {uint8_t(delay)};
    }
    if (short_up_to_ef)
    {
        if (delay > 0xFFF)
        {
            throw too_long();
        }
        return {uint8_t(0xF0 | (delay >> 8)), uint8_t(delay)};
    }
    if (r == Revision::kUltimateMasters)
    {
        if (delay > 0xFFFF)
        {
            throw too_long();
        }
        return delay <= 0xFFF ? std::vector<uint8_t>{uint8_t(0xE0 | (delay >> 8)), uint8_t(delay)}
                              : std::vector<uint8_t>{0xF0, uint8_t(delay), uint8_t(delay >> 8)};
    }
    if (delay > 0x1FFF)
    {
        throw too_long();
    }

    return {uint8_t(0xE0 | (delay >> 8)), uint8_t(delay)};
}

// The usual opcode of a duty byte or a wave. The bytes that the revision can't hold read back as something else.
uint8_t UsualDutyOrWave(const Item& item, Revision r)
{
    const int64_t value = item.value.value;
    const bool wave = item.index == 1;
    if (r == Revision::kUltimateMasters)
    {
        return (value & 0x3F) == 0 ? uint8_t(0x80 | (value >> 6)) : uint8_t(value + 4);
    }
    if (r == Revision::kDungeonDiceMonsters)
    {
        if (wave)
        {
            return uint8_t(0x70 | value);
        }
        return value <= 0x6F ? uint8_t(value) : uint8_t(0x7C | (value >> 6));
    }
    if (wave)
    {
        return uint8_t(0x80 | (value + 4));
    }
    if (LaterUltimateMasters(r))
    {
        return (value & 0x3F) == 0 ? uint8_t(0x80 | (value >> 6)) : uint8_t(0x80 | (value + 4));
    }

    // The Kitaro revision and those with Rave Master's commands store a duty as it is, and the others in bits 6-7.
    return uint8_t(0x80 | (RaveMasterCommands(r) || r == Revision::kKitaro ? value : value >> 6));
}

// The usual bytes of `item`, whose values are the console's. Throws std::invalid_argument for a value that no byte
// holds. The caller reads the bytes back: a revision without the command reads them as something else.
std::vector<uint8_t> UsualBytes(const Item& item, Revision r)
{
    const Kind kind = item.kind;
    const int64_t value = item.value.value;
    const bool ultimate = UltimateMasters(r);
    const bool dungeon = r == Revision::kDungeonDiceMonsters;
    const auto byte = [kind](int64_t v)
    {
        return ByteOf(v, 0, 0xFF, kind);
    };

    switch (kind)
    {
    case Kind::kWait:
        return UsualDelay(value, r);

    case Kind::kProgram:
        if (item.index == 2)
        {
            return {0xF6, byte(value)};
        }
        return {UsualDutyOrWave(item, r)};

    case Kind::kNote:
        {
            const int64_t volume = item.velocity;
            if (item.sample)
            {
                const uint16_t sample = *item.sample;
                if (dungeon)
                {
                    return {uint8_t((item.index == 1 ? 0xA0 : 0xB0) | ByteOf(volume, 0, 15, kind)), byte(sample)};
                }

                std::vector<uint8_t> bytes;
                const uint8_t first = item.key != 0 ? 0xB0 : 0xA0;
                if (ultimate && volume > 7)
                {
                    bytes = {uint8_t(first | 8), byte(volume)};
                }
                else
                {
                    bytes = {uint8_t(first | ByteOf(volume, 0, ultimate ? 7 : 15, kind))};
                }

                if (sample > 0xEF && r == Revision::kLiveStart)
                {
                    bytes.push_back(uint8_t(0xF0 | ByteOf(sample >> 8, 0, 15, kind)));
                    bytes.push_back(uint8_t(sample));
                }
                else
                {
                    bytes.push_back(byte(sample));
                }
                if (item.key != 0)
                {
                    bytes.push_back(item.key);
                }

                return bytes;
            }

            if (dungeon)
            {
                return {uint8_t((item.index == 1 ? 0x90 : 0xE0) | ByteOf(volume, 0, 15, kind)), item.key};
            }
            if (ultimate && volume > 7)
            {
                return {0xD8, byte(volume), item.key};
            }
            return {uint8_t(0xD0 | ByteOf(volume, 0, ultimate ? 7 : 15, kind)), item.key};
        }

    case Kind::kVolume:
        if (item.index == 2)
        {
            return {0xF0, uint8_t((ByteOf(item.velocity, 0, 15, kind) << 4) | ByteOf(value, 0, 15, kind))};
        }
        if (dungeon)
        {
            return {uint8_t((item.index == 1 ? 0xD0 : 0xC0) | ByteOf(value, 0, 15, kind))};
        }
        if (ultimate && value > 7)
        {
            return {0xC8, byte(value)};
        }
        return {uint8_t(0xC0 | ByteOf(value, 0, ultimate ? 7 : 15, kind))};

    case Kind::kNoteOff:
        return {uint8_t(dungeon ? 0xFC : 0xE0)};

    case Kind::kRelease:
        return {uint8_t(item.index == 0 ? 0xFB : item.index == 1 ? 0xF8 : 0xF9)};

    case Kind::kPan:
        if (ultimate)
        {
            return {0xF0, byte(value)};
        }
        else
        {
            // The larger level stays on its side, and the other lies `value` below it. A new command takes level 15.
            const int64_t level = item.velocity != 0 ? item.velocity : kNewPanLevel;
            const int64_t left = value > 0 ? level - value : level;
            const int64_t right = value < 0 ? level + value : level;
            return {0xF0, uint8_t((ByteOf(left, 0, 15, kind) << 4) | ByteOf(right, 0, 15, kind))};
        }

    case Kind::kLfoDepth:
        if (dungeon)
        {
            return {uint8_t(0x80 | ByteOf(value, 0, 15, kind))};
        }
        return {0xF1, byte(value)};

    case Kind::kPitchBend:
        if (dungeon)
        {
            return {uint8_t(item.index == 0 ? 0xF3 : item.index == 1 ? 0xF2 : 0xF1), byte(value + 32)};
        }
        if (ultimate && value >= -0x400 && (value < -0x40 || value > 0x3F))
        {
            const int64_t held = ByteOf((value + 0x400) >> 8, 0, 15, kind);
            return {0xF2, uint8_t(0x80 | held), uint8_t(value + 0x400)};
        }
        return {0xF2, byte(value + 0x40)};

    case Kind::kLoopStart:
        {
            std::vector<uint8_t> bytes = {uint8_t(dungeon ? 0xFA : 0xF3)};
            bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            return bytes;
        }

    case Kind::kLoopEnd:
        return ultimate ? std::vector<uint8_t>{0xFF, 1} : std::vector<uint8_t>{0xFE};

    case Kind::kEnd:
        if (item.index == 2)
        {
            return ultimate ? std::vector<uint8_t>{0xFF, 0} : std::vector<uint8_t>{0xFF};
        }
        return {uint8_t(ultimate && item.index == 0 ? 0xFE : 0xFD)};

    case Kind::kAttack:
        return {0xF4, byte(value)};

    case Kind::kDecay:
        return {0xF5, byte(value)};

    case Kind::kEchoVolume:
        return {0xF7, byte(value)};

    case Kind::kEchoLength:
        return {0xF8, byte(value)};

    case Kind::kMute:
        return {0xFA, byte(value)};

    case Kind::kMainVolume:
        return {0xFB, byte(value)};

    case Kind::kCall:
        // The caller writes the target.
        return {item.key, 0, 0, byte(value)};

    case Kind::kOther:
        {
            std::vector<uint8_t> bytes = {uint8_t(item.encoding.opcode)};
            bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
            return bytes;
        }

    default:
        throw std::invalid_argument("the driver has no command of kind " + std::string(commands::Name(kind)));
    }
}

// The track that reads an item's bytes the way the item says.
int ReadingTrack(const Item& item)
{
    const bool sample_track = (item.kind == Kind::kVolume && item.index == 2) ||
                              (item.kind == Kind::kLoopStart && !item.bytes.empty());
    return sample_track ? kSampleTrack : 0;
}

// Returns the item in the console's values that `bytes` read as for `item`'s kind of track, or nothing where they read
// otherwise: more or fewer bytes than they hold.
std::optional<Item> ReadBack(const std::vector<uint8_t>& bytes, bool wait, int track, Revision r)
{
    std::vector<uint8_t> image = bytes;
    image.resize(std::max(image.size(), kLongestItem), 0);
    Rom rom;
    rom.Assign(std::move(image));

    konami::Command c;
    if (wait)
    {
        c.op = Op::kDelay;
        c.length = ReadDelay(rom, kRomBase, r, c.delay);
    }
    else
    {
        c = DecodeCommand(rom, kRomBase, track, r);
    }
    if (c.length != bytes.size())
    {
        return std::nullopt;
    }

    return RawItem(rom, c);
}

// The bytes of `item`, a command or a wait. The item's kept bytes come first, while they read as its values. Throws
// std::invalid_argument where the usual bytes don't read as its values either: the revision lacks the command.
std::vector<uint8_t> ItemBytes(const Item& item, Revision r)
{
    const Item raw = ToConsole(item, r);
    const bool wait = item.kind == Kind::kWait;
    const bool arguments = KeepsArguments(item.kind);
    const int track = ReadingTrack(item);
    if (!arguments && (item.encoding.opcode & kKeptBytes))
    {
        std::vector<uint8_t> kept = {uint8_t(item.encoding.opcode)};
        kept.insert(kept.end(), item.bytes.begin(), item.bytes.end());
        const std::optional<Item> read = ReadBack(kept, wait, track, r);
        if (read && SameMeaning(*read, raw, false))
        {
            return kept;
        }
    }

    std::vector<uint8_t> usual = UsualBytes(raw, r);
    Item expected = raw;
    if (!arguments)
    {
        expected.bytes.clear();
    }
    if (item.kind == Kind::kCall)
    {
        expected.index = 0;
    }
    if (item.kind == Kind::kPan && raw.velocity == 0 && !UltimateMasters(r))
    {
        expected.velocity = kNewPanLevel;
    }
    const std::optional<Item> read = ReadBack(usual, wait, track, r);
    if (!read || !SameMeaning(*read, expected, arguments))
    {
        throw std::invalid_argument("revision " + std::string(RevisionName(r)) + " has no bytes for a " +
                                    std::string(commands::Name(item.kind)) + " command with these values");
    }

    return usual;
}

// How one or more tracks read a byte.
struct Read
{
    uint32_t length = 0;
    bool wait = false;
    int track = 0;
};

// The driver's state of a track at a delay.
struct TrackState
{
    uint16_t start = 0;
    uint32_t pos = 0;
    uint16_t ret = 0;
    uint16_t saved_start = 0;
    uint8_t count = 0;
};

// Follows a song's tracks as the driver plays them, and records what each byte reads as.
class Flow
{
public:
    Flow(const Rom& rom, uint32_t base, uint32_t size, std::span<const uint16_t> offsets, Revision revision)
        : rom_(rom), base_(base), size_(size), offsets_(offsets), revision_(revision)
    {
        std::vector<std::set<uint16_t>> starts(offsets.size());
        for (std::size_t t = 0; t < offsets.size() && problem_.empty(); t++)
        {
            starts_ = &starts[t];
            seen_.clear();
            TrackState state;
            state.start = offsets[t];
            starts_->insert(state.start);
            Follow(int(t), state);
            ran_.push_back(seen_);
        }

        // A song loop sends each track back to the start that it holds, with no call running.
        bool more = loops_;
        while (more && problem_.empty())
        {
            more = false;
            for (std::size_t t = 0; t < offsets.size() && problem_.empty(); t++)
            {
                const std::set<uint16_t> held = starts[t];
                starts_ = &starts[t];
                seen_ = std::move(ran_[t]);
                for (const uint16_t start : held)
                {
                    if (restarted_.insert({int(t), start}).second)
                    {
                        TrackState state;
                        state.start = start;
                        Follow(int(t), state);
                        more = true;
                    }
                }
                ran_[t] = std::move(seen_);
            }
        }

        if (problem_.empty())
        {
            CheckOverlaps();
        }
    }

    const std::string& Problem() const
    {
        return problem_;
    }

    const std::map<uint32_t, Read>& Reads() const
    {
        return reads_;
    }

    // The offset past the last byte that a read took.
    uint32_t End() const
    {
        return end_;
    }

    // Whether the read at `offset` lies inside a loop point's bytes.
    bool Inner(uint32_t offset) const
    {
        return inner_.contains(offset);
    }

private:
    struct Key
    {
        friend bool operator==(const Key&, const Key&) = default;
        uint64_t low = 0;
        uint64_t high = 0;
    };

    struct KeyHash
    {
        std::size_t operator()(const Key& key) const
        {
            return std::size_t(key.low * 0x9E3779B97F4A7C15ULL) ^ std::size_t(key.high * 0xC2B2AE3D27D4EB4FULL);
        }
    };

    void Fail(const std::string& what)
    {
        if (problem_.empty())
        {
            problem_ = what;
        }
    }

    // Records a read of `length` bytes. Returns false after a problem.
    bool Add(uint32_t offset, uint32_t length, bool wait, int track, const konami::Command* command)
    {
        if (uint64_t(offset) + length > size_)
        {
            Fail("a track runs past the song's bytes");
            return false;
        }

        const auto [it, added] = reads_.try_emplace(offset, Read{length, wait, track});
        end_ = std::max(end_, offset + length);
        if (added)
        {
            return true;
        }

        const Read& read = it->second;
        if (read.wait != wait)
        {
            Fail("a byte reads both as a delay and as a command");
            return false;
        }
        if (read.length != length)
        {
            Fail("two tracks read a command with other lengths");
            return false;
        }
        if (!command)
        {
            return true;
        }
        if (IsPsgTrack(read.track) != IsPsgTrack(track))
        {
            const konami::Command first = DecodeCommand(rom_, base_ + offset, read.track, revision_);
            if (!SameMeaning(RawItem(rom_, first), RawItem(rom_, *command), true))
            {
                Fail("a PSG track and a sample track read a command with other values");
                return false;
            }
        }
        if (command->op == Op::kCall && offsets_[size_t(read.track)] != offsets_[size_t(track)])
        {
            Fail("two tracks read a call with other targets");
            return false;
        }

        return true;
    }

    void Follow(int track, TrackState s)
    {
        const bool keep = KeepsReturn(revision_);
        for (;;)
        {
            // The delay, then the command after it.
            const uint32_t at = s.start + s.pos;
            if (at >= size_)
            {
                Fail("a track runs past the song's bytes");
                return;
            }

            uint32_t delay;
            const uint32_t delay_size = ReadDelay(rom_, base_ + at, revision_, delay);
            if (!Add(at, delay_size, true, track, nullptr))
            {
                return;
            }

            s.pos = uint16_t(s.pos + delay_size);
            const bool call_runs = s.ret != 0;
            const Key key = {uint64_t(s.start) | uint64_t(s.pos) << 16 | uint64_t(s.ret) << 32 |
                                 uint64_t(call_runs ? s.saved_start : 0) << 48,
                             uint64_t(call_runs ? s.count : 0)};
            if (!seen_.insert(key).second)
            {
                return;
            }

            const uint32_t command_at = s.start + s.pos;
            if (command_at >= size_)
            {
                Fail("a track runs past the song's bytes");
                return;
            }

            const konami::Command c = DecodeCommand(rom_, base_ + command_at, track, revision_);
            if (c.op == Op::kUnknown)
            {
                Fail("a track reaches an opcode that the driver doesn't read");
                return;
            }
            if (!Add(command_at, c.length, false, track, &c))
            {
                return;
            }

            uint32_t pos = s.pos + c.length;
            bool counted = true;
            switch (c.op)
            {
            case Op::kEndTrack:
            case Op::kEndTrackHard:
                return;

            case Op::kJump:
                loops_ = loops_ || c.value != 0;
                return;

            case Op::kCall:
                if (revision_ == Revision::kDungeonDiceMonsters && CallSetsFirst(c.opcode, revision_))
                {
                    // F5 takes its first command from the delay byte after it, whether or not the track ever goes back
                    // there.
                    uint32_t after;
                    const uint32_t after_size = ReadDelay(rom_, base_ + command_at + c.length, revision_, after);
                    if (!Add(command_at + c.length, after_size, true, track, nullptr))
                    {
                        return;
                    }
                }

                s.saved_start = s.start;
                s.ret = uint16_t(pos);
                s.start = offsets_[size_t(track)];
                pos = uint32_t(c.value);
                if (CallSetsFirst(c.opcode, revision_))
                {
                    s.count = uint8_t(c.count + 1);
                }
                else
                {
                    s.count = uint8_t(c.count);
                    counted = false;
                }
                starts_->insert(s.start);
                break;

            case Op::kLoopPoint:
                if (revision_ == Revision::kDungeonDiceMonsters)
                {
                    s.start = uint16_t(s.start + pos - 2);
                    pos = 2;
                }
                else
                {
                    s.start = uint16_t(s.start + pos);
                    pos = 0;
                }
                starts_->insert(s.start);
                break;

            default:
                break;
            }

            if (counted && s.ret != 0)
            {
                s.count--;
                if (s.count == 0)
                {
                    s.start = s.saved_start;
                    pos = s.ret;
                    if (!keep)
                    {
                        s.ret = 0;
                    }
                    starts_->insert(s.start);
                }
            }
            s.pos = pos;
        }
    }

    // Finds reads that overlap. A song loop in Dungeon Dice Monsters reads the two bytes after FA, and those lie inside
    // the loop point.
    void CheckOverlaps()
    {
        uint32_t outer_end = 0;
        bool in_loop_point = false;
        for (const auto& [offset, read] : reads_)
        {
            if (offset < outer_end)
            {
                if (!in_loop_point || offset + read.length > outer_end)
                {
                    Fail(in_loop_point ? "a song loop reads a command past its loop point's bytes"
                                       : "commands of the song overlap");
                    return;
                }
                inner_.insert(offset);
                continue;
            }

            outer_end = offset + read.length;
            in_loop_point = revision_ == Revision::kDungeonDiceMonsters && !read.wait &&
                            rom_.U8(base_ + offset) == 0xFA;
        }
    }

    const Rom& rom_;
    uint32_t base_;
    uint32_t size_;
    std::span<const uint16_t> offsets_;
    Revision revision_;
    std::map<uint32_t, Read> reads_;
    std::set<uint32_t> inner_;
    uint32_t end_ = 0;
    bool loops_ = false;
    std::string problem_;
    std::set<uint16_t>* starts_ = nullptr;
    std::unordered_set<Key, KeyHash> seen_;
    std::vector<std::unordered_set<Key, KeyHash>> ran_;
    std::set<std::pair<int, uint16_t>> restarted_;
};

// The whole song as data, split at the track's starts.
Decoded Opaque(const Rom& rom, uint32_t base, uint32_t size, std::span<const uint16_t> offsets, std::string reason)
{
    Decoded decoded;
    decoded.opaque = true;
    decoded.reason = std::move(reason);

    std::set<uint32_t> splits = {0};
    for (const uint16_t offset : offsets)
    {
        splits.insert(offset);
    }

    std::map<uint32_t, uint32_t> items;
    for (auto it = splits.begin(); it != splits.end(); ++it)
    {
        const auto next = std::next(it);
        const uint32_t end = next == splits.end() ? std::max(size, *it) : *next;
        Item data;
        data.kind = Kind::kData;
        for (uint32_t i = *it; i < end; i++)
        {
            data.bytes.push_back(rom.U8(base + i));
        }
        items[*it] = uint32_t(decoded.sequence.items.size());
        decoded.sequence.items.push_back(std::move(data));
    }
    for (const uint16_t offset : offsets)
    {
        decoded.entries.push_back(items[offset]);
    }

    return decoded;
}

// Decodes the song at `base`, of `size` bytes, from what `flow` found.
Decoded Build(const Rom& rom, uint32_t base, uint32_t size, std::span<const uint16_t> offsets, Revision revision,
              const Flow& flow)
{
    if (!flow.Problem().empty())
    {
        return Opaque(rom, base, size, offsets, flow.Problem());
    }

    Decoded decoded;
    std::vector<Item>& items = decoded.sequence.items;
    std::map<uint32_t, uint32_t> item_at;
    const auto add_data = [&](uint32_t from, uint32_t to)
    {
        if (to > from)
        {
            Item data;
            data.kind = Kind::kData;
            data.bytes.assign(rom.Ptr(base + from), rom.Ptr(base + from) + (to - from));
            items.push_back(std::move(data));
        }
    };

    uint32_t at = 0;
    for (const auto& [offset, read] : flow.Reads())
    {
        if (flow.Inner(offset))
        {
            continue;
        }

        add_data(at, offset);
        at = offset + read.length;

        konami::Command c;
        if (read.wait)
        {
            c.op = Op::kDelay;
            c.addr = base + offset;
            c.length = ReadDelay(rom, c.addr, revision, c.delay);
        }
        else
        {
            c = DecodeCommand(rom, base + offset, read.track, revision);
        }

        Item raw = RawItem(rom, c);
        Item item = raw;
        const std::vector<uint8_t> real(rom.Ptr(base + offset), rom.Ptr(base + offset) + read.length);
        if (!KeepsArguments(item.kind) && item.kind != Kind::kCall)
        {
            std::vector<uint8_t> usual;
            try
            {
                usual = UsualBytes(raw, revision);
            }
            catch (const std::invalid_argument&)
            {
            }
            if (usual != real)
            {
                item.encoding.opcode = uint16_t(kKeptBytes | real[0]);
                item.bytes.assign(real.begin() + 1, real.end());
            }
        }

        if (item.kind == Kind::kCall)
        {
            item.index = uint8_t(read.track);
            item.target = uint32_t(c.value);
            item.target_outside = true; // until the items are known
        }

        ToUnits(item, revision);
        item_at[offset] = uint32_t(items.size());
        items.push_back(std::move(item));
    }
    add_data(at, size);

    // The call's targets as items.
    for (Item& item : items)
    {
        if (item.kind == Kind::kCall)
        {
            const uint32_t target = offsets[item.index] + item.target;
            const auto found = item_at.find(target);
            if (found != item_at.end())
            {
                item.target = found->second;
                item.target_outside = false;
            }
        }
    }

    for (const uint16_t offset : offsets)
    {
        decoded.entries.push_back(item_at.at(offset));
    }

    return decoded;
}

} // namespace

Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint16_t> offsets, Revision revision)
{
    for (const uint16_t offset : offsets)
    {
        if (offset >= data.size())
        {
            throw std::invalid_argument("a track starts outside the song's bytes");
        }
    }

    Rom rom;
    rom.Assign(std::vector<uint8_t>(data.begin(), data.end()));
    const auto size = uint32_t(data.size());
    const Flow flow(rom, kRomBase, size, offsets, revision);

    return Build(rom, kRomBase, size, offsets, revision, flow);
}

Decoded DecodeSong(const Rom& rom, const DriverInfo& info, int song)
{
    SongHeader header;
    if (!ReadSongHeader(rom, info.song_table, song, info.revision, header) || !rom.Contains(header.base))
    {
        throw std::invalid_argument("song " + std::to_string(song) + " has no bytes in the ROM");
    }

    const std::span<const uint16_t> offsets(header.offsets, size_t(header.tracks));
    const Flow flow(rom, header.base, rom.End() - header.base, offsets, info.revision);

    // The song ends with the last byte that a track reads, or at the next song where that lies close after it.
    uint32_t size = std::max<uint32_t>(flow.End(), 1);
    uint32_t next = rom.End();
    for (int s = 0; s < info.song_count; s++)
    {
        SongHeader other;
        if (ReadSongHeader(rom, info.song_table, s, info.revision, other) && other.base > header.base + size - 1 &&
            other.base < next)
        {
            next = other.base;
        }
    }
    if (next - (header.base + size) < kSongGap && next < rom.End())
    {
        size = next - header.base;
    }

    return Build(rom, header.base, size, offsets, info.revision, flow);
}

std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, Revision revision)
{
    std::vector<uint32_t> offsets;
    offsets.reserve(sequence.items.size() + 1);
    uint32_t at = 0;
    for (const Item& item : sequence.items)
    {
        offsets.push_back(at);
        at += uint32_t(item.kind == Kind::kData ? item.bytes.size() : ItemBytes(item, revision).size());
    }
    offsets.push_back(at);

    return offsets;
}

Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, Revision revision)
{
    Encoded encoded;
    std::vector<std::vector<uint8_t>> pieces;
    pieces.reserve(sequence.items.size());
    uint32_t at = 0;
    for (const Item& item : sequence.items)
    {
        encoded.offsets.push_back(at);
        pieces.push_back(item.kind == Kind::kData ? item.bytes : ItemBytes(item, revision));
        at += uint32_t(pieces.back().size());
    }
    encoded.offsets.push_back(at);

    std::vector<uint16_t> starts;
    for (const uint32_t entry : entries)
    {
        if (entry >= sequence.items.size())
        {
            throw std::invalid_argument("a track starts past the song's items");
        }
        if (encoded.offsets[entry] > 0xFFFF)
        {
            throw std::invalid_argument("a track starts past 0xFFFF bytes from the song's base");
        }
        starts.push_back(uint16_t(encoded.offsets[entry]));
    }

    encoded.bytes.reserve(at);
    bool any_command = false;
    for (std::size_t i = 0; i < pieces.size(); i++)
    {
        const Item& item = sequence.items[i];
        any_command = any_command || item.kind != Kind::kData;
        if (item.kind == Kind::kCall)
        {
            if (item.index >= starts.size())
            {
                throw std::invalid_argument("a call counts from a track that the song lacks");
            }

            uint64_t value = item.target;
            if (!item.target_outside)
            {
                if (item.target >= sequence.items.size())
                {
                    throw std::invalid_argument("a call's target lies past the song's items");
                }
                const uint32_t target = encoded.offsets[item.target];
                if (target < starts[item.index] || target - starts[item.index] > 0xFFFF)
                {
                    throw std::invalid_argument("a call's target lies before its track's start or too far after it");
                }
                value = target - starts[item.index];
            }
            else if (value > 0xFFFF)
            {
                throw std::invalid_argument("a call's offset doesn't fit in the driver's bytes");
            }
            pieces[i][1] = uint8_t(value);
            pieces[i][2] = uint8_t(value >> 8);
        }
        encoded.bytes.insert(encoded.bytes.end(), pieces[i].begin(), pieces[i].end());
    }

    // The driver has to read the bytes as the items say. A song kept as data holds no commands.
    if (any_command)
    {
        Rom rom;
        rom.Assign(encoded.bytes);
        const Flow flow(rom, kRomBase, at, starts, revision);
        if (!flow.Problem().empty())
        {
            throw std::invalid_argument("the song's items don't read back: " + flow.Problem());
        }

        std::map<uint32_t, std::size_t> item_at;
        for (std::size_t i = 0; i < sequence.items.size(); i++)
        {
            if (encoded.offsets[i + 1] > encoded.offsets[i])
            {
                item_at[encoded.offsets[i]] = i;
            }
        }
        for (const auto& [offset, read] : flow.Reads())
        {
            if (flow.Inner(offset))
            {
                continue;
            }

            const auto found = item_at.find(offset);
            if (found == item_at.end() || sequence.items[found->second].kind == Kind::kData)
            {
                throw std::invalid_argument("a track reaches bytes that no item starts at");
            }
            if ((sequence.items[found->second].kind == Kind::kWait) != read.wait)
            {
                throw std::invalid_argument(read.wait ? "a track reads a delay where a command lies"
                                                      : "a track reads a command where a wait lies");
            }
        }
    }

    return encoded;
}

commands::Scale ScaleOf(Kind kind, Revision revision)
{
    const bool dungeon = revision == Revision::kDungeonDiceMonsters;
    switch (kind)
    {
    case Kind::kPitchBend:
        return {dungeon ? kOne / 32 : kOne / 64, 0};
    case Kind::kPan:
        if (UltimateMasters(revision))
        {
            return {kOne, -64 * kOne};
        }
        return {63 * kOne / 15, 0};
    default:
        return {};
    }
}

Item NewLoopPoint(int track, Revision revision)
{
    Item item;
    item.kind = Kind::kLoopStart;
    const bool extra = revision == Revision::kDungeonDiceMonsters ||
                       (!IsPsgTrack(track) && !RaveMasterCommands(revision));
    if (extra)
    {
        item.bytes.assign(2, 0);
    }

    return item;
}

} // namespace ntdlib::konami

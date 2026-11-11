// SPDX-License-Identifier: MIT

#include "ntdlib/nx/bex/music_sync.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "nx/bex/message_pack.h"

namespace ntdlib::nx::bex
{

namespace
{

using detail::MessagePack;
using detail::Value;

// The size of the header, the place of its count of blocks, and the size of each entry of the block table after it.
constexpr std::size_t kHeaderSize = 0x14;
constexpr std::size_t kBlockCount = 0x10;
constexpr std::size_t kBlockEntry = 12;

constexpr uint16_t kInfoBlock = 0x4000;
constexpr uint16_t kRegionBlock = 0x4003;
constexpr uint16_t kMarkerBlock = 0x4005;
constexpr std::size_t kRegionEntry = 0x100;
constexpr std::size_t kRegionName = 0xC0;
constexpr std::size_t kMarkerEntry = 0x48;
constexpr std::size_t kNameBytes = 64;

// A name of up to 64 bytes that a zero ends.
std::string Name(std::span<const uint8_t> b, std::size_t at)
{
    std::string name;
    for (std::size_t i = 0; i < kNameBytes && b[at + i] != 0; ++i)
    {
        name.push_back(static_cast<char>(b[at + i]));
    }

    return name;
}

void Need(std::span<const uint8_t> file, std::size_t end)
{
    if (end > file.size())
    {
        throw xsflib::FormatError("the stream file ends inside a block");
    }
}

// A beat marker as the marker system keeps it (fn_c9f40): its position, and the time and tempo that it starts.
struct Beat
{
    int64_t position = 0;
    int8_t denominator = 4; // +8
    int8_t numerator = 4;   // +9
    int16_t tempo = 0;      // +0xa
};

// A chord marker: its position and its chord.
struct Chord
{
    int64_t position = 0;
    uint16_t code = 0;
};

// The marker system's object (0xd0 bytes, fn_c91b0) for a stream that the engine plays by regions. Such a stream's
// sound has a jump controller, and the object counts the region jumps (+0xc0 set by fn_c9870).
class MarkerSystem
{
public:
    explicit MarkerSystem(const StreamCues& cues) : sample_rate_(static_cast<int32_t>(cues.sample_rate))
    {
        for (const StreamMarker& marker : cues.markers)
        {
            Parse(marker);
        }
    }

    // The attach (fn_c9870). The beats are taken for the pair (0, 0): a beat at 0 starts there.
    void Attach()
    {
        TakeBeats(0, 0);
    }

    // The jump that the region callback reports (fn_cbe28): from the end of a region to the start of the next.
    void Jump(int64_t from, int64_t to)
    {
        jump_from_ = from;
        jump_to_ = to;
    }

    // A frame's update for the stream's position `position` (fn_cb250).
    void Update(int64_t position)
    {
        const int64_t prev = last_;
        const int64_t shifted = position - offset_;
        int64_t cur = std::max<int64_t>(shifted, 0);
        if (jump_from_ != -1)
        {
            if (prev >= jump_from_)
            {
                jump_from_ = -1;
                jump_to_ = -1;
            }
            else if ((prev > position || jump_from_ < position) && shifted < jump_to_)
            {
                cur = jump_from_ + shifted - jump_to_;
            }
        }

        TakeBeats(prev, cur);
        Count(prev, cur);
        TakeChords(prev, cur);
        published_ = counters_;
        last_ = cur;
    }

    SyncFrame State() const
    {
        SyncFrame state;
        state.counters = published_;
        state.chord = chord_;
        state.has_beat = beat_.has_value();
        state.tempo = beat_ ? beats_[*beat_].tempo : 0;

        return state;
    }

private:
    // The markers that a search looks for.
    enum class MarkerKind
    {
        kBeat,
        kChord,
    };

    // A marker's name split at each '#': MRK_BEAT#n/d#t and MRK_CHORD#c count, the others don't (fn_c9f40).
    void Parse(const StreamMarker& marker)
    {
        const std::string& name = marker.name;
        if (name.rfind("MRK_", 0) != 0)
        {
            return;
        }

        std::vector<std::string> parts;
        std::size_t from = 0;
        while (true)
        {
            const std::size_t hash = name.find('#', from);
            parts.push_back(name.substr(from, hash == std::string::npos ? std::string::npos : hash - from));
            if (hash == std::string::npos)
            {
                break;
            }
            from = hash + 1;
        }

        if (parts[0] == "MRK_BEAT" && parts.size() >= 3)
        {
            Beat beat;
            beat.position = marker.position;
            const std::size_t slash = parts[1].find('/');
            beat.numerator = static_cast<int8_t>(std::atoi(parts[1].substr(0, slash).c_str()));
            beat.denominator =
                static_cast<int8_t>(slash == std::string::npos ? 0 : std::atoi(parts[1].substr(slash + 1).c_str()));
            beat.tempo = static_cast<int16_t>(std::atoi(parts[2].c_str()));
            beats_.push_back(beat);
        }
        else if (parts[0] == "MRK_CHORD" && parts.size() >= 2)
        {
            chords_.push_back({marker.position, static_cast<uint16_t>(std::atoi(parts[1].c_str()))});
        }
    }

    // The quarter notes of a bar: 4 / d in 8 bits, times n. A division by 0 gives 0, as on the CPU.
    static int32_t Quarters(const Beat& beat)
    {
        const int32_t unit = beat.denominator == 0 ? 0 : 4 / beat.denominator;
        return static_cast<int32_t>(static_cast<int8_t>(unit)) * beat.numerator;
    }

    void ResetCounters(int64_t origin)
    {
        counters_.fill(1);
        origin_ = origin;
    }

    // Whether the pair passes a marker at `pos`, as the search for `kind` decides it, with the jump `from` to `to` that
    // the search read before it started. Without a jump, the pair passes what lies after `prev`, up to `cur`. After a
    // jump, it passes what lies from the jump's target up to `cur`. The search for beats (fn_c9b90) passes everything
    // up to `cur` after a jump that doesn't go forward. The search for chords (fn_cb890) doesn't.
    static bool Passes(MarkerKind kind, int64_t prev, int64_t cur, int64_t pos, int64_t from, int64_t to)
    {
        if ((prev | cur | pos) == 0)
        {
            return true;
        }

        const bool forward = prev < pos && cur >= pos;
        if (from == -1 || to >= cur)
        {
            return forward;
        }
        if (cur == prev)
        {
            return false;
        }
        if (from <= to ? from >= cur : prev <= cur)
        {
            return forward;
        }

        return ((kind == MarkerKind::kBeat && from >= to) || to <= pos) && cur >= pos;
    }

    // The beats that the pair passes (fn_c9b90). The search starts at the first beat past the last position, and goes
    // round all of them. Each one passed becomes the beat, and the counters start again there.
    void TakeBeats(int64_t prev, int64_t cur)
    {
        const std::size_t n = beats_.size();
        std::size_t index = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (last_ != 0 ? last_ < beats_[i].position : beats_[i].position >= 0)
            {
                index = i;
                break;
            }
        }

        const int64_t from = jump_from_;
        const int64_t to = jump_to_;
        for (std::size_t k = 0; k < n; ++k, index = (index + 1) % n)
        {
            if (beat_ != index && Passes(MarkerKind::kBeat, prev, cur, beats_[index].position, from, to))
            {
                beat_ = index;
                ResetCounters(beats_[index].position);
            }
        }
    }

    // One counter (fn_cb4ac): the subdivisions of the bar that the pair crosses, while fewer than `parts` a bar.
    static int32_t Step(int32_t counter, float parts, double bar, double cur, double prev, int32_t modulus, bool clip)
    {
        if (!(std::ceil(parts) > static_cast<float>(counter)))
        {
            return counter;
        }

        const double each = bar / static_cast<double>(parts);
        const int32_t now = static_cast<int32_t>(cur / each) % modulus;
        const int32_t before = static_cast<int32_t>(prev / each) % modulus;
        int32_t diff = now - before;
        if (clip && diff >= modulus)
        {
            diff = modulus;
        }

        return counter + (diff != 0 && now != 0 ? diff : 0);
    }

    // The counters of the bar's subdivisions (fn_cb4ac), from where the counters started: halves, quarters, eighths,
    // triplet eighths, sixteenths, triplet sixteenths and thirty-seconds. The first counter stays.
    void Count(int64_t prev, int64_t cur)
    {
        if (!beat_)
        {
            return;
        }

        const Beat& beat = beats_[*beat_];
        const double d_cur = static_cast<double>(cur - origin_) - static_cast<double>(offset_);
        const double d_prev = static_cast<double>(prev - origin_) - static_cast<double>(offset_);
        const int32_t quarters = Quarters(beat);
        const float seconds = 60.0f / static_cast<float>(beat.tempo);
        const float samples = seconds * static_cast<float>(sample_rate_);
        const int32_t beat_samples =
            std::isfinite(samples) ? static_cast<int32_t>(samples) : std::numeric_limits<int32_t>::max();
        const float wholes = static_cast<float>(quarters) * 0.25f;
        const double bar = static_cast<double>(static_cast<int64_t>(quarters) * beat_samples);

        counters_[1] = Step(counters_[1], wholes + wholes, bar, d_cur, d_prev, 2, false);
        counters_[2] = Step(counters_[2], wholes * 4.0f, bar, d_cur, d_prev, 4, true);
        counters_[3] = Step(counters_[3], wholes * 8.0f, bar, d_cur, d_prev, 8, true);
        counters_[4] = Step(counters_[4], wholes * 12.0f, bar, d_cur, d_prev, 12, true);
        counters_[5] = Step(counters_[5], wholes * 16.0f, bar, d_cur, d_prev, 16, true);
        counters_[6] = Step(counters_[6], wholes * 24.0f, bar, d_cur, d_prev, 24, true);
        counters_[7] = Step(counters_[7], wholes * 32.0f, bar, d_cur, d_prev, 32, true);
    }

    // The chords that the pair passes (fn_cb890), in the markers' order. Each sets the chord. One that comes once the
    // thirty-seconds have counted a whole bar starts the counters again.
    void TakeChords(int64_t prev, int64_t cur)
    {
        for (const Chord& chord : chords_)
        {
            const int64_t pos = chord.position;
            if (!Passes(MarkerKind::kChord, prev, cur, pos, jump_from_, jump_to_))
            {
                continue;
            }

            chord_ = chord.code;
            if (counters_[7] != 0 && beat_)
            {
                const float thirty_seconds = static_cast<float>(Quarters(beats_[*beat_])) * 0.25f * 32.0f;
                if (thirty_seconds > static_cast<float>(counters_[7]))
                {
                    continue;
                }
            }
            ResetCounters(pos);
        }
    }

    int32_t sample_rate_ = 0;
    std::vector<Beat> beats_;
    std::vector<Chord> chords_;
    int64_t jump_from_ = -1;                                  // +0x88
    int64_t jump_to_ = -1;                                    // +0x90
    int64_t last_ = 0;                                        // +0x28
    int32_t offset_ = 0;                                      // +0x7c: 0 from the constructor on
    int64_t origin_ = 0;                                      // +0xc8
    std::optional<std::size_t> beat_;                         // +0x30
    std::array<int32_t, 8> counters_{1, 1, 1, 1, 1, 1, 1, 1}; // +0x38
    std::array<int32_t, 8> published_{};                      // +0x58
    uint16_t chord_ = 0xFFFF;                                 // +0x78
};

} // namespace

StreamCues ReadStreamCues(std::span<const uint8_t> file)
{
    if (file.size() < kHeaderSize || file[0] != 'F' || file[1] != 'S' || file[2] != 'T' || file[3] != 'M' ||
        xsflib::LoadLe16(&file[4]) != 0xFEFF)
    {
        throw xsflib::FormatError("not a stream file of the Switch");
    }

    StreamCues cues;
    const uint16_t blocks = xsflib::LoadLe16(&file[kBlockCount]);
    Need(file, kHeaderSize + std::size_t{blocks} * kBlockEntry);
    for (uint16_t i = 0; i < blocks; ++i)
    {
        const std::size_t entry = kHeaderSize + std::size_t{i} * kBlockEntry;
        const uint16_t type = xsflib::LoadLe16(&file[entry]);
        const uint32_t offset = xsflib::LoadLe32(&file[entry + 4]);
        const uint32_t size = xsflib::LoadLe32(&file[entry + 8]);
        Need(file, std::size_t{offset} + size);
        if (type == kInfoBlock)
        {
            Need(file, std::size_t{offset} + 16);
            const std::size_t settings = offset + 8 + static_cast<int32_t>(xsflib::LoadLe32(&file[offset + 12]));
            Need(file, settings + 8);
            cues.sample_rate = xsflib::LoadLe32(&file[settings + 4]);
        }
        else if (type == kRegionBlock && size >= 0x20)
        {
            for (std::size_t at = offset + 0x20; at + kRegionEntry <= std::size_t{offset} + size; at += kRegionEntry)
            {
                cues.regions.push_back(
                    {xsflib::LoadLe32(&file[at]), xsflib::LoadLe32(&file[at + 4]), Name(file, at + kRegionName)});
            }
        }
        else if (type == kMarkerBlock && size >= 12)
        {
            const uint32_t count = xsflib::LoadLe32(&file[offset + 8]);
            Need(file, offset + 12 + std::size_t{count} * kMarkerEntry);
            for (uint32_t m = 0; m < count; ++m)
            {
                const std::size_t at = offset + 12 + std::size_t{m} * kMarkerEntry;
                cues.markers.push_back({xsflib::LoadLe32(&file[at]), Name(file, at + 8)});
            }
        }
    }

    return cues;
}

std::vector<RegionSequence> ReadJumpSetting(std::span<const uint8_t> file)
{
    MessagePack reader(file, "the jump setting");
    const Value root = reader.Read();
    const auto* map = root.Map();
    if (!map)
    {
        throw xsflib::FormatError("the jump setting isn't a map");
    }

    for (const auto& [key, value] : *map)
    {
        const std::string* name = key.String();
        const auto* list = value.Array();
        if (!name || *name == "system" || !list)
        {
            continue;
        }

        std::vector<RegionSequence> sequences;
        for (const Value& item : *list)
        {
            const Value* label = item.Get("label");
            const Value* next = item.Get("param1");
            if (!label || !label->String() || !next || !next->String())
            {
                throw xsflib::FormatError("a region sequence lacks its label or the next sequence");
            }

            RegionSequence sequence{*label->String(), *next->String(), {}};
            for (int p = 3;; ++p)
            {
                const Value* region = item.Get("param" + std::to_string(p));
                if (!region || !region->String() || region->String()->empty())
                {
                    break;
                }
                sequence.regions.push_back(*region->String());
            }
            sequences.push_back(std::move(sequence));
        }

        return sequences;
    }

    throw xsflib::FormatError("the jump setting holds no region sequences");
}

RegionOrder PlayOrder(const StreamCues& cues, const std::vector<RegionSequence>& sequences)
{
    if (sequences.empty())
    {
        throw xsflib::FormatError("the jump setting holds no region sequences");
    }

    std::map<std::string, std::size_t> region_index;
    for (std::size_t i = 0; i < cues.regions.size(); ++i)
    {
        region_index.emplace(cues.regions[i].name, i);
    }
    std::map<std::string, const RegionSequence*> by_label;
    for (const RegionSequence& sequence : sequences)
    {
        by_label.emplace(sequence.label, &sequence);
    }

    RegionOrder order;
    std::map<std::string, std::size_t> first_region;
    std::string label = sequences.front().label;
    while (!first_region.contains(label))
    {
        const auto found = by_label.find(label);
        if (found == by_label.end())
        {
            throw xsflib::FormatError("the jump setting lacks the region sequence " + label);
        }

        first_region.emplace(label, order.regions.size());
        for (const std::string& region : found->second->regions)
        {
            const auto index = region_index.find(region);
            if (index == region_index.end())
            {
                throw xsflib::FormatError("the stream lacks the region " + region);
            }
            order.regions.push_back(index->second);
        }
        label = found->second->next;
    }

    order.loop = first_region.at(label);

    return order;
}

std::vector<SyncFrame> RunMusicSync(const StreamCues& cues, const RegionOrder& order, uint32_t frame_samples,
                                    int passes)
{
    std::vector<std::size_t> timeline = order.regions;
    for (int pass = 1; pass < passes && order.loop < order.regions.size(); ++pass)
    {
        timeline.insert(timeline.end(), order.regions.begin() + static_cast<std::ptrdiff_t>(order.loop),
                        order.regions.end());
    }

    MarkerSystem sync(cues);
    sync.Attach();

    std::vector<SyncFrame> frames;
    std::size_t index = 0;
    uint64_t into = 0;
    std::vector<bool> reported(timeline.size(), false);
    bool first = true;
    while (index < timeline.size())
    {
        const StreamRegion& region = cues.regions[timeline[index]];
        if (index + 1 < timeline.size() && !reported[index] && cues.regions[timeline[index + 1]].start != region.end)
        {
            sync.Jump(region.end, cues.regions[timeline[index + 1]].start);
            reported[index] = true;
        }
        if (!first)
        {
            sync.Update(static_cast<int64_t>(region.start + into));
        }
        first = false;
        frames.push_back(sync.State());

        into += frame_samples;
        while (index < timeline.size() &&
               into >= cues.regions[timeline[index]].end - cues.regions[timeline[index]].start)
        {
            into -= cues.regions[timeline[index]].end - cues.regions[timeline[index]].start;
            ++index;
        }
    }

    return frames;
}

std::vector<ProcedureWrite> UserProcedureWrites(const std::vector<SyncFrame>& frames, int16_t index)
{
    // The table at 0x15d6798: the counter for each value of variable 14, or -1 for none.
    constexpr std::array<int, 8> kCounterOf = {-1, 0, 1, 4, 2, -1, 6, -1};
    if (index < 0 || index >= static_cast<int16_t>(kCounterOf.size()) || kCounterOf[index] < 0)
    {
        throw std::invalid_argument("user procedure 2 has no counter for " + std::to_string(index));
    }

    const auto counter_of = [index, &kCounterOf](const SyncFrame& frame)
    {
        return frame.has_beat ? static_cast<uint16_t>(frame.counters[kCounterOf[index]]) : uint16_t{0xFFFF};
    };

    std::vector<ProcedureWrite> writes;
    if (frames.empty())
    {
        return writes;
    }

    int16_t tempo = frames[0].tempo;
    uint16_t count = counter_of(frames[0]);
    writes.push_back({0, 4, 15, tempo});
    writes.push_back({0, 3, 13, static_cast<int16_t>(count)});
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        const SyncFrame& frame = frames[f];
        const uint16_t now = counter_of(frame);
        if (now != count)
        {
            count = now;
            writes.push_back({static_cast<uint32_t>(f), 2, 14, -1});
            writes.push_back({static_cast<uint32_t>(f), 2, 13, static_cast<int16_t>(now)});
            writes.push_back({static_cast<uint32_t>(f), 2, 12, static_cast<int16_t>(frame.chord)});
        }
        if (frame.tempo != tempo)
        {
            tempo = frame.tempo;
            writes.push_back({static_cast<uint32_t>(f), 4, 15, tempo});
        }
    }

    return writes;
}

namespace
{

// The board streams of Super Mario Party Jamboree (0100965017338000 version 0) whose first user parameter has bit 7 set
// (0x180) in _Resident/AddonAudioProject.fspj: the stream sounds that attach the music sync as they start (fn_c2e6c,
// fn_c4fb4). The paths of their files come from the archive's file table, and their jump settings are in audio.nx.bea.
constexpr SyncedStream kJamboreeStreams[] = {
    {"SM_BGM_BD01_MAIN_JMP", "stream/SM_BGM_BD01_MAIN_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD01_MAIN_JMP.msgpack"},
    {"SM_BGM_BD01_MAIN_LS_JMP", "stream/SM_BGM_BD01_MAIN_LS_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD01_MAIN_LS_JMP.msgpack"},
    {"SM_BGM_BD02_MAIN_JMP", "stream/SM_BGM_BD02_MAIN_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD02_MAIN_JMP.msgpack"},
    {"SM_BGM_BD02_MAIN_LS_JMP", "stream/SM_BGM_BD02_MAIN_LS_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD02_MAIN_LS_JMP.msgpack"},
    {"SM_BGM_BD03_MAIN_JMP", "stream/SM_BGM_BD03_MAIN_JMP.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD03_MAIN_JMP.msgpack"},
    {"SM_BGM_BD03_MAIN_LS_JMP", "stream/SM_BGM_BD03_MAIN_LS_JMP.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD03_MAIN_LS_JMP.msgpack"},
    {"SM_BGM_BD04_MAIN_JMP", "stream/SM_BGM_BD04_MAIN_JMP.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD04_MAIN_JMP.msgpack"},
    {"SM_BGM_BD04_MAIN_LS_JMP", "stream/SM_BGM_BD04_MAIN_LS_JMP.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD04_MAIN_LS_JMP.msgpack"},
    {"SM_BGM_BD05_MAIN_JMP", "stream/SM_BGM_BD05_MAIN_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD05_MAIN_JMP.msgpack"},
    {"SM_BGM_BD05_MAIN_LS_JMP", "stream/SM_BGM_BD05_MAIN_LS_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD05_MAIN_LS_JMP.msgpack"},
    {"SM_BGM_BD05_MAIN_TIMESALE_JMP", "stream/SM_BGM_BD05_MAIN_TIMESALE_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD05_MAIN_TIMESALE_JMP.msgpack"},
    {"SM_BGM_BD06_MAIN_JMP", "stream/SM_BGM_BD06_MAIN_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD06_MAIN_JMP.msgpack"},
    {"SM_BGM_BD06_MAIN_LS_JMP", "stream/SM_BGM_BD06_MAIN_LS_JMP.dspadpcm.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD06_MAIN_LS_JMP.msgpack"},
    {"SM_BGM_BD07_MAIN_JMP", "stream/SM_BGM_BD07_MAIN_JMP.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD07_MAIN_JMP.msgpack"},
    {"SM_BGM_BD07_MAIN_LS_JMP", "stream/SM_BGM_BD07_MAIN_LS_JMP.bfstm",
     "audio/jump_setting/conv/SM_BGM_BD07_MAIN_LS_JMP.msgpack"},
};

// Jamboree's star melody, the only sound of subarc_bgm_bd01_star_melody.fsst. StarSpot::RunSelect (0x29b75c) starts
// it on the second of the game's two music players while the board's stream plays on the first. Its sequence writes 4
// to variable 14 before each call of procedure 2 (offset 731).
constexpr SyncedPiece kStarMelody[] = {{"SQ_BGM_STAR_MELODY", 4, kJamboreeStreams}};

constexpr std::size_t kStarMelodyArchiveSize = 462'848;
constexpr std::string_view kStarMelodyArchiveSha1 = "af9f39f60480d9f520871c82ae68fc098990369d";

} // namespace

std::span<const SyncedPiece> SyncedPieces(std::span<const uint8_t> archive)
{
    if (archive.size() != kStarMelodyArchiveSize ||
        xsflib::Sha1::Hex(xsflib::Sha1::Of(archive)) != kStarMelodyArchiveSha1)
    {
        return {};
    }

    return kStarMelody;
}

SyncedPlay PlaySynced(const SyncedPiece& piece, std::span<const uint8_t> stream, std::span<const uint8_t> jump_setting,
                      uint32_t extra)
{
    const StreamCues cues = ReadStreamCues(stream);
    if (cues.sample_rate == 0 || cues.sample_rate % 60 != 0)
    {
        throw xsflib::FormatError("the stream's sample rate of " + std::to_string(cues.sample_rate) +
                                  " Hz isn't a multiple of 60");
    }

    // A frame of the game at `g` comes 10/3 frames of 5 ms after the frame before it.
    const auto sound_frame = [](uint64_t g)
    {
        return static_cast<uint32_t>((g * 10 + 2) / 3);
    };

    const RegionOrder order = PlayOrder(cues, ReadJumpSetting(jump_setting));
    const uint32_t frame_samples = cues.sample_rate / 60;
    SyncedPlay play;
    const std::size_t game_frames = RunMusicSync(cues, order, frame_samples, 2).size();
    play.frames = sound_frame(game_frames);
    std::vector<SyncFrame> frames = RunMusicSync(cues, order, frame_samples, 3);
    frames.resize(std::min<std::size_t>(frames.size(), game_frames + std::size_t{extra} * 3 / 10 + 1));
    play.writes = UserProcedureWrites(frames, piece.index);
    for (ProcedureWrite& write : play.writes)
    {
        write.frame = sound_frame(write.frame);
    }

    return play;
}

} // namespace ntdlib::nx::bex

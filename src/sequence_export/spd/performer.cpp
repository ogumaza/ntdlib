// SPDX-License-Identifier: MIT

// The performer of Nintendo SPD's sounds (see convert.h): the model's writes to the sound unit's registers as MIDI
// events.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"

#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/spd/convert.h"
#include "ntdlib/spd/loops.h"
#include "ntdlib/spd/player.h"
#include "sources.h"

namespace ntdlib::sequence_export::spd
{
namespace
{

namespace sp = ntdlib::spd;
using seqlib::midi::EventKind;
using seqlib::midi::TrackOutput;

constexpr uint32_t kChannelRegisters = 0x04000400;
constexpr uint32_t kCaptureArea = 0x83C00; // the capture buffers, from the sound area's start
constexpr uint32_t kCaptureAreaEnd = 0x87C00;

// The system clock's cycles in a frame of the driver: 4,096 ticks of the OS timer.
constexpr double kFrameCycles = 4096.0 * 64.0;

// The longest a sound may play before its loop: an hour. A sound that stops changing with no track left to play
// stops 10 s after its last change.
constexpr uint32_t kMaxFrames = static_cast<uint32_t>(3600.0 / sp::kFrameSeconds);
constexpr uint32_t kHoldFrames = static_cast<uint32_t>(10.0 / sp::kFrameSeconds);

// The driver's ticks that the MIDI file's quarter note holds.
constexpr int kTicksPerQuarter = 48;

const LoopRules kLoopRules{{}, false};

// A hardware channel's registers as the driver last wrote them.
struct Registers
{
    uint8_t volume = 0;
    uint8_t pan = 0;
    uint16_t timer = 0;
    uint32_t address = 0; // SOUNDxSAD as the driver writes it
    uint16_t loop = 0;
    uint32_t length = 0;
    uint8_t control = 0; // SOUNDxCNT bits 24-31
};

// The note that a channel plays.
struct Voice
{
    uint32_t id = 0;
    uint8_t key = 60;
    int root = 60;       // the source's key at `base`
    double base = 1.0;   // the timer's period at which the source plays at `root`
    bool once = false;   // the sample plays once, and ends after `length` samples
    double played = 0.0; // samples played so far
    double length = 0.0; // samples to play
    double samples_per_word = 0.0;
    uint16_t period = 1;
    std::size_t strike = 0; // the note's index in the performer's strikes
};

// A note's start: its frame, its key and its source.
struct Strike
{
    uint32_t frame = 0;
    uint8_t key = 0;
    Source source;
    bool audible = false; // the channel's volume rose above 0 while the note played
};

double Semitones(double base, uint16_t period)
{
    return 12.0 * std::log2(base / period);
}

// CC11 for a channel at hardware `volume` with `pan`: the level and the pan's gain, as a square.
uint8_t ExpressionOf(uint8_t volume, uint8_t pan)
{
    const double level = volume / 128.0 * PanGain(pan);
    return static_cast<uint8_t>(std::clamp<long>(std::lround(127.0 * std::sqrt(level)), 0, 127));
}

class Performer
{
public:
    Performer(const SoundData& data, int slot, int sound, const PerformOptions& options)
        : memory_(data), player_(data.header, *data.set_a, data.set_b, data.set_b_slot, memory_.layout)
    {
        player_.SetBranchFlag(options.branch_flag);
        player_.Post(sp::StartMessage(slot, sound));
        reverb_ = static_cast<uint8_t>(player_.Reverb() & 0x7F); // the start-up's level, from the header
        for (std::size_t c = 0; c < outputs_.size(); c++)
        {
            outputs_[c].midi_track = static_cast<uint8_t>(c);
        }
    }

    // Plays frames until the sound stops, holds, or every track that plays has jumped back `passes` times.
    void Play(uint32_t passes)
    {
        uint32_t last_change = 0;
        while (frame_ < kMaxFrames)
        {
            const std::vector<sp::RegisterWrite> writes = player_.Frame();
            const bool changed = Record(writes);
            NoteReverb();
            EndSamples();
            loops_.Note(player_, frame_);
            NoteTempo();
            last_change = changed ? frame_ : last_change;
            frame_++;

            if (!player_.Busy())
            {
                return;
            }

            if (loops_.Looped(player_, passes))
            {
                return;
            }
            if (!sp::LoopWatch::Reading(player_) && frame_ - last_change >= kHoldFrames)
            {
                out_.holds = true;
                return;
            }
        }
        out_.truncated = true;
    }

    uint32_t Frames() const
    {
        return frame_;
    }

    // The loop of the track with the longest (loops.h).
    std::optional<sp::TrackLoop> LongestLoop() const
    {
        return loops_.Longest();
    }

    // Ends every note that still sounds, and gives the performance its end and its tempo.
    Performance Finish(uint32_t end_frame)
    {
        for (std::size_t c = 0; c < voices_.size(); c++)
        {
            EndNote(c, frame_);
        }
        seqlib::midi::EndPassAt(out_, end_frame);
        out_.end_tick = end_frame;
        out_.seconds = end_frame * sp::kFrameSeconds;

        // A quarter note holds the frames of 48 ticks at the first tempo.
        const int tempo = first_tempo_ == 0 ? sp::kTickFrames : first_tempo_;
        out_.timebase = static_cast<uint32_t>(
            std::max(1L, std::lround(static_cast<double>(kTicksPerQuarter) * sp::kTickFrames / tempo)));
        recorder_.SetTempo(out_, 0, 60.0 / (out_.timebase * sp::kFrameSeconds));
        if (captures_ > 0)
        {
            out_.approximations["capture reverb as CC91, the synth's reverb at the driver's return level"] += captures_;
        }
        for (const auto& [what, count] : recorder_.Approximations())
        {
            out_.approximations[what] += count;
        }
        out_.warnings.insert(out_.warnings.end(), recorder_.Warnings().begin(), recorder_.Warnings().end());

        return out_;
    }

    const Performance& Recorded() const
    {
        return out_;
    }

    // Whether the pass from `start` to `end` strikes the notes of the pass after it, each within a frame of its time.
    // Notes that stay silent don't count. The driver keeps a tick's fraction of a frame from pass to pass. Where a
    // loop's ticks don't fill whole frames, a note moves by a frame from one pass to the next.
    bool StrikesAsTheNextPass(uint32_t start, uint32_t end) const
    {
        const uint32_t length = end - start;
        const auto pass = [this](uint32_t from, uint32_t to)
        {
            std::vector<Strike> strikes;
            for (const Strike& strike : strikes_)
            {
                if (strike.audible && strike.frame >= from && strike.frame < to)
                {
                    strikes.push_back(strike);
                }
            }
            return strikes;
        };

        // Each strike of the pass requires a twin in the next one. The strikes of a frame come in the order of the
        // channels, and a frame's notes may go to other channels in the next pass.
        const std::vector<Strike> a = pass(start, end);
        const std::vector<Strike> b = pass(end, end + length);
        if (a.size() != b.size())
        {
            return false;
        }

        std::vector<bool> matched(b.size(), false);
        std::size_t from = 0;
        for (const Strike& x : a)
        {
            const int64_t at = static_cast<int64_t>(x.frame - start);
            while (from < b.size() && static_cast<int64_t>(b[from].frame - end) < at - 1)
            {
                from++;
            }

            bool found = false;
            for (std::size_t i = from; i < b.size() && static_cast<int64_t>(b[i].frame - end) <= at + 1; i++)
            {
                if (!matched[i] && b[i].key == x.key && b[i].source == x.source)
                {
                    matched[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
        }

        return true;
    }

private:
    // The first tempo of the started sound's first track.
    void NoteTempo()
    {
        if (first_tempo_ != 0)
        {
            return;
        }

        for (const sp::PlayerState& player : player_.Players())
        {
            if ((player.state & 0xF) != 0 && !player.tracks.empty() && player.tracks[0].tempo != 0)
            {
                first_tempo_ = player.tracks[0].tempo;
                return;
            }
        }
    }

    void EndNote(std::size_t channel, uint32_t frame)
    {
        Voice& voice = voices_[channel];
        if (voice.id == 0)
        {
            return;
        }

        recorder_.NoteOff(out_, outputs_[channel].midi_track, frame, outputs_[channel].part, voice.key, voice.id);
        voice = Voice{};
    }

    // Applies a frame's writes to the channels' registers, then records the starts, stops and changes. Returns whether
    // any register took another value.
    bool Record(const std::vector<sp::RegisterWrite>& writes)
    {
        std::array<bool, sp::kChannels> started = {};
        std::array<bool, sp::kChannels> stopped = {};
        bool changed = false;
        for (const sp::RegisterWrite& w : writes)
        {
            if (w.address < kChannelRegisters || w.address >= kChannelRegisters + sp::kChannels * 16)
            {
                continue;
            }

            const std::size_t c = (w.address - kChannelRegisters) / 16;
            Registers& r = registers_[c];
            const Registers before = r;
            switch (w.address & 0xF)
            {
            case 0x0:
                r.volume = static_cast<uint8_t>(w.value & 0x7F);
                if (w.size == 4)
                {
                    r.pan = static_cast<uint8_t>((w.value >> 16) & 0x7F);
                    r.control = static_cast<uint8_t>(w.value >> 24);
                    stopped[c] = stopped[c] || (r.control & 0x80) == 0;
                }
                break;
            case 0x2:
                r.pan = static_cast<uint8_t>(w.value & 0x7F);
                break;
            case 0x3:
                r.control = static_cast<uint8_t>(w.value);
                if ((r.control & 0x80) != 0)
                {
                    started[c] = true;
                    stopped[c] = false;
                }
                else
                {
                    stopped[c] = true;
                }
                break;
            case 0x4:
                r.address = w.value;
                break;
            case 0x8:
                r.timer = static_cast<uint16_t>(w.value);
                break;
            case 0xA:
                r.loop = static_cast<uint16_t>(w.value);
                break;
            case 0xC:
                r.length = w.value & 0x3FFFFF;
                break;
            default:
                break;
            }
            changed = changed || std::tie(before.volume, before.pan, before.timer, before.control) !=
                                     std::tie(r.volume, r.pan, r.timer, r.control);
        }

        for (std::size_t c = 0; c < sp::kChannels; c++)
        {
            if (started[c])
            {
                Start(c);
            }
            else if (stopped[c])
            {
                EndNote(c, frame_);
            }
            else if (voices_[c].id != 0)
            {
                Update(c);
            }
        }

        return changed;
    }

    // A channel's start ends its note and starts the next one.
    void Start(std::size_t c)
    {
        EndNote(c, frame_);
        const Registers& r = registers_[c];
        const int format = (r.control >> 5) & 3;
        const uint32_t data = r.address & 0x07FFFFFC;
        if (format != 3 && data >= memory_.layout.area + kCaptureArea && data < memory_.layout.area + kCaptureAreaEnd)
        {
            captures_++;
            return;
        }

        Source source;
        source.control = static_cast<uint8_t>(r.control & 0x7F);
        Voice voice;
        if (format == 3)
        {
            source.noise = c >= 14;
            voice.root = kToneKey;
            voice.base = kToneTimer;
        }
        else
        {
            source.header = r.address - kHeaderToData;
            source.loop = r.loop;
            source.length = r.length;
            voice.root = memory_.Read8(source.header + 2);
            voice.base = memory_.Read16(source.header + 4);
            voice.once = ((r.control >> 3) & 3) != 1;
            voice.samples_per_word = format == 2 ? 8.0 : format == 1 ? 2.0 : 4.0;
            voice.length = (static_cast<double>(r.loop) + r.length - (format == 2 ? 1 : 0)) * voice.samples_per_word;
            if (voice.once && voice.length <= 0.0)
            {
                recorder_.Approximate("notes of an empty sample left out");
                return;
            }
        }

        voice.period = static_cast<uint16_t>(std::max(1, 0x10000 - r.timer));
        const double semitones = Semitones(voice.base, voice.period);
        const int key = std::clamp(voice.root + static_cast<int>(std::lround(semitones)), 0, 127);

        // CC91 is the capture reverb's return level, or 0 while the reverb is off: a synth that follows General MIDI 2
        // gives a channel without CC91 a send of 40.
        TrackOutput& output = outputs_[c];
        recorder_.SendSetting(out_, output, frame_, EventKind::kVolume, 127);
        recorder_.SendSetting(out_, output, frame_, EventKind::kPan, PanController(r.pan));
        recorder_.SendSetting(out_, output, frame_, EventKind::kExpression, ExpressionOf(r.volume, r.pan));
        recorder_.SendSetting(out_, output, frame_, EventKind::kReverb, reverb_);
        recorder_.SendPitch(out_, output, frame_, static_cast<float>(semitones - (key - voice.root)));
        recorder_.SendPreset(out_, output, frame_, seqlib::midi::PresetIndex(out_.presets, source));

        voice.id = recorder_.NoteOn(out_, output, frame_, static_cast<uint8_t>(key), 127);
        voice.strike = strikes_.size();
        strikes_.push_back({frame_, static_cast<uint8_t>(key), source, r.volume > 0});
        voice.key = static_cast<uint8_t>(key);
        voices_[c] = voice;
    }

    void Update(std::size_t c)
    {
        const Registers& r = registers_[c];
        Voice& voice = voices_[c];
        strikes_[voice.strike].audible = strikes_[voice.strike].audible || r.volume > 0;
        TrackOutput& output = outputs_[c];
        recorder_.SendSetting(out_, output, frame_, EventKind::kPan, PanController(r.pan));
        recorder_.SendSetting(out_, output, frame_, EventKind::kExpression, ExpressionOf(r.volume, r.pan));
        voice.period = static_cast<uint16_t>(std::max(1, 0x10000 - r.timer));
        recorder_.SendPitch(out_, output, frame_,
                            static_cast<float>(Semitones(voice.base, voice.period) - (voice.key - voice.root)));
    }

    // The capture reverb records the mix and plays it back on channels 1 and 3 at the reverb's level, the channels'
    // volume. Every channel goes through it. The nearest form is therefore the synth's reverb with that level as each
    // channel's send. A change of the level goes to every channel that sounds, and a note's start sends the level.
    void NoteReverb()
    {
        const auto level = static_cast<uint8_t>(player_.Reverb() & 0x7F);
        if (level == reverb_)
        {
            return;
        }

        reverb_ = level;
        for (std::size_t c = 0; c < voices_.size(); c++)
        {
            if (voices_[c].id != 0)
            {
                recorder_.SendSetting(out_, outputs_[c], frame_, EventKind::kReverb, reverb_);
            }
        }
    }

    // A sample that plays once ends in the frame after the one in which its last sample plays.
    void EndSamples()
    {
        for (std::size_t c = 0; c < voices_.size(); c++)
        {
            Voice& voice = voices_[c];
            if (voice.id == 0 || !voice.once)
            {
                continue;
            }

            voice.played += kFrameCycles / 2.0 / voice.period;
            if (voice.played >= voice.length)
            {
                EndNote(c, frame_ + 1);
            }
        }
    }

    Memory memory_;
    sp::Player player_;
    Performance out_;
    seqlib::midi::Recorder recorder_;
    std::array<TrackOutput, sp::kChannels> outputs_;
    std::array<Voice, sp::kChannels> voices_;
    std::array<Registers, sp::kChannels> registers_;
    sp::LoopWatch loops_;
    std::vector<Strike> strikes_;
    uint32_t frame_ = 0;
    int first_tempo_ = 0;
    uint32_t captures_ = 0;
    uint8_t reverb_ = 0; // the capture reverb's level, CC91
};

} // namespace

Performance Perform(const SoundData& data, int slot, int sound, const PerformOptions& options)
{
    // Play two passes more than the loops. The second pass then has a pass after it to compare with.
    const int loops = std::max(options.loops, 1);
    Performer performer(data, slot, sound, options);
    performer.Play(static_cast<uint32_t>(loops) + 2);
    const std::optional<sp::TrackLoop> loop = performer.LongestLoop();
    if (!loop || loop->jumps.size() < static_cast<std::size_t>(loops) + 1)
    {
        return performer.Finish(performer.Frames());
    }

    // The passes of the longest loop. The first pass whose notes the next pass strikes again is the marked one. The
    // first pass can lose notes to the notes of the sound's start that still sound, and so can the second.
    std::vector<std::pair<uint32_t, uint32_t>> passes = {{*loop->start, loop->jumps[0]}};
    for (std::size_t i = 1; i < loop->jumps.size(); i++)
    {
        passes.emplace_back(loop->jumps[i - 1], loop->jumps[i]);
    }
    std::optional<std::size_t> marked;
    for (std::size_t i = 0; i < 2 && i + static_cast<std::size_t>(loops) < passes.size() && !marked; i++)
    {
        if (performer.StrikesAsTheNextPass(passes[i].first, passes[i].second))
        {
            marked = i;
        }
    }

    const std::size_t pass = marked.value_or(0);
    const uint32_t loop_start = passes[pass].first;
    const uint32_t loop_end = passes[pass].second;
    const uint32_t end = passes[pass + static_cast<std::size_t>(loops) - 1].second;
    const bool repeats = RepeatsAsTheGame(performer.Recorded(), {}, loop_start, loop_end, kLoopRules);
    const PassStart next_pass = PassStartAt(performer.Recorded(), {}, loop_end);
    Performance out = performer.Finish(end);
    out.loop = seqlib::midi::Performance::Loop{loop_start, loop_end};
    out.loops = loops;
    RestoreAtLoopStart(out, {}, next_pass, kLoopRules);
    if (!marked)
    {
        out.warnings.push_back("the loop's passes play other notes; the markers loop the first");
    }
    else if (!repeats)
    {
        out.approximations
            ["loops whose next pass moves notes by a frame or to other channels (the markers repeat "
             "one pass)"]++;
    }

    return out;
}

} // namespace ntdlib::sequence_export::spd

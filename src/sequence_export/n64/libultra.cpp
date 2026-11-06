// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/libultra.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <utility>

#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/n64/rom_data.h"
#include "ntdlib/n64/seq_file.h"
#include "ntdlib/n64/vadpcm.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

namespace n64 = ntdlib::n64;
namespace midi = seqlib::midi;

// MIDI's status bytes.
constexpr uint8_t kStatusMask = 0xF0;
constexpr uint8_t kChannelMask = 0x0F;
constexpr uint8_t kNoteOff = 0x80;
constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kPolyPressure = 0xA0;
constexpr uint8_t kControlChange = 0xB0;
constexpr uint8_t kProgramChange = 0xC0;
constexpr uint8_t kChannelPressure = 0xD0;
constexpr uint8_t kPitchBend = 0xE0;

// The controllers that the sequence players act on.
constexpr uint8_t kControlVolume = 7;
constexpr uint8_t kControlPan = 10;
constexpr uint8_t kControlSustain = 64;
constexpr uint8_t kControlEffect = 91;

// The controllers that only Ambrella's player acts on. Its loop's controllers go by the code at 0x800055C4.
constexpr uint8_t kControlWheel = 1;
constexpr uint8_t kControlSweepTime = 5;
constexpr uint8_t kControlExpression = 11;
constexpr uint8_t kControlSweep = 65;
constexpr uint8_t kControlLoopStart = 102;
constexpr uint8_t kControlLoopEnd = 103;

// The controllers that only HAL's player acts on, and their values at the start. The code at 0x8002F0A4 reads them.
constexpr uint8_t kControlHalChannelScale = 8;
constexpr uint8_t kControlHalPlayerScale = 21;
constexpr int kHalChannelScale = 127;
constexpr int kHalPlayerScale = 120;
constexpr double kHalChannelScaleFull = 128.0;

constexpr int kChannels = 16;
constexpr int kPercussionChannel = 9;

// A standard MIDI file's header, and each track's.
constexpr std::size_t kMidiHeaderSize = 14;
constexpr std::size_t kMidiTrackHeaderSize = 8;

// The tempo that a sequence starts with, in microseconds a quarter note.
constexpr float kDefaultTempo = 500000.0F;

// The count in a loop's end that loops for ever.
constexpr uint8_t kLoopForever = 0xFF;

// The volume at the top of the player's scale, and the voice's volume when every factor stands at its top: the
// player's fixed-point product. The mixer squares it, out of 32768.
constexpr int kFullPlayerVolume = 0x7FFF;
constexpr double kFullVoiceVolume = 31507.0 / 32768.0;

// Ambrella's player, read at 0x80006254 and 0x80007420 in Hey You, Pikachu!. A voice's volume at the top of every
// factor: a controller of 127 scales by 16256/16384, and the envelope's level by 0xF000/0x10000. A channel starts at a
// volume of 100 and at the bank's percussion, with 64 as its last key (the reset at 0x80005F30). A bend covers 200
// cents. A note's pitch takes the key map's detune away.
constexpr double kAmbrellaFullVoiceVolume = 29769.0 / 32768.0;
constexpr int kAmbrellaBendCents = 200;
constexpr uint8_t kAmbrellaChannelVolume = 100;
constexpr int kAmbrellaFirstKey = 64;

// The player's update lasts this long. Its oscillators step through a period in 16 / rate seconds. The bank's depths
// scale CC1's value over 127 by 127 / 128 twice over.
constexpr double kAmbrellaUpdateSeconds = 0.00575;
constexpr double kAmbrellaLfoUnits = 16.0;
constexpr double kAmbrellaWheelVibrato = 127.0 * 127.0 / (128.0 * 128.0);
constexpr double kAmbrellaWheelTremolo = 127.0 * 255.0 / (128.0 * 256.0 * 256.0);

// A sweep moves this many cents, over CC5's value and 1, in each update. The file holds a step for two of them.
constexpr double kAmbrellaSweepUnits = 768.0;
constexpr double kAmbrellaSweepStepUpdates = 2.0;

// A ramp to nothing ends at an amplitude of 1/32768: 90.3 dB down. A voice with an attack starts there.
constexpr double kFloorDb = 90.31;

// The velocity that the envelope's times are fitted at. A ramp to nothing takes the same time from any level. A quieter
// note falls more slowly in decibels. SoundFont's envelopes can't follow that.
constexpr double kNominalVelocity = 100.0;

// Each channel's gain at its centre pan, in the mixer's sin/cos law.
constexpr double kCentreDb = -3.01;

// The places where a decay's fit meets a straight line in amplitude. Each is a share of the line's time.
constexpr double kLineRampEarlyPlace = 0.5;
constexpr double kLineRampLatePlace = 0.9;

// A release in a straight line of amplitude falls like a line in decibels that takes this many times its time for 100
// dB. That line falls 20 dB in the release's time. A longer one follows the top of the ramp more closely and adds a
// tail that the game doesn't have. A shorter one falls too soon.
constexpr double kLineReleaseTo100Db = 5.0;

// The resampler's greatest pitch: just under an octave up.
constexpr int kOctaveCents = 1200;

// The SDK's oscillators. A sine has 259 - rate steps and the others 256 - rate. Each step lasts 16 ms, and a delay's
// unit 16.384 ms.
constexpr int kTremoloSine = 1;
constexpr int kVibratoSine = 128;
constexpr int kFirstVibrato = 128;
constexpr int kSineSteps = 259;
constexpr int kOtherSteps = 256;
constexpr double kOscillatorStepMs = 16.0;
constexpr double kOscillatorDelayMs = 16.384;

// The base of an oscillator's depth in cents: a vibrato's depth is this to the power of the bank's depth.
constexpr double kVibratoDepthBase = 1.03099303;

// A channel's sound in a player. A voice whose sound has no release ends at once.
struct Voice
{
    int key = 0;
    uint32_t note = 0;
    uint8_t part = 0;
    bool ends_at_once = false;
};

// A loop that a track repeats for ever: the ticks of its start and its end.
struct ForeverLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
};

// The place of a track in its song.
struct TrackState
{
    std::size_t index = 0;
    uint32_t next_tick = 0;
    bool ended = false;
    std::optional<ForeverLoop> forever;
    uint32_t end_tick = 0;
    std::map<std::size_t, uint8_t> loop_counts;
    std::map<std::size_t, uint32_t> loop_ticks;

    // Where a loop of Ambrella's player starts: the next event, and its tick.
    std::optional<std::size_t> loop_start_index;
    uint32_t loop_start_tick = 0;
};

// A note-off that a compressed note on schedules.
struct PendingOff
{
    int channel = 0;
    int key = 0;
};

class Performer
{
public:
    Performer(const LibultraSong& song, const n64::BankFile& file, const n64::Bank& bank,
              const LibultraSettings& settings, const PerformOptions& options)
        : song_(song), file_(file), bank_(bank), settings_(settings), options_(options)
    {
    }

    LibultraPerformance Run();

private:
    void SetTempo(uint32_t tick, float tempo);
    double SecondsAt(uint32_t tick) const;
    void SetInstrument(int channel, int32_t instrument, uint32_t tick);
    void HandleEvent(int track, const n64::MidiEvent& event, uint32_t tick);
    void HandleChannelMessage(const n64::MidiEvent& event, uint32_t tick);
    void HandleNoteOn(int channel, int key, int velocity, uint32_t duration, uint32_t tick);
    void HandleNoteOff(int channel, int key, uint32_t tick);
    bool HandleLoopEnd(TrackState& state, const n64::MidiEvent& event, uint32_t tick);
    void PlanLoop();
    double ReleaseMs(int32_t instrument) const;
    void Finish(uint32_t tick);
    void SendExpression(int channel, uint32_t tick);
    void SendPitch(int channel, uint32_t tick);
    void StartSweep(int channel, int key, uint32_t tick);
    void RunSweeps(uint32_t until_tick);
    double TicksPerUpdate(uint32_t tick) const;

    const LibultraSong& song_;
    const n64::BankFile& file_;
    const n64::Bank& bank_;
    const LibultraSettings& settings_;
    const PerformOptions& options_;

    LibultraPerformance out_;
    midi::Recorder recorder_;
    std::array<midi::TrackOutput, kChannels> outputs_;
    std::array<int32_t, kChannels> instruments_{};
    int hal_player_scale_ = kHalPlayerScale;
    std::array<int, kChannels> hal_channel_scales_{};
    std::array<int, kChannels> bend_ranges_{};
    std::array<std::vector<Voice>, kChannels> voices_;

    // What Ambrella's player holds for each channel: CC11, the effect's share, the bend in cents, CC5 and CC65, the
    // last key, and a sweep from that key: the cents that are left and the next step's tick.
    std::array<int, kChannels> expressions_{};
    std::array<int, kChannels> effects_{};
    std::array<double, kChannels> bend_cents_{};
    std::array<int, kChannels> sweep_times_{};
    std::array<bool, kChannels> sweeps_on_{};
    std::array<int, kChannels> last_keys_{};
    std::array<double, kChannels> sweep_cents_{};
    std::array<uint32_t, kChannels> sweep_ticks_{};
    std::multimap<uint32_t, PendingOff> pending_offs_;
    std::vector<TrackState> tracks_;

    // The tempo map.
    struct TempoSegment
    {
        uint32_t tick = 0;
        double seconds = 0.0;
        double uspt = 0.0;
    };

    std::vector<TempoSegment> tempo_;

    // Where the performance stops, once every track has ended or found its loop.
    std::optional<uint32_t> stop_tick_;
    std::optional<midi::Performance::Loop> loop_;
    double last_release_end_ = 0.0;
};

void Performer::SetTempo(uint32_t tick, float tempo)
{
    // The player's microseconds a tick are a whole number, in single precision.
    const float ticks_per_quarter = 1.0F / static_cast<float>(song_.division);
    const double uspt = std::max(static_cast<int32_t>(tempo * ticks_per_quarter), 1) / settings_.rate;
    tempo_.push_back({tick, SecondsAt(tick), uspt});
    recorder_.SetTempo(out_.performance, tick, 60000000.0 / (uspt * song_.division));
}

double Performer::SecondsAt(uint32_t tick) const
{
    if (tempo_.empty())
    {
        return 0.0;
    }

    const TempoSegment& last = tempo_.back();
    return last.seconds + (static_cast<double>(tick) - last.tick) * last.uspt / 1000000.0;
}

void Performer::SetInstrument(int channel, int32_t instrument, uint32_t tick)
{
    const n64::BankInstrument& object = file_.instruments[static_cast<std::size_t>(instrument)];
    instruments_[static_cast<std::size_t>(channel)] = instrument;
    if (settings_.player == LibultraPlayer::kAmbrella)
    {
        return;
    }

    bend_ranges_[static_cast<std::size_t>(channel)] = object.bend_range;

    midi::TrackOutput& output = outputs_[static_cast<std::size_t>(channel)];
    recorder_.SendSetting(out_.performance, output, tick, midi::EventKind::kVolume, object.volume);
    recorder_.SendSetting(out_.performance, output, tick, midi::EventKind::kPan, object.pan);
}

double Performer::ReleaseMs(int32_t instrument) const
{
    double longest = 0.0;
    for (const int32_t sound : file_.instruments[static_cast<std::size_t>(instrument)].sounds)
    {
        const int32_t envelope = file_.sounds[static_cast<std::size_t>(sound)].envelope;
        if (envelope != n64::kNoObject)
        {
            longest = std::max(longest, file_.envelopes[static_cast<std::size_t>(envelope)].release_time / 1000.0);
        }
    }

    return longest;
}

// The binary search of the players' sound lookup. A note that it doesn't find plays no sound.
int32_t LookupSound(const n64::BankFile& file, const n64::BankInstrument& instrument, int key, int velocity)
{
    int low = 1;
    int high = static_cast<int>(instrument.sounds.size());
    while (high >= low)
    {
        const int middle = (low + high) / 2;
        const int32_t sound = instrument.sounds[static_cast<std::size_t>(middle - 1)];
        const int32_t key_map = file.sounds[static_cast<std::size_t>(sound)].key_map;
        if (key_map == n64::kNoObject)
        {
            return n64::kNoObject;
        }

        const n64::BankKeyMap& map = file.key_maps[static_cast<std::size_t>(key_map)];
        if (key >= map.key_min && key <= map.key_max && velocity >= map.velocity_min && velocity <= map.velocity_max)
        {
            return sound;
        }

        if (key < map.key_min || (velocity < map.velocity_min && key <= map.key_max))
        {
            high = middle - 1;
        }
        else
        {
            low = middle + 1;
        }
    }

    return n64::kNoObject;
}

void Performer::SendExpression(int channel, uint32_t tick)
{
    // The mixer takes the send's share from the dry level, on the sin/cos law. The controller acts squared.
    const std::size_t index = static_cast<std::size_t>(channel);
    const double dry = std::cos(std::numbers::pi / 2.0 * effects_[index] / 127.0);
    const double expression = settings_.player == LibultraPlayer::kHal
                                  ? hal_player_scale_ * hal_channel_scales_[index] / kHalChannelScaleFull
                                  : expressions_[index];
    recorder_.SendSetting(out_.performance, outputs_[index], tick, midi::EventKind::kExpression,
                          static_cast<uint8_t>(std::lround(expression * std::sqrt(dry))));
}

void Performer::SendPitch(int channel, uint32_t tick)
{
    const std::size_t index = static_cast<std::size_t>(channel);
    recorder_.SendPitch(out_.performance, outputs_[index], tick,
                        static_cast<float>((bend_cents_[index] + sweep_cents_[index]) / 100.0));
}

double Performer::TicksPerUpdate(uint32_t tick) const
{
    const TempoSegment& segment =
        *std::prev(std::ranges::upper_bound(tempo_, tick, {}, [](const TempoSegment& s) { return s.tick; }));

    return kAmbrellaUpdateSeconds * 1000000.0 / segment.uspt;
}

// A note takes over from the channel's last key. The player sweeps each voice, and a channel's other voices move along
// here. The channels that sweep play one note at a time.
void Performer::StartSweep(int channel, int key, uint32_t tick)
{
    const std::size_t index = static_cast<std::size_t>(channel);
    if (sweeps_on_[index])
    {
        sweep_cents_[index] = (last_keys_[index] - key) * 100.0;
        sweep_ticks_[index] = tick;
        SendPitch(channel, tick);
    }

    last_keys_[index] = key;
}

void Performer::RunSweeps(uint32_t until_tick)
{
    for (int channel = 0; channel < kChannels; channel++)
    {
        const std::size_t index = static_cast<std::size_t>(channel);
        while (sweep_cents_[index] != 0.0)
        {
            const double updates = std::max(kAmbrellaSweepStepUpdates * TicksPerUpdate(sweep_ticks_[index]), 1.0);
            const uint32_t next = sweep_ticks_[index] + static_cast<uint32_t>(std::lround(updates));
            if (next > until_tick)
            {
                break;
            }

            const double step = kAmbrellaSweepStepUpdates * std::floor(kAmbrellaSweepUnits / (sweep_times_[index] + 1));
            const double left = std::max(std::abs(sweep_cents_[index]) - step, 0.0);
            sweep_cents_[index] = std::copysign(left, sweep_cents_[index]);
            sweep_ticks_[index] = next;
            SendPitch(channel, next);
        }
    }
}

void Performer::HandleNoteOn(int channel, int key, int velocity, uint32_t duration, uint32_t tick)
{
    const std::size_t index = static_cast<std::size_t>(channel);
    const n64::BankInstrument& instrument = file_.instruments[static_cast<std::size_t>(instruments_[index])];
    const int32_t sound = LookupSound(file_, instrument, key, velocity);
    if (sound == n64::kNoObject)
    {
        return;
    }

    // The Ambrella player updates the last key only for a note that finds a sound.
    if (settings_.player == LibultraPlayer::kAmbrella)
    {
        StartSweep(channel, key, tick);
    }

    // A program goes out with its channel's first note. A program that plays no note then stays out of the SoundFont.
    midi::TrackOutput& output = outputs_[index];
    recorder_.SendPreset(out_.performance, output, tick, midi::PresetIndex(out_.instruments, instruments_[index]));
    const uint32_t note =
        recorder_.NoteOn(out_.performance, output, tick, static_cast<uint8_t>(key), static_cast<uint8_t>(velocity));
    const int32_t envelope = file_.sounds[static_cast<std::size_t>(sound)].envelope;
    const bool ends_at_once =
        envelope == n64::kNoObject || file_.envelopes[static_cast<std::size_t>(envelope)].release_time == 0;
    voices_[index].push_back({key, note, output.part, ends_at_once});
    if (duration != 0)
    {
        pending_offs_.insert({tick + duration, {channel, key}});
    }
}

void Performer::HandleNoteOff(int channel, int key, uint32_t tick)
{
    std::vector<Voice>& voices = voices_[static_cast<std::size_t>(channel)];
    const auto release = [&](const Voice& voice)
    {
        recorder_.NoteOff(out_.performance, static_cast<uint8_t>(channel), tick, voice.part, static_cast<uint8_t>(key),
                          voice.note);
        last_release_end_ = std::max(
            last_release_end_, SecondsAt(tick) + ReleaseMs(instruments_[static_cast<std::size_t>(channel)]) / 1000.0);
    };

    // The Ambrella player walks the voice list from the newest voice (the code at 0x800067BC). It releases every voice
    // of the key. A sound with no release ends its voice at once. The walk then leaves the list.
    if (settings_.player == LibultraPlayer::kAmbrella)
    {
        for (std::size_t place = voices.size(); place-- > 0;)
        {
            if (voices[place].key != key)
            {
                continue;
            }

            const bool ends_walk = voices[place].ends_at_once;
            release(voices[place]);
            voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(place));
            if (ends_walk)
            {
                break;
            }
        }

        return;
    }

    // The other players release the first voice of the key.
    const auto found = std::ranges::find_if(voices, [key](const Voice& voice) { return voice.key == key; });
    if (found != voices.end())
    {
        release(*found);
        voices.erase(found);
    }
}

void Performer::HandleChannelMessage(const n64::MidiEvent& event, uint32_t tick)
{
    const int channel = event.status & kChannelMask;
    const std::size_t index = static_cast<std::size_t>(channel);
    midi::TrackOutput& output = outputs_[index];
    switch (event.status & kStatusMask)
    {
    case kNoteOn:
        if (event.data2 != 0)
        {
            HandleNoteOn(channel, event.data1, event.data2, event.duration, tick);
            break;
        }

        [[fallthrough]];

    case kNoteOff:
        HandleNoteOff(channel, event.data1, tick);
        break;

    case kControlChange:
        if (settings_.player == LibultraPlayer::kAmbrella)
        {
            switch (event.data1)
            {
            case kControlWheel:
                recorder_.SendSetting(out_.performance, output, tick, midi::EventKind::kModulation, event.data2);
                break;

            case kControlExpression:
                expressions_[index] = event.data2;
                SendExpression(channel, tick);
                break;

            case kControlSweepTime:
                sweep_times_[index] = event.data2;
                break;

            case kControlSweep:
                sweeps_on_[index] = event.data2 != 0;
                break;

            default:
                break;
            }
        }

        if (settings_.player == LibultraPlayer::kHal)
        {
            if (event.data1 == kControlHalChannelScale)
            {
                hal_channel_scales_[index] = event.data2;
                SendExpression(channel, tick);
            }
            else if (event.data1 == kControlHalPlayerScale)
            {
                hal_player_scale_ = event.data2;
                for (int other = 0; other < kChannels; other++)
                {
                    SendExpression(other, tick);
                }
            }
        }

        switch (event.data1)
        {
        case kControlVolume:
            recorder_.SendSetting(out_.performance, output, tick, midi::EventKind::kVolume, event.data2);
            break;

        case kControlPan:
            recorder_.SendSetting(out_.performance, output, tick, midi::EventKind::kPan, event.data2);
            break;

        case kControlSustain:
            recorder_.SendSetting(out_.performance, output, tick, midi::ControlKind(kControlSustain), event.data2);
            break;

        case kControlEffect:
            effects_[index] = event.data2;
            SendExpression(channel, tick);
            if (settings_.effect != BusEffect::kNone)
            {
                recorder_.SendSetting(out_.performance, output, tick, midi::EventKind::kReverb, event.data2);
            }

            break;

        default:
            break;
        }

        break;

    case kProgramChange:
        if (event.data1 < bank_.instruments.size() && bank_.instruments[event.data1] != n64::kNoObject)
        {
            SetInstrument(channel, bank_.instruments[event.data1], tick);
        }

        break;

    case kPitchBend:
        {
            const int bend = (event.data2 << 7) + event.data1 - 8192;
            bend_cents_[index] = bend_ranges_[index] * bend / 8192;
            SendPitch(channel, tick);
            break;
        }

    case kPolyPressure:
    case kChannelPressure:
        recorder_.Approximate("aftertouch left out");
        break;

    default:
        break;
    }
}

bool Performer::HandleLoopEnd(TrackState& state, const n64::MidiEvent& event, uint32_t tick)
{
    if (event.loop_start < 0 || event.payload.size() < 2)
    {
        return false;
    }

    // The first byte holds the count, and the second the count that's left. A count of 0xFF repeats for ever. A count
    // that has run out is set again for the next time the track reaches the loop.
    const std::size_t index = state.index;
    if (!state.loop_counts.contains(index))
    {
        state.loop_counts[index] = event.payload[1];
    }

    uint8_t& left = state.loop_counts[index];
    if (left == 0)
    {
        left = event.payload[0];
        return false;
    }

    const std::size_t start = static_cast<std::size_t>(event.loop_start);
    if (left != kLoopForever)
    {
        left--;
    }
    else if (!state.forever)
    {
        state.forever = ForeverLoop{state.loop_ticks[start], tick};
    }

    // The player goes on from the bytes after the loop's start.
    state.index = start + 1;

    return true;
}

// Once every track has ended or found its loop, the performance repeats. The main loop starts when the last track has
// joined it, and lasts until every track's loop comes round together.
void Performer::PlanLoop()
{
    if (stop_tick_)
    {
        return;
    }

    uint32_t start = 0;
    uint64_t length = 1;
    bool any_loop = false;
    for (const TrackState& state : tracks_)
    {
        if (state.ended)
        {
            start = std::max(start, state.end_tick);
        }
        else if (state.forever)
        {
            any_loop = true;
            start = std::max(start, state.forever->start);
            length = std::lcm(length, static_cast<uint64_t>(state.forever->end - state.forever->start));
        }
        else
        {
            return;
        }
    }

    if (!any_loop)
    {
        return;
    }

    const uint64_t end = start + length * static_cast<uint64_t>(options_.loops);
    if (end >= UINT32_MAX || SecondsAt(static_cast<uint32_t>(start + length)) > kLongestSeconds)
    {
        return;
    }

    stop_tick_ = static_cast<uint32_t>(end);
    loop_ = midi::Performance::Loop{start, static_cast<uint32_t>(start + length)};
}

void Performer::Finish(uint32_t tick)
{
    Performance& performance = out_.performance;
    const double sound_end = last_release_end_;

    // A note that's still on stops where the performance stops.
    for (int channel = 0; channel < kChannels; channel++)
    {
        for (const Voice& voice : voices_[static_cast<std::size_t>(channel)])
        {
            recorder_.NoteOff(performance, static_cast<uint8_t>(channel), tick, voice.part,
                              static_cast<uint8_t>(voice.key), voice.note);
        }
    }

    if (loop_)
    {
        // The next pass starts at the end of the last. What it plays stays out, and the notes stop there.
        midi::ClampNoteOffs(performance, tick);
        midi::CutAt(performance, tick);
        performance.loop = loop_;
        performance.loops = options_.loops;
        performance.end_tick = tick;
        performance.seconds = SecondsAt(tick);
    }
    else
    {
        // The file ends once the last release has ended.
        const TempoSegment& last = tempo_.back();
        const double seconds = std::max(sound_end, SecondsAt(tick));
        const double ticks = (seconds - last.seconds) * 1000000.0 / last.uspt + last.tick;
        performance.end_tick = std::max(tick, static_cast<uint32_t>(std::ceil(ticks)));
        performance.seconds = seconds;
    }

    performance.approximations = recorder_.Approximations();
    performance.warnings = recorder_.Warnings();
}

void Performer::HandleEvent(int track, const n64::MidiEvent& event, uint32_t tick)
{
    TrackState& state = tracks_[static_cast<std::size_t>(track)];
    state.index++;
    if (event.status == n64::kMidiMeta)
    {
        switch (event.type)
        {
        case n64::kMidiMetaTempo:
            SetTempo(tick, static_cast<float>(event.TempoMicroseconds()));
            break;

        case n64::kMidiMetaEndOfTrack:
            state.ended = true;
            state.end_tick = tick;
            break;

        case n64::kCompressedLoopStart:
            state.loop_ticks[state.index - 1] = tick;
            break;

        case n64::kCompressedLoopEnd:
            state.index--;
            if (!HandleLoopEnd(state, event, tick))
            {
                state.index++;
            }

            break;

        default:
            break;
        }
    }
    else if (settings_.player == LibultraPlayer::kAmbrella && (event.status & kStatusMask) == kControlChange &&
             event.data1 == kControlLoopStart)
    {
        state.loop_start_index = state.index;
        state.loop_start_tick = tick;
    }
    else if (settings_.player == LibultraPlayer::kAmbrella && (event.status & kStatusMask) == kControlChange &&
             event.data1 == kControlLoopEnd && state.loop_start_index)
    {
        // The player ends every note that's on. From the fourth repeat, it plays each repeat but every third with a
        // random set of the channels 9 to 16. The loop in the file is the song as it first plays.
        for (int channel = 0; channel < kChannels; channel++)
        {
            while (!voices_[static_cast<std::size_t>(channel)].empty())
            {
                HandleNoteOff(channel, voices_[static_cast<std::size_t>(channel)].front().key, tick);
            }
        }

        state.forever = ForeverLoop{state.loop_start_tick, tick};
        state.index = *state.loop_start_index;
    }
    else if (event.IsChannel())
    {
        HandleChannelMessage(event, tick);
    }

    const std::vector<n64::MidiEvent>& events = song_.tracks[static_cast<std::size_t>(track)];
    if (state.ended || state.index >= events.size())
    {
        state.ended = true;
        state.end_tick = std::max(state.end_tick, tick);
        return;
    }

    state.next_tick = tick + events[state.index].delta;
}

LibultraPerformance Performer::Run()
{
    Performance& performance = out_.performance;
    performance.timebase = static_cast<uint32_t>(song_.division);
    for (int channel = 0; channel < kChannels; channel++)
    {
        outputs_[static_cast<std::size_t>(channel)].midi_track = static_cast<uint8_t>(channel);
    }

    SetTempo(0, kDefaultTempo);

    // Each channel starts on the bank's first instrument, and channel 10 on the bank's percussion.
    const auto first = std::ranges::find_if(bank_.instruments, [](int32_t i) { return i != n64::kNoObject; });
    if (first == bank_.instruments.end())
    {
        throw xsflib::FormatError("the bank has no instruments");
    }

    for (int channel = 0; channel < kChannels; channel++)
    {
        const std::size_t index = static_cast<std::size_t>(channel);
        const bool ambrella = settings_.player == LibultraPlayer::kAmbrella;
        const bool percussion = (ambrella || channel == kPercussionChannel) && bank_.percussion != n64::kNoObject;
        SetInstrument(channel, percussion ? bank_.percussion : *first, 0);
        expressions_[index] = 127;
        hal_channel_scales_[index] = kHalChannelScale;
        if (settings_.player == LibultraPlayer::kHal)
        {
            SendExpression(channel, 0);
        }

        if (ambrella)
        {
            bend_ranges_[index] = kAmbrellaBendCents;
            last_keys_[index] = kAmbrellaFirstKey;
            recorder_.SendSetting(performance, outputs_[index], 0, midi::EventKind::kVolume, kAmbrellaChannelVolume);
        }
    }

    tracks_.resize(song_.tracks.size());
    for (std::size_t i = 0; i < song_.tracks.size(); i++)
    {
        if (song_.tracks[i].empty())
        {
            tracks_[i].ended = true;
        }
        else
        {
            tracks_[i].next_tick = song_.tracks[i][0].delta;
        }
    }

    uint32_t tick = 0;
    while (true)
    {
        // The lowest track with the nearest event goes next. A note's end at the same tick goes before it.
        int next = -1;
        for (int i = 0; i < static_cast<int>(tracks_.size()); i++)
        {
            const TrackState& state = tracks_[static_cast<std::size_t>(i)];
            if (!state.ended && (next < 0 || state.next_tick < tracks_[static_cast<std::size_t>(next)].next_tick))
            {
                next = i;
            }
        }

        const bool stopped =
            next >= 0 && stop_tick_ && tracks_[static_cast<std::size_t>(next)].next_tick >= *stop_tick_;
        const uint32_t next_tick = next < 0 || stopped ? UINT32_MAX : tracks_[static_cast<std::size_t>(next)].next_tick;
        while (!pending_offs_.empty() && pending_offs_.begin()->first <= next_tick &&
               (!stop_tick_ || pending_offs_.begin()->first < *stop_tick_))
        {
            const auto [off_tick, off] = *pending_offs_.begin();
            pending_offs_.erase(pending_offs_.begin());
            tick = std::max(tick, off_tick);
            HandleNoteOff(off.channel, off.key, off_tick);
        }

        if (settings_.player == LibultraPlayer::kAmbrella)
        {
            RunSweeps(next_tick != UINT32_MAX ? next_tick : stop_tick_.value_or(tick));
        }

        if (next < 0)
        {
            break;
        }

        if (stopped)
        {
            tick = *stop_tick_;
            break;
        }

        tick = next_tick;
        if (SecondsAt(tick) > kLongestSeconds)
        {
            performance.truncated = true;
            break;
        }

        TrackState& state = tracks_[static_cast<std::size_t>(next)];
        const std::vector<n64::MidiEvent>& events = song_.tracks[static_cast<std::size_t>(next)];
        HandleEvent(next, events[state.index], tick);
        PlanLoop();
    }

    Finish(tick);
    return std::move(out_);
}

// The times of a zone's envelope on a library whose ramps are straight lines in amplitude. SoundFont's and SFZ's
// attacks are such lines. Their decays and releases are straight lines in decibels. A ramp to nothing stands at -6 dB
// at half of its time, at -20 dB at 9/10 of it and at nothing at its end. No straight line in decibels meets all three.
// The fit follows the top 20 dB. A listener hears that part. A tail remains where the ramp has ended.
void ApplyLineRamps(Zone& zone, double attack, double decay, double attack_ms, double decay_ms, double release_ms)
{
    zone.attack_ms = attack_ms;
    if (decay_ms > 0.0 && decay < attack)
    {
        // A hold, then a line in decibels through the ramp's level at the two places. A fall to nothing stands at -23
        // dB at the ramp's end, and at -40 dB at 1.5 times its time.
        const double end = (decay / attack) * (decay / attack);
        const auto fall_at = [&](double share)
        {
            return -20.0 * std::log10(1.0 - (1.0 - end) * share);
        };

        const double db_per_ramp =
            (fall_at(kLineRampLatePlace) - fall_at(kLineRampEarlyPlace)) / (kLineRampLatePlace - kLineRampEarlyPlace);
        zone.hold_ms = decay_ms * (kLineRampEarlyPlace - fall_at(kLineRampEarlyPlace) / db_per_ramp);
        zone.decay_ms = decay_ms * 100.0 / db_per_ramp;
        zone.sustain_db = decay > 0.0 ? -20.0 * std::log10(end) : 100.0;
    }
    else if (decay_ms > 0.0 && decay > attack)
    {
        // A rise after the attack. The fit lets the attack run on to the decay's end.
        zone.delay_ms = attack_ms;
        zone.attack_ms = decay_ms;
    }

    // A release can't hold, and a line falls to nothing in its time from any level.
    zone.release_ms = release_ms * kLineReleaseTo100Db;
}

// The envelope and the level of a zone. `before_envelope` is the amplitude of the zone without its envelope.
void ApplyEnvelope(Zone& zone, const n64::BankEnvelope& envelope, double before_envelope, bool line_ramps)
{
    const double attack = envelope.attack_volume / 127.0;
    const double decay = envelope.decay_volume / 127.0;
    const double attack_ms = std::max(envelope.attack_time, 0) / 1000.0;
    const double decay_ms = std::max(envelope.decay_time, 0) / 1000.0;
    const double release_ms = std::max(envelope.release_time, 0) / 1000.0;

    // The top is where the envelope's volume is highest. A ramp at once goes straight to its target.
    const double top = decay_ms == 0.0 ? decay : std::max(attack, decay);
    zone.gain = before_envelope * top * top;
    if (line_ramps)
    {
        ApplyLineRamps(zone, attack, decay, attack_ms, decay_ms, release_ms);
        return;
    }

    // How far above the mixer's floor the top is, for a typical note. The ramps run on a straight line in decibels.
    const double nominal = kNominalVelocity / 127.0;
    const double top_db = 20.0 * std::log10(std::max(zone.gain * nominal * nominal, 1e-7)) + kCentreDb;
    const double range_db = std::max(kFloorDb + top_db, 1.0);

    // A rise from the floor in `attack_ms`. SoundFont and SFZ attacks are straight lines in amplitude. A line of the
    // same energy has this length, and the same end.
    if (attack_ms > 0.0)
    {
        const double line_ms = std::min(attack_ms, attack_ms * 3.0 * 10.0 / (range_db * std::numbers::ln10));
        zone.delay_ms = attack_ms - line_ms;
        zone.attack_ms = line_ms;
    }

    double sustain_db = 0.0;
    if (decay_ms > 0.0 && decay < attack)
    {
        // A fall to the decay's volume, or to the floor.
        const double fall_db = decay > 0.0 ? 40.0 * std::log10(attack / decay) : range_db;
        zone.decay_ms = decay_ms * 100.0 / fall_db;
        zone.sustain_db = decay > 0.0 ? fall_db : 100.0;
        sustain_db = fall_db;
    }
    else if (decay_ms > 0.0 && decay > attack)
    {
        // A rise after the attack. The fit lets the attack run on to the decay's end.
        zone.delay_ms = attack_ms;
        zone.attack_ms = decay_ms;
    }

    // A release falls from where the note stands to the floor in the release's time. A fall to the floor has no sustain
    // to start from. A note of that kind usually ends well before its decay's end, so the fit takes the release from
    // the top.
    const double release_range_db = decay_ms > 0.0 && decay == 0.0 ? range_db : std::max(range_db - sustain_db, 1.0);
    zone.release_ms = release_ms * 100.0 / release_range_db;
}

// A tremolo's or a vibrato's LFO: the steps of a period and the delay. A wave other than a sine keeps its period and
// its range.
Lfo OscillatorLfo(int type, int rate, int delay)
{
    Lfo lfo;
    const bool sine = type == kTremoloSine || type == kVibratoSine;
    const int steps = std::max((sine ? kSineSteps : kOtherSteps) - rate, 1);
    lfo.hz = 1000.0 / (kOscillatorStepMs * steps);
    lfo.delay_ms = delay * kOscillatorDelayMs;

    return lfo;
}

void ApplyOscillators(Zone& zone, const n64::BankInstrument& instrument)
{
    if (instrument.tremolo_type != 0 && instrument.tremolo_type < kFirstVibrato)
    {
        // The tremolo holds the voice's volume between 127 and 127 less its depth. The mixer squares it.
        const int depth = std::min<int>(instrument.tremolo_depth & 0xFE, 126);
        Lfo lfo = OscillatorLfo(instrument.tremolo_type, instrument.tremolo_rate, instrument.tremolo_delay);
        lfo.depth = 20.0 * std::log10(127.0 / (127.0 - depth));
        zone.tremolo = lfo;
    }

    if (instrument.vibrato_type >= kFirstVibrato)
    {
        Lfo lfo = OscillatorLfo(instrument.vibrato_type, instrument.vibrato_rate, instrument.vibrato_delay);
        lfo.depth = std::pow(kVibratoDepthBase, instrument.vibrato_depth);
        zone.vibrato = lfo;
    }
}

// Ambrella's player moves a note's pitch and volume by CC1 and the bank's depths and rates. It reads neither the
// oscillators' types nor their delays.
void ApplyAmbrellaWheel(Zone& zone, const n64::BankInstrument& instrument)
{
    if (instrument.vibrato_depth != 0 && instrument.vibrato_rate != 0)
    {
        zone.vibrato = Lfo{.hz = instrument.vibrato_rate / kAmbrellaLfoUnits,
                           .depth = instrument.vibrato_depth * kAmbrellaWheelVibrato,
                           .wheel = true};
    }

    if (instrument.tremolo_depth != 0 && instrument.tremolo_rate != 0)
    {
        const double fall = std::min(instrument.tremolo_depth * kAmbrellaWheelTremolo, 0.99);
        zone.tremolo = Lfo{
            .hz = instrument.tremolo_rate / kAmbrellaLfoUnits, .depth = -20.0 * std::log10(1.0 - fall), .wheel = true};
    }
}

// A run of velocities that plays one sound.
struct VelocityRun
{
    friend bool operator==(const VelocityRun&, const VelocityRun&) = default;

    int low = 0;
    int high = 0;
    int32_t sound = n64::kNoObject;
};

// The runs of a key: what the players' lookup finds at each velocity.
std::vector<VelocityRun> RunsOfKey(const n64::BankFile& file, const n64::BankInstrument& instrument, int key)
{
    std::vector<VelocityRun> runs;
    for (int velocity = 1; velocity <= 127; velocity++)
    {
        const int32_t sound = LookupSound(file, instrument, key, velocity);
        if (!runs.empty() && runs.back().sound == sound)
        {
            runs.back().high = velocity;
        }
        else
        {
            runs.push_back({velocity, velocity, sound});
        }
    }

    std::erase_if(runs, [](const VelocityRun& run) { return run.sound == n64::kNoObject; });
    if (!runs.empty() && runs.front().low == 1)
    {
        runs.front().low = 0;
    }

    return runs;
}

std::string HexName(uint32_t value)
{
    char name[16];
    std::snprintf(name, sizeof(name), "w%08X", value);

    return name;
}

std::string SequenceName(std::size_t index, std::span<const SongName> names, const char* beside = nullptr)
{
    char base[32];
    std::snprintf(base, sizeof(base), "Sequence 0x%02zX", index);
    std::string inside;
    for (const SongName& song : names)
    {
        if (song.index == index)
        {
            inside = song.name;
            break;
        }
    }

    if (beside != nullptr)
    {
        inside += (inside.empty() ? "" : ", ") + std::string(beside);
    }

    return inside.empty() ? std::string(base) : std::string(base) + " (" + inside + ")";
}

// Takes a track's events from a compressed MIDI file or a standard one.
LibultraSong ReadSong(std::span<const uint8_t> bytes)
{
    LibultraSong song;
    if (n64::CompressedMidi::Matches(bytes))
    {
        const n64::CompressedMidi file = n64::CompressedMidi::Parse(bytes);
        song.division = file.Division();
        for (std::size_t track = 0; track < n64::CompressedMidi::kTrackCount; track++)
        {
            song.tracks.push_back(file.DecodeTrack(track));
        }
    }
    else
    {
        const n64::StandardMidi file = n64::StandardMidi::Parse(bytes);
        song.division = file.Division();
        for (std::size_t track = 0; track < file.TrackCount(); track++)
        {
            song.tracks.push_back(file.DecodeTrack(track));
        }
    }

    return song;
}

// The volume that the game gives its player for song `index`.
int SongVolume(std::span<const uint8_t> image, const LibultraProfile& profile, std::size_t index)
{
    if (index < profile.volume_list.size())
    {
        return profile.volume_list[index];
    }

    const SongVolumes& table = profile.volumes;
    if (table.stride == 0)
    {
        return profile.volume;
    }

    if (index >= table.count)
    {
        return table.other;
    }

    const std::size_t place = table.offset + index * table.stride;
    if (place + 2 > image.size())
    {
        return table.other;
    }

    return xsflib::LoadBe16(image.data() + place);
}

} // namespace

std::shared_ptr<const Wave> DecodeWave(std::span<const uint8_t> bytes, const n64::VadpcmBook* book,
                                       const n64::BankLoop* loop, std::string name, uint32_t rate)
{
    auto out = std::make_shared<Wave>();
    out->name = std::move(name);
    out->rate = rate;
    std::vector<int16_t> samples;
    if (book != nullptr)
    {
        samples = n64::DecodeVadpcm(bytes, *book);
    }
    else
    {
        samples.reserve(bytes.size() / 2);
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
        {
            samples.push_back(static_cast<int16_t>(xsflib::LoadBe16(bytes.data() + i)));
        }
    }

    if (loop != nullptr && loop->count != 0 && loop->end > loop->start && loop->end <= samples.size())
    {
        // A VADPCM wave's repeats start from the state that the loop holds, at the frame of the loop's start. Where
        // they differ from the first pass, the file keeps one pass more.
        samples.resize(loop->end);
        out->pcm.loop = true;
        out->pcm.loop_start = loop->start;
        out->pcm.loop_end = loop->end;
        if (book != nullptr && loop->has_state)
        {
            const std::size_t first_frame = loop->start / n64::kVadpcmFrameSamples;
            n64::VadpcmState state = loop->state;
            const std::vector<int16_t> rest =
                n64::DecodeVadpcm(bytes.subspan((first_frame + 1) * n64::kVadpcmFrameSize), *book, state);
            std::vector<int16_t> repeat(loop->state.begin(), loop->state.end());
            repeat.insert(repeat.end(), rest.begin(), rest.end());
            const std::size_t skip = loop->start - first_frame * n64::kVadpcmFrameSamples;
            const std::size_t length = loop->end - loop->start;
            if (repeat.size() >= skip + length &&
                !std::equal(samples.begin() + loop->start, samples.end(), repeat.begin() + static_cast<long>(skip)))
            {
                samples.insert(samples.end(), repeat.begin() + static_cast<long>(skip),
                               repeat.begin() + static_cast<long>(skip + length));
                out->pcm.loop_start = loop->end;
                out->pcm.loop_end = loop->end + static_cast<uint32_t>(length);
            }
        }
    }

    out->pcm.channels.push_back(std::move(samples));

    return out;
}

LibultraPerformance PerformLibultra(const LibultraSong& song, const n64::BankFile& file, const n64::Bank& bank,
                                    const LibultraSettings& settings, const PerformOptions& options)
{
    Performer performer(song, file, bank, settings, options);
    return performer.Run();
}

std::vector<Zone> LibultraZones(const n64::BankFile& file, int32_t index, const LibultraSettings& settings,
                                std::vector<int32_t>& wave_of_zone)
{
    const n64::BankInstrument& instrument = file.instruments[static_cast<std::size_t>(index)];
    const bool ambrella = settings.player == LibultraPlayer::kAmbrella;
    const double full_voice = ambrella ? kAmbrellaFullVoiceVolume : kFullVoiceVolume;
    const double volume = full_voice * settings.volume / kFullPlayerVolume;

    std::vector<Zone> zones;
    wave_of_zone.clear();

    // Adds the zone of a run over `low_key` to `high_key`.
    const auto add = [&](int low_key, int high_key, const VelocityRun& run)
    {
        const n64::BankSound& sound = file.sounds[static_cast<std::size_t>(run.sound)];
        if (sound.wave == n64::kNoObject || sound.key_map == n64::kNoObject || sound.envelope == n64::kNoObject)
        {
            return;
        }

        const n64::BankKeyMap& map = file.key_maps[static_cast<std::size_t>(sound.key_map)];
        const double sample_volume = settings.player == LibultraPlayer::kHal ? 1.0 : sound.volume / 127.0;
        const double before_envelope = (volume * sample_volume) * (volume * sample_volume);

        Zone zone;
        zone.low_velocity = run.low;
        zone.high_velocity = run.high;
        zone.root_key = map.key_base;
        const int detune = ambrella ? -map.detune : map.detune;
        zone.tune_cents = detune;
        zone.pan = std::clamp((sound.pan - 64) / 64.0, -1.0, 1.0);
        n64::BankEnvelope envelope = file.envelopes[static_cast<std::size_t>(sound.envelope)];
        if (ambrella)
        {
            // The player starts a note at its top and only acts on a fall to nothing.
            const bool falls = envelope.decay_time != 0 && envelope.decay_volume == 0;
            envelope.attack_time = 0;
            envelope.attack_volume = 127;
            envelope.decay_time = falls ? envelope.decay_time : 0;
            envelope.decay_volume = falls ? 0 : 127;
            ApplyAmbrellaWheel(zone, instrument);
        }

        ApplyEnvelope(zone, envelope, before_envelope, settings.line_ramps);
        if (settings.oscillators)
        {
            ApplyOscillators(zone, instrument);
        }

        // Keys past the resampler's limit play at the limit.
        const int limit_key = (map.key_base * 100 + kOctaveCents - detune + 99) / 100;
        if (low_key < limit_key)
        {
            zone.low_key = low_key;
            zone.high_key = std::min(high_key, limit_key - 1);
            zones.push_back(zone);
            wave_of_zone.push_back(sound.wave);
        }

        if (high_key >= limit_key)
        {
            zone.low_key = std::max(low_key, limit_key);
            zone.high_key = high_key;
            zone.follows_key = false;
            zone.tune_cents = kOctaveCents;
            zones.push_back(zone);
            wave_of_zone.push_back(sound.wave);
        }
    };

    int low_key = 0;
    std::vector<VelocityRun> runs = RunsOfKey(file, instrument, 0);
    for (int key = 1; key <= 128; key++)
    {
        std::vector<VelocityRun> next = key <= 127 ? RunsOfKey(file, instrument, key) : std::vector<VelocityRun>();
        if (key == 128 || next != runs)
        {
            for (const VelocityRun& run : runs)
            {
                add(low_key, key - 1, run);
            }

            low_key = key;
            runs = std::move(next);
        }
    }

    return zones;
}

std::unique_ptr<LibultraGame> LibultraGame::Open(std::span<const uint8_t> image, const LibultraProfile* profile)
{
    std::unique_ptr<LibultraGame> game(new LibultraGame());
    game->image_ = image;
    if (profile != nullptr)
    {
        game->profile_ = *profile;
    }
    else
    {
        // Without a profile, the largest sequence file and the bank with the most instruments.
        const n64::FoundFiles found = n64::FindFiles(image);
        std::size_t best_songs = 0;
        for (const std::size_t offset : found.sequence_banks)
        {
            const n64::SeqFile file = n64::SeqFile::Parse(image.subspan(offset));
            std::size_t songs = 0;
            for (std::size_t i = 0; i < file.Entries().size(); i++)
            {
                const std::span<const uint8_t> bytes = file.Sequence(i);
                songs += n64::CompressedMidi::Matches(bytes) || n64::StandardMidi::Matches(bytes) ? 1 : 0;
            }

            if (songs > best_songs)
            {
                best_songs = songs;
                game->profile_.sequences = offset;
            }
        }

        std::size_t best_instruments = 0;
        for (const std::size_t offset : found.bank_files)
        {
            const n64::BankFile file = n64::BankFile::Parse(image.subspan(offset));
            if (file.instruments.size() > best_instruments)
            {
                best_instruments = file.instruments.size();
                game->profile_.bank = offset;
            }
        }

        if (best_songs == 0 || best_instruments == 0)
        {
            return nullptr;
        }
    }

    if (game->profile_.midi_files != 0)
    {
        // A file ends where its only track ends.
        std::size_t place = game->profile_.sequences;
        for (std::size_t i = 0; i < game->profile_.midi_files; i++)
        {
            if (place + kMidiHeaderSize + kMidiTrackHeaderSize > image.size())
            {
                throw xsflib::FormatError("the game's MIDI files run past the image");
            }

            const std::size_t size =
                kMidiHeaderSize + kMidiTrackHeaderSize + xsflib::LoadBe32(image.data() + place + kMidiHeaderSize + 4);
            const std::span<const uint8_t> bytes = image.subspan(place, size);
            game->songs_.emplace_back(bytes.begin(), bytes.end());
            place += size;
        }
    }
    else if (game->profile_.rare_sequences)
    {
        const std::optional<std::size_t> place = n64::FindRareSequences(image);
        if (!place)
        {
            throw xsflib::FormatError("the game's table of sequences can't be found");
        }

        const n64::RareSequenceTable table = n64::RareSequenceTable::Parse(image.subspan(*place));
        for (std::size_t i = 0; i < table.entries.size(); i++)
        {
            game->songs_.push_back(table.Sequence(i));
        }
    }
    else
    {
        const n64::SeqFile sequences = n64::SeqFile::Parse(image.subspan(game->profile_.sequences));
        for (const n64::SeqFile::Entry& entry : sequences.Entries())
        {
            const std::span<const uint8_t> bytes = image.subspan(game->profile_.sequences + entry.offset, entry.length);
            game->songs_.emplace_back(bytes.begin(), bytes.end());
        }
    }

    game->file_ = n64::BankFile::Parse(image.subspan(game->profile_.bank));
    if (game->profile_.program_bend_range != 0)
    {
        for (const n64::Bank& bank : game->file_.banks)
        {
            for (const int32_t object : bank.instruments)
            {
                if (object != n64::kNoObject)
                {
                    game->file_.instruments[static_cast<std::size_t>(object)].bend_range =
                        static_cast<int16_t>(game->profile_.program_bend_range);
                }
            }
        }
    }

    const std::optional<std::size_t> samples = n64::FindBankSamples(image, game->file_);
    if (!samples)
    {
        throw xsflib::FormatError("the place of the bank's samples can't be found");
    }

    game->samples_ = *samples;

    return game;
}

std::vector<ntdlib::sequence_export::Sequence> LibultraGame::Sequences() const
{
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (std::size_t i = 0; i < songs_.size(); i++)
    {
        sequences.push_back({i, SequenceName(i, profile_.names)});
    }

    std::size_t index = songs_.size();
    for (const LibultraRateChange& change : profile_.rate_changes)
    {
        sequences.push_back({index++, SequenceName(change.song, profile_.names, change.beside)});
    }

    return sequences;
}

std::shared_ptr<const Wave> LibultraGame::WaveAt(int32_t index)
{
    const auto found = waves_.find(index);
    if (found != waves_.end())
    {
        return found->second;
    }

    const n64::BankWave& wave = file_.waves[static_cast<std::size_t>(index)];
    const n64::BankLoop* loop =
        wave.loop == n64::kNoObject ? nullptr : &file_.loops[static_cast<std::size_t>(wave.loop)];
    const n64::VadpcmBook* book =
        wave.type == n64::kWaveVadpcm ? &file_.books.at(static_cast<std::size_t>(wave.book)).book : nullptr;
    std::shared_ptr<const Wave> out = DecodeWave(image_.subspan(samples_ + wave.base, wave.length), book, loop,
                                                 HexName(wave.base), profile_.output_rate);
    waves_[index] = out;

    return out;
}

Conversion LibultraGame::Convert(std::size_t index, const PerformOptions& options)
{
    // A rate change's track comes after the songs.
    const LibultraRateChange* change = index < songs_.size() ? nullptr : &profile_.rate_changes[index - songs_.size()];
    const std::size_t song_index = change != nullptr ? change->song : index;
    const LibultraSong song = ReadSong(songs_.at(song_index));

    LibultraSettings settings;
    settings.player = profile_.player;
    settings.line_ramps = profile_.line_ramps;
    settings.volume = SongVolume(image_, profile_, song_index);
    settings.oscillators = profile_.oscillators;
    settings.effect = profile_.effect;
    if (change != nullptr)
    {
        settings.rate = change->rate;
    }

    LibultraPerformance played = PerformLibultra(song, file_, file_.banks.at(0), settings, options);
    for (const int32_t instrument : played.instruments)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "i%d", instrument);

        Instrument preset;
        preset.name = name;
        std::vector<int32_t> waves;
        preset.zones = LibultraZones(file_, instrument, settings, waves);
        for (std::size_t i = 0; i < preset.zones.size(); i++)
        {
            preset.zones[i].wave = WaveAt(waves[i]);
        }

        played.performance.presets.push_back(std::move(preset));
    }

    const std::string name = SequenceName(song_index, profile_.names, change != nullptr ? change->beside : nullptr);
    return BuildFiles(std::move(played.performance), name, options.program);
}

} // namespace ntdlib::sequence_export::n64

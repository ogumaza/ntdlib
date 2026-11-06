// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/sbn.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <tuple>

#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/vadpcm.h"
#include "ntdlib/sequence_export/n64/envelope.h"
#include "ntdlib/sequence_export/n64/libultra.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

namespace n64 = ntdlib::n64;
namespace midi = seqlib::midi;

// The clock.
constexpr uint32_t kFrameSamples = 184;
constexpr uint32_t kNominalRate = 32000;
constexpr uint32_t kMilliFramesPerMinute = 1000 * 60 * kNominalRate / kFrameSamples;
constexpr uint32_t kShortestInterval = 80000;
constexpr uint32_t kLongestInterval = 500000;
constexpr uint32_t kStepsPerBeatsPerMinute = 1000;
constexpr int kDefaultBeatsPerMinute = 156;

// The voices.
constexpr std::size_t kVoiceCount = n64::kBgmTrackCount * 4 + 1;
constexpr std::array<int, 8> kVoicesOfPolyphony = {0, 1, 0, 0, 0, 2, 3, 4};
constexpr int kFirstSharedPolyphony = 5;
constexpr int kFirstSongDrum = 72;
constexpr std::size_t kSongInstruments = 128;

// The levels. The master volume and an instrument's volume are numbers with 24 bits below the point.
constexpr int kFractionBits = 24;
constexpr int kFullLevel = 127;
constexpr int64_t kFullVolume = int64_t{kFullLevel} << kFractionBits;
constexpr int64_t kFirstInstrumentVolume = int64_t{0x7FFF} << 16;
constexpr double kEnvelopeScale = 128.0;
constexpr double kVelocityScale = 128.0;

// A voice's volume at the top of every factor but the envelope, on a scale of 32768. The engine multiplies the master
// and instrument volumes and the proximity's volume in 10 bits each, then the track's volume and the velocity.
constexpr int kFullFactors = (((kFullLevel << 3) * (kFullLevel << 3) * 1023) >> 20) * kFullLevel * 128 >> 9;
constexpr double kFullVoice = kFullFactors / 32768.0;

// Pitch.
constexpr double kHighestRatio = 1.99996;
constexpr int kCentsPerKey = 100;

// The commands of a track's special command.
// The game's code waits a few frames after it starts a song before it sets the levels of the song's tracks. A song's
// first commands would undo them. Each level then takes this many ticks to reach.
constexpr double kTrackLevelsSeconds = 0.2;
constexpr int kTrackLevelsFadeTicks = 48;
constexpr std::size_t kTrackLevelsBytes = 32;

// The level that the game's code at 0x80056044 gives a mix's tracks at the near state.
constexpr int kNearLevel = 0x57;

// The compositions that a song can hold.
constexpr int kCompositions = 4;

constexpr uint8_t kSpecialStereoDelay = 1;
constexpr uint8_t kSpecialSeekCustom = 2;
constexpr uint8_t kSpecialWriteCustom = 3;
constexpr uint8_t kSpecialUseCustom = 4;
constexpr uint8_t kSpecialSound = 5;
constexpr uint8_t kSpecialProximity = 6;

constexpr std::size_t kCustomEnvelopes = 8;
constexpr std::size_t kCustomSteps = 9;
constexpr uint16_t kCustomEmpty = 0xFF00;

constexpr uint8_t kFirstEnvelopeCommand = 0x80;
constexpr uint8_t kRelativeLevel = 0x80;
constexpr uint8_t kLevelMask = 0x7F;

constexpr int kCentrePan = 64;
constexpr int kHighestPan = 127;
constexpr int kLoopDepth = 4;
constexpr int kLoopLabels = 32;
constexpr int kMostPassesInATick = 100;
constexpr double kNoEnd = std::numeric_limits<double>::infinity();

// A tremolo's phase goes round in 256 steps, and its peak comes just short of its depth.
constexpr double kTremoloPeriod = 256.0;
constexpr double kTremoloPeak = 255.0 / 256.0;

int WordAt(const n64::BgmCommand& command, std::size_t at)
{
    return command.args[at] << 8 | command.args[at + 1];
}

// The share of the dry level that a reverb's send leaves, on the sin/cos law.
double DryShare(int reverb)
{
    return std::cos(std::numbers::pi / 2.0 * reverb / kFullLevel);
}

double PanOf(int pan)
{
    return pan >= kCentrePan ? static_cast<double>(pan - kCentrePan) / (kHighestPan - kCentrePan)
                             : static_cast<double>(pan - kCentrePan) / kCentrePan;
}

uint8_t VelocityOf(int velocity)
{
    return static_cast<uint8_t>(std::clamp<long>(std::lround((velocity + 1) * kFullLevel / kVelocityScale), 1, 127));
}

// The drum that a key of a drum track plays, or null. Keys past a song's list play its first drum.
const n64::BgmDrum* DrumOf(const n64::BgmFile& file, std::span<const n64::BgmDrum> per, int key)
{
    if (key < kFirstSongDrum)
    {
        return static_cast<std::size_t>(key) < per.size() ? &per[static_cast<std::size_t>(key)] : nullptr;
    }

    if (file.drums.empty())
    {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(key - kFirstSongDrum);

    return index < file.drums.size() ? &file.drums[index] : &file.drums[0];
}

class Performer
{
public:
    Performer(const n64::BgmFile& file, std::span<const n64::BgmDrum> per, std::span<const n64::BgmInstrument> prg,
              const SbnFacts& facts, const SbnSettings& settings, const PerformOptions& options);

    SbnPerformance Run();

private:
    struct Track
    {
        bool active = false;
        n64::BgmCursor cursor;
        int delay = 0;
        bool drums = false;
        bool muted = false;
        bool started = false; // a note has played
        int polyphony = 0;
        int first_voice = 0;
        int last_voice = 0;
        std::optional<uint16_t> bank_patch;
        int64_t instrument_volume = kFirstInstrumentVolume;
        int fade_ticks = 0;
        int64_t fade_step = 0;
        int64_t fade_target = 0;
        int pan = kCentrePan;
        int random_pan = 0;
        int reverb = 0;
        int volume = kFullLevel;
        int proximity = kFullLevel;
        bool proximity_changed = false; // the game's code has changed the mix, and no branch has read it yet
        int proximity_full = 0;
        int proximity_near = 0;
        int coarse = 0;
        int fine = 0;
        int detune = 0;
        int tremolo_delay = 0;
        int tremolo_rate = 0;
        int tremolo_depth = 0;
        int custom_press = 0;
        midi::TrackOutput output;
    };

    struct Voice
    {
        bool busy = false;
        bool pending = false; // it started in this tick
        int length = 0;
        double free_at = kNoEnd;
        bool sounding = false;
        double release_seconds = 0.0;
        uint32_t note = 0;
        uint8_t midi_track = 0;
        uint8_t key = 0;
    };

    void ReadComposition();
    void EndCompositionLoop(const n64::BgmCompositionCommand& command);
    void LoadPhrase(uint32_t offset);
    bool RunTracks();
    void RunCommand(Track& track, const n64::BgmCommand& command);
    void RunSpecial(Track& track, const n64::BgmCommand& command);
    void FadeInstrument(Track& track, int time, int level);
    void ApplyTrackLevels();
    void UseInstrument(Track& track, int index);
    void StartNote(Track& track, const n64::BgmCommand& command, int& scan);
    void StartInstrumentNote(Track& track, const n64::BgmCommand& command, Voice& voice);
    void StartDrumNote(Track& track, const n64::BgmCommand& command, Voice& voice);
    std::optional<std::size_t> AcquireVoice(Track& track, int& scan);
    void RunVoices(Track& track);
    void CheckDone(Voice& voice);
    void ReleaseVoice(Voice& voice);
    void ResetVoice(Voice& voice);
    void SendLevels(Track& track);
    void SendAllLevels();
    void SetTicksPerBeat(int ticks_per_beat);
    void SetTempo(int64_t tempo);
    void AdvanceClock();
    double TicksPerSecond() const;
    int TremoloCents(const Track& track) const;

    const n64::BgmFile& file_;
    std::span<const n64::BgmDrum> per_;
    std::span<const n64::BgmInstrument> prg_;
    const SbnFacts& facts_;
    const SbnSettings& settings_;
    const PerformOptions& options_;

    midi::Recorder recorder_;
    SbnPerformance out_;
    std::array<Track, n64::kBgmTrackCount> tracks_;
    std::array<Voice, kVoiceCount> voices_;
    std::array<std::array<uint16_t, kCustomSteps>, kCustomEnvelopes> custom_ = {};
    std::array<uint8_t, kCustomEnvelopes> custom_write_ = {};
    int custom_writing_ = 0;

    // The composition.
    uint32_t composition_offset_ = 0;
    std::vector<n64::BgmCompositionCommand> commands_;
    std::size_t position_ = 0;
    std::array<std::size_t, kLoopLabels> labels_ = {};
    std::array<uint32_t, kLoopLabels> label_ticks_ = {};
    std::array<std::optional<std::size_t>, kLoopDepth + 1> loop_ends_ = {};
    std::array<int, kLoopDepth + 1> loop_counts_ = {};
    int loop_depth_ = 0;
    bool link_mutes_left_ = true;
    bool ended_ = false;
    std::optional<std::pair<uint32_t, uint32_t>> loop_;

    // The master settings.
    int64_t master_volume_ = kFullVolume;
    bool track_levels_applied_ = false;
    int master_fade_ticks_ = 0;
    int64_t master_fade_step_ = 0;
    int64_t master_fade_target_ = 0;
    int master_shift_ = 0;
    int64_t tempo_ = int64_t{kDefaultBeatsPerMinute} * 100;
    int tempo_fade_ticks_ = 0;
    int64_t tempo_fade_step_ = 0;
    int64_t tempo_fade_target_ = 0;

    // The clock.
    uint32_t interval_ = 0;
    uint32_t step_ = 0;
    int64_t counter_ = 0;
    uint64_t frames_ = 0;
    uint32_t tick_ = 0;
    double seconds_ = 0.0;
    uint32_t random_ = 0;
};

Performer::Performer(const n64::BgmFile& file, std::span<const n64::BgmDrum> per,
                     std::span<const n64::BgmInstrument> prg, const SbnFacts& facts, const SbnSettings& settings,
                     const PerformOptions& options)
    : file_(file), per_(per), prg_(prg), facts_(facts), settings_(settings), options_(options)
{
    for (auto& steps : custom_)
    {
        steps.fill(kCustomEmpty);
    }

    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        tracks_[i].output.midi_track = static_cast<uint8_t>(i);
        tracks_[i].proximity_changed = settings.mix != 0;
    }

    SetTicksPerBeat(settings.ticks_per_beat);
}

void Performer::SetTicksPerBeat(int ticks_per_beat)
{
    interval_ =
        std::clamp(kMilliFramesPerMinute / static_cast<uint32_t>(ticks_per_beat), kShortestInterval, kLongestInterval);
    counter_ = interval_;
    step_ = static_cast<uint32_t>(tempo_ * kStepsPerBeatsPerMinute / 100);
}

// A tempo holds hundredths of a beat a minute.
void Performer::SetTempo(int64_t tempo)
{
    tempo_ = tempo;
    step_ = static_cast<uint32_t>(tempo * kStepsPerBeatsPerMinute / 100);
    recorder_.SetTempo(out_.performance, tick_, TicksPerSecond() * 60.0 / settings_.ticks_per_beat);
}

double Performer::TicksPerSecond() const
{
    const double frames_per_second = static_cast<double>(settings_.output_rate) / kFrameSamples;

    return std::min(1.0, static_cast<double>(step_) / interval_) * frames_per_second;
}

// Runs frames up to the next tick.
void Performer::AdvanceClock()
{
    do
    {
        frames_++;
        counter_ -= step_;
    } while (counter_ > 0);

    counter_ += interval_;
    seconds_ = static_cast<double>(frames_) * kFrameSamples / settings_.output_rate;

    // The game's random numbers mix a count of the song's frames and a count of the video's. The conversion takes the
    // video's count from the seed and the frames.
    constexpr uint64_t kFramesPerVideoFrame = 3;
    const uint32_t video = options_.seed + static_cast<uint32_t>(frames_ / kFramesPerVideoFrame);
    random_ = (random_ & 0xFFFF) + static_cast<uint32_t>(frames_ & 0xFFFF) + (video & 0xFFFF);
}

void Performer::SendLevels(Track& track)
{
    Performance& performance = out_.performance;
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kVolume,
                          static_cast<uint8_t>(track.volume));

    // The instrument's volume, the master volume and the send's share of a track's level. The controller acts squared.
    const double instrument = static_cast<double>(track.instrument_volume) / kFullVolume;
    const double master = static_cast<double>(master_volume_) / kFullVolume;
    const double dry = track.drums ? 1.0 : std::sqrt(std::max(DryShare(track.reverb), 0.0));
    const double proximity = static_cast<double>(track.proximity) / kFullLevel;
    const long expression = std::lround(kFullLevel * std::min(instrument, 1.0) * master * dry * proximity);
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kExpression,
                          static_cast<uint8_t>(std::clamp<long>(expression, 0, kFullLevel)));
}

void Performer::SendAllLevels()
{
    for (Track& track : tracks_)
    {
        if (track.active && track.started)
        {
            SendLevels(track);
        }
    }
}

void Performer::EndCompositionLoop(const n64::BgmCompositionCommand& command)
{
    const std::size_t label = command.Label();
    const int count = command.LoopCount();
    if (count == 0)
    {
        // The song loops for ever from here. A loop of no length ends the song.
        ended_ = true;
        if (label_ticks_.at(label) < tick_)
        {
            loop_ = {label_ticks_.at(label), tick_};
        }

        return;
    }

    std::optional<std::size_t>& loop_end = loop_ends_.at(static_cast<std::size_t>(loop_depth_));
    int& loop_count = loop_counts_.at(static_cast<std::size_t>(loop_depth_));
    if (!loop_end)
    {
        loop_end = position_;
        loop_count = count;
        position_ = labels_.at(label);
    }
    else if (*loop_end == position_)
    {
        loop_count--;
        if (loop_count != 0)
        {
            position_ = labels_.at(label);
        }
        else
        {
            loop_end.reset();
            loop_depth_ = std::max(loop_depth_ - 1, 0);
        }
    }
    else if (loop_depth_ < kLoopDepth)
    {
        loop_depth_++;
        loop_ends_.at(static_cast<std::size_t>(loop_depth_)) = position_;
        loop_counts_.at(static_cast<std::size_t>(loop_depth_)) = count;
        position_ = labels_.at(label);
    }
}

void Performer::LoadPhrase(uint32_t offset)
{
    const std::array<n64::BgmTrackInfo, n64::kBgmTrackCount> infos = file_.Phrase(offset);
    int next_voice = 0;
    bool found_link = false;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        const n64::BgmTrackInfo& info = infos[i];
        Track& track = tracks_[i];
        track.active = false;
        if (info.offset == 0 || info.disabled)
        {
            continue;
        }

        track.polyphony = info.polyphony;
        track.drums = info.drums;
        if (info.linked_track != 0)
        {
            const std::size_t linked = info.linked_track - std::size_t{1};
            if (linked >= i)
            {
                continue;
            }

            track.first_voice = tracks_[linked].first_voice;
            track.last_voice = tracks_[linked].last_voice;
            if (link_mutes_left_)
            {
                track.muted = !settings_.linked;
                tracks_[linked].muted = settings_.linked;
            }

            found_link = true;
        }
        else
        {
            track.first_voice = next_voice;
            next_voice += kVoicesOfPolyphony.at(track.polyphony);
            track.last_voice = next_voice;
        }

        track.active = true;
        track.delay = 1;
        track.cursor = {};
        track.cursor.position = info.offset;
    }

    if (found_link)
    {
        link_mutes_left_ = false;
    }
}

void Performer::ReadComposition()
{
    // A new phrase ends a tempo's fade.
    tempo_fade_ticks_ = 0;
    while (!ended_)
    {
        if (position_ >= commands_.size())
        {
            ended_ = true;
            return;
        }

        const n64::BgmCompositionCommand& command = commands_[position_];
        position_++;
        switch (command.Op())
        {
        case n64::BgmCompositionOp::kEnd:
            ended_ = true;
            return;
        case n64::BgmCompositionOp::kPlayPhrase:
            LoadPhrase(command.PhraseOffset(composition_offset_));
            return;
        case n64::BgmCompositionOp::kStartLoop:
            label_ticks_.at(command.Label()) = tick_;
            break;
        case n64::BgmCompositionOp::kEndLoop:
        case n64::BgmCompositionOp::kEndLoopUnless:
            // The game's flag for a conditional loop stays off.
            EndCompositionLoop(command);
            break;
        default:
            break;
        }
    }
}

void Performer::CheckDone(Voice& voice)
{
    if (voice.busy && seconds_ >= voice.free_at)
    {
        ResetVoice(voice);
    }
}

void Performer::ReleaseVoice(Voice& voice)
{
    if (voice.sounding)
    {
        recorder_.NoteOff(out_.performance, voice.midi_track, tick_, 0, voice.key, voice.note);
        voice.sounding = false;
    }

    voice.free_at = std::min(voice.free_at, seconds_ + voice.release_seconds);
}

void Performer::ResetVoice(Voice& voice)
{
    if (voice.sounding)
    {
        recorder_.NoteOff(out_.performance, voice.midi_track, tick_, 0, voice.key, voice.note);
        voice.sounding = false;
    }

    voice.busy = false;
    voice.pending = false;
}

std::optional<std::size_t> Performer::AcquireVoice(Track& track, int& scan)
{
    for (int index = scan; index < track.last_voice; index++)
    {
        scan++;
        Voice& voice = voices_.at(static_cast<std::size_t>(index));
        CheckDone(voice);
        if (!voice.busy)
        {
            return static_cast<std::size_t>(index);
        }
    }

    const std::size_t first = static_cast<std::size_t>(track.first_voice);
    const std::size_t last = static_cast<std::size_t>(track.last_voice);
    if (track.polyphony < kFirstSharedPolyphony)
    {
        ResetVoice(voices_.at(first));

        return first;
    }

    for (std::size_t index = first; index < last; index++)
    {
        if (voices_[index].busy && voices_[index].length == 0)
        {
            ResetVoice(voices_[index]);

            return index;
        }
    }

    std::optional<std::size_t> shortest;
    int least = std::numeric_limits<uint16_t>::max();
    for (std::size_t index = first; index < last; index++)
    {
        if (voices_[index].busy && !voices_[index].pending && voices_[index].length < least)
        {
            least = voices_[index].length;
            shortest = index;
        }
    }

    if (shortest)
    {
        ResetVoice(voices_[*shortest]);
    }

    return shortest;
}

int Performer::TremoloCents(const Track& track) const
{
    return static_cast<int>(std::lround(track.tremolo_depth * kTremoloPeak));
}

void Performer::StartInstrumentNote(Track& track, const n64::BgmCommand& command, Voice& voice)
{
    SbnInstrumentFacts facts;
    if (track.bank_patch)
    {
        facts = facts_(*track.bank_patch);
    }

    voice.busy = true;
    voice.pending = true;
    voice.free_at = seconds_;
    if (!facts.exists)
    {
        if (track.bank_patch)
        {
            recorder_.Approximate("a note on an instrument that the game's banks don't hold");
        }
        else
        {
            recorder_.Approximate("a note on a track with no instrument");
        }

        return;
    }

    voice.release_seconds = facts.release_seconds;

    // The key takes the track's whole semitones, and the preset takes its cents.
    const int cents = command.key * kCentsPerKey + track.coarse + master_shift_ + track.fine - facts.key_base;
    if (facts.loops)
    {
        voice.free_at = kNoEnd;
    }
    else if (facts.sample_rate != 0)
    {
        const double ratio =
            std::min(std::pow(2.0, (cents + track.detune) / 1200.0) * facts.sample_rate / kNominalRate, kHighestRatio);
        voice.free_at = seconds_ + facts.samples / (ratio * settings_.output_rate);
    }

    if (command.velocity == 0)
    {
        return;
    }

    SbnPresetKey key;
    key.bank_patch = *track.bank_patch;
    key.fine_cents = track.fine;
    key.cuts = track.polyphony < kFirstSharedPolyphony;
    if (track.custom_press != 0)
    {
        for (const uint16_t step : custom_.at(static_cast<std::size_t>(track.custom_press - 1)))
        {
            key.press.push_back(static_cast<uint8_t>(step >> 8));
            key.press.push_back(static_cast<uint8_t>(step));
        }
    }

    if (track.tremolo_depth != 0 && track.tremolo_rate != 0)
    {
        constexpr double kHzSteps = 16.0;
        constexpr double kDelayStepMs = 10.0;
        const double ticks = TicksPerSecond();
        key.tremolo_depth = TremoloCents(track);
        key.tremolo_hz_sixteenths =
            static_cast<int>(std::lround(ticks * track.tremolo_rate / kTremoloPeriod * kHzSteps));
        key.tremolo_delay_ms =
            static_cast<int>(std::lround(track.tremolo_delay * 1000.0 / ticks / kDelayStepMs) * kDelayStepMs);
    }

    int pan = track.pan;
    if (track.random_pan != 0)
    {
        // The engine's tap of its random number decides the side, and other bits decide the distance.
        const int value = static_cast<int>(random_ >> 8 & 0x3F) + static_cast<int>(random_ << 4 & 0xC0);
        const int shift = track.random_pan * value >> 8;
        const bool right = (((random_ >> 7) + (random_ >> 2)) & 1) != 0;
        pan = std::clamp(right ? pan + shift : pan - shift, 0, kHighestPan);
    }

    Performance& performance = out_.performance;
    track.started = true;
    SendLevels(track);
    recorder_.SendPreset(performance, track.output, tick_, static_cast<uint32_t>(midi::PresetIndex(out_.presets, key)));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kPan, static_cast<uint8_t>(pan));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kReverb,
                          static_cast<uint8_t>(track.reverb));
    recorder_.SendPitch(performance, track.output, tick_, static_cast<float>(track.detune) / kCentsPerKey);
    const int shifted = command.key + (track.coarse + master_shift_) / kCentsPerKey;
    voice.key = static_cast<uint8_t>(std::clamp(shifted, 0, 127));
    voice.midi_track = track.output.midi_track;
    voice.note = recorder_.NoteOn(performance, track.output, tick_, voice.key, VelocityOf(command.velocity));
    voice.sounding = true;
}

void Performer::StartDrumNote(Track& track, const n64::BgmCommand& command, Voice& voice)
{
    const int drum_key = command.key;
    const n64::BgmDrum* drum = DrumOf(file_, per_, drum_key);
    voice.busy = true;
    voice.pending = true;
    voice.free_at = seconds_;
    if (drum == nullptr)
    {
        return;
    }

    const SbnInstrumentFacts facts = facts_(drum->bank_patch);
    if (!facts.exists)
    {
        recorder_.Approximate("a drum on an instrument that the game's banks don't hold");
        return;
    }

    voice.release_seconds = facts.release_seconds;
    int tune = track.detune;
    if (drum->random_tune != 0)
    {
        const int value = static_cast<int>(random_ >> 6 & 0xF) + static_cast<int>(random_ << 2 & 0xF0);
        const int shift = drum->random_tune * 5 * value >> 8;
        tune += (((random_ >> 4) + (random_ >> 1)) & 1) != 0 ? shift : -shift;
    }

    if (facts.loops)
    {
        voice.free_at = kNoEnd;
    }
    else if (facts.sample_rate != 0)
    {
        const int cents = drum->key_base + track.coarse + track.fine - facts.key_base + tune;
        const double ratio = std::min(std::pow(2.0, cents / 1200.0) * facts.sample_rate / kNominalRate, kHighestRatio);
        voice.free_at = seconds_ + facts.samples / (ratio * settings_.output_rate);
    }

    if (command.velocity == 0)
    {
        return;
    }

    int pan_shift = 0;
    if (drum->random_pan != 0)
    {
        const int value = static_cast<int>(random_ >> 8 & 0x3F) + static_cast<int>(random_ << 4 & 0xC0);
        const int shift = drum->random_pan * value >> 8;
        pan_shift = ((random_ >> 7) + (random_ >> 2) & 1) != 0 ? shift : -shift;
    }

    if (drum->random_volume != 0 || drum->random_reverb != 0)
    {
        recorder_.Approximate("a drum's random volume or reverb");
    }

    SbnPresetKey key;
    key.drums = true;
    key.fine_cents = track.coarse + track.fine;
    key.cuts = track.polyphony < kFirstSharedPolyphony;
    out_.drum_keys.insert(drum_key);

    Performance& performance = out_.performance;
    track.started = true;
    SendLevels(track);
    recorder_.SendPreset(performance, track.output, tick_, static_cast<uint32_t>(midi::PresetIndex(out_.presets, key)));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kPan,
                          static_cast<uint8_t>(std::clamp(kCentrePan + pan_shift, 0, kHighestPan)));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kReverb, drum->reverb);
    recorder_.SendPitch(performance, track.output, tick_, static_cast<float>(tune) / kCentsPerKey);
    voice.key = static_cast<uint8_t>(drum_key);
    voice.midi_track = track.output.midi_track;
    voice.note = recorder_.NoteOn(performance, track.output, tick_, voice.key, VelocityOf(command.velocity));
    voice.sounding = true;
}

void Performer::StartNote(Track& track, const n64::BgmCommand& command, int& scan)
{
    if (track.muted)
    {
        return;
    }

    const std::optional<std::size_t> index = AcquireVoice(track, scan);
    if (!index)
    {
        return;
    }

    Voice& voice = voices_.at(*index);
    voice.length = command.length;

    // A note this short starts no voice. It has still cut the one before.
    if (command.length <= 1)
    {
        return;
    }

    if (track.drums)
    {
        StartDrumNote(track, command, voice);
    }
    else
    {
        StartInstrumentNote(track, command, voice);
    }
}

// The command 0xF5. It sets the track's levels, pan and tuning from an instrument, and keeps the track's random pan.
void Performer::UseInstrument(Track& track, int index)
{
    const n64::BgmInstrument* instrument = nullptr;
    const auto number = static_cast<std::size_t>(index);
    if (number < kSongInstruments)
    {
        instrument = number < file_.instruments.size() ? &file_.instruments[number] : nullptr;
    }
    else if (number - kSongInstruments < prg_.size())
    {
        instrument = &prg_[number - kSongInstruments];
    }

    if (instrument == nullptr)
    {
        track.bank_patch.reset();
        return;
    }

    track.bank_patch = instrument->bank_patch;
    track.instrument_volume = int64_t{instrument->volume & kLevelMask} << kFractionBits;
    track.pan = static_cast<uint8_t>(instrument->pan) & kLevelMask;
    track.reverb = instrument->reverb & kLevelMask;
    track.coarse = instrument->coarse_tune * kCentsPerKey;
    track.fine = instrument->fine_tune;
}

void Performer::RunSpecial(Track& track, const n64::BgmCommand& command)
{
    const uint8_t type = command.args[0];
    const uint8_t first = command.args[1];
    const uint8_t second = command.args[2];
    switch (type)
    {
    case kSpecialStereoDelay:
        recorder_.Approximate("a song's delay of one stereo side");
        break;
    case kSpecialSeekCustom:
        custom_writing_ = first;
        if (first != 0 && first <= kCustomEnvelopes)
        {
            custom_.at(first - std::size_t{1}).fill(kCustomEmpty);
            custom_write_.at(first - std::size_t{1}) = 0;
        }
        else
        {
            custom_writing_ = 0;
        }

        break;
    case kSpecialWriteCustom:
        if (custom_writing_ != 0)
        {
            const auto index = static_cast<std::size_t>(custom_writing_ - 1);
            if (custom_write_.at(index) + std::size_t{1} < kCustomSteps)
            {
                const uint8_t time = first < settings_.custom_times.size() ? settings_.custom_times[first] : first;
                custom_.at(index).at(custom_write_.at(index)) = static_cast<uint16_t>(time << 8 | second);
                custom_write_.at(index)++;
            }
        }

        break;
    case kSpecialUseCustom:
        track.custom_press = first <= kCustomEnvelopes ? first : 0;
        break;
    case kSpecialSound:
        recorder_.Approximate("a sound effect that a song starts");
        break;
    case kSpecialProximity:
        if (first != 0)
        {
            track.proximity_full = first;
            track.proximity_near = second;
        }
        else if (track.proximity_changed)
        {
            track.proximity_changed = false;
            for (Track& other : tracks_)
            {
                const int level = settings_.proximity == kFullLevel ? other.proximity_full : other.proximity_near;
                if (level != 0)
                {
                    other.proximity_changed = false;
                    other.proximity = level;
                    if (other.active && other.started)
                    {
                        SendLevels(other);
                    }
                }
            }
        }

        break;
    default:
        break;
    }
}

void Performer::FadeInstrument(Track& track, int time, int level)
{
    const int64_t target = int64_t{level & kLevelMask} << kFractionBits;
    if (target != track.instrument_volume)
    {
        const int ticks = std::max(time, 1);
        track.fade_ticks = ticks;
        track.fade_target = target;
        track.fade_step = (target - track.instrument_volume) / ticks;
    }
}

void Performer::ApplyTrackLevels()
{
    const std::span<const uint8_t> list = settings_.track_levels;
    for (std::size_t i = 0; i + 1 < list.size() && list[i] != 0 && list[i] <= tracks_.size(); i += 2)
    {
        Track& track = tracks_[list[i] - std::size_t{1}];
        if (track.active)
        {
            FadeInstrument(track, kTrackLevelsFadeTicks, list[i + 1]);
        }
    }
}

void Performer::RunCommand(Track& track, const n64::BgmCommand& command)
{
    const auto volume_of = [](int value) -> int64_t
    {
        return int64_t{value & kLevelMask} << kFractionBits;
    };

    switch (command.op)
    {
    case n64::BgmOp::kTempo:
        {
            const int64_t limit = interval_ / kStepsPerBeatsPerMinute;
            tempo_fade_ticks_ = 0;
            SetTempo(std::clamp<int64_t>(WordAt(command, 0), 1, limit) * 100);
            break;
        }
    case n64::BgmOp::kMasterVolume:
        // The command ends a running fade of the master volume, as the game's player does.
        master_volume_ = volume_of(command.args[0]);
        master_fade_ticks_ = 0;
        SendAllLevels();
        break;
    case n64::BgmOp::kMasterTranspose:
        master_shift_ = static_cast<int8_t>(command.args[0]) * kCentsPerKey;
        break;
    case n64::BgmOp::kTempoFade:
        {
            const int64_t limit = interval_ / kStepsPerBeatsPerMinute;
            const int time = std::max(WordAt(command, 0), 1);
            tempo_fade_ticks_ = time;
            tempo_fade_target_ = std::clamp<int64_t>(WordAt(command, 2), 1, limit) * 100;
            tempo_fade_step_ = (tempo_fade_target_ - tempo_) / time;
            break;
        }
    case n64::BgmOp::kMasterVolumeFade:
        {
            const int time = std::max(WordAt(command, 0), 1);
            master_fade_ticks_ = time;
            master_fade_target_ = volume_of(command.args[2]);
            master_fade_step_ = (master_fade_target_ - master_volume_) / time;
            break;
        }
    case n64::BgmOp::kPatch:
        track.bank_patch = static_cast<uint16_t>(command.args[0] << 8 | command.args[1]);
        break;
    case n64::BgmOp::kInstrumentVolume:
        track.instrument_volume = volume_of(command.args[0]);
        break;
    case n64::BgmOp::kInstrumentPan:
        track.pan = command.args[0] & kLevelMask;
        track.random_pan = 0;
        if (track.started && !track.drums)
        {
            recorder_.SendSetting(out_.performance, track.output, tick_, midi::EventKind::kPan,
                                  static_cast<uint8_t>(track.pan));
        }

        break;
    case n64::BgmOp::kInstrumentReverb:
        track.reverb = command.args[0] & kLevelMask;
        if (track.started && !track.drums)
        {
            recorder_.SendSetting(out_.performance, track.output, tick_, midi::EventKind::kReverb,
                                  static_cast<uint8_t>(track.reverb));
        }

        break;
    case n64::BgmOp::kTrackVolume:
        track.volume = command.args[0] & kLevelMask;
        break;
    case n64::BgmOp::kCoarseTune:
        track.coarse = static_cast<int8_t>(command.args[0]) * kCentsPerKey;
        break;
    case n64::BgmOp::kFineTune:
        track.fine = static_cast<int8_t>(command.args[0]);
        break;
    case n64::BgmOp::kTrackDetune:
        track.detune = static_cast<int16_t>(WordAt(command, 0));
        if (track.started)
        {
            recorder_.SendPitch(out_.performance, track.output, tick_, static_cast<float>(track.detune) / kCentsPerKey);
        }

        break;
    case n64::BgmOp::kTremolo:
        track.tremolo_delay = command.args[0];
        track.tremolo_rate = command.args[1];
        track.tremolo_depth = command.args[2];
        break;
    case n64::BgmOp::kTremoloRate:
        track.tremolo_rate = command.args[0];
        break;
    case n64::BgmOp::kTremoloDepth:
        track.tremolo_depth = command.args[0];
        break;
    case n64::BgmOp::kTremoloStop:
        track.tremolo_depth = 0;
        break;
    case n64::BgmOp::kRandomPan:
        track.pan = command.args[0] & kLevelMask;
        track.random_pan = command.args[1] & kLevelMask;
        break;
    case n64::BgmOp::kInstrument:
        UseInstrument(track, command.args[0]);
        break;
    case n64::BgmOp::kInstrumentVolumeFade:
        FadeInstrument(track, WordAt(command, 0), command.args[2]);
        break;
    case n64::BgmOp::kBranch:
        {
            const n64::BgmBranchTarget target =
                n64::ReadBgmBranch(file_.Bytes(), command, static_cast<uint8_t>(settings_.mix));
            track.cursor.branch_end = track.cursor.position;
            track.cursor.position = target.position;
            track.drums = target.drums;
            track.coarse = 0;
            track.fine = 0;
            track.custom_press = 0;
            track.detune = 0;
            track.tremolo_depth = 0;
            track.fade_ticks = 0;
            track.random_pan = 0;
            if (track.proximity_changed)
            {
                track.proximity_changed = false;
                track.proximity = settings_.proximity;
            }

            break;
        }
    case n64::BgmOp::kSpecial:
        RunSpecial(track, command);
        break;
    default:
        break;
    }

    if (track.started)
    {
        SendLevels(track);
    }
}

void Performer::RunVoices(Track& track)
{
    if (track.muted)
    {
        return;
    }

    for (int index = track.first_voice; index < track.last_voice; index++)
    {
        Voice& voice = voices_.at(static_cast<std::size_t>(index));
        CheckDone(voice);
        if (!voice.busy)
        {
            continue;
        }

        if (!voice.pending && voice.length > 0)
        {
            voice.length--;
            if (voice.length == 0)
            {
                ReleaseVoice(voice);
            }
        }

        voice.pending = false;
    }
}

bool Performer::RunTracks()
{
    if (!track_levels_applied_ && seconds_ >= kTrackLevelsSeconds)
    {
        track_levels_applied_ = true;
        ApplyTrackLevels();
    }

    if (tempo_fade_ticks_ != 0)
    {
        tempo_fade_ticks_--;
        tempo_ = tempo_fade_ticks_ == 0 ? tempo_fade_target_ : tempo_ + tempo_fade_step_;
        SetTempo(tempo_);
    }

    if (master_fade_ticks_ != 0)
    {
        master_fade_ticks_--;
        master_volume_ = master_fade_ticks_ == 0 ? master_fade_target_ : master_volume_ + master_fade_step_;
        SendAllLevels();
    }

    bool finished = false;
    for (Track& track : tracks_)
    {
        if (!track.active)
        {
            continue;
        }

        if (track.fade_ticks != 0)
        {
            track.fade_ticks--;
            track.instrument_volume =
                track.fade_ticks == 0 ? track.fade_target : track.instrument_volume + track.fade_step;
            if (track.started)
            {
                SendLevels(track);
            }
        }

        track.delay--;
        if (track.delay <= 0)
        {
            int scan = track.first_voice;
            while (track.delay == 0)
            {
                const n64::BgmCommand command = n64::ReadBgmCommand(file_.Bytes(), track.cursor);
                if (command.op == n64::BgmOp::kEnd)
                {
                    finished = true;
                    break;
                }

                if (command.op == n64::BgmOp::kWait)
                {
                    track.delay = command.length;
                }
                else if (command.op == n64::BgmOp::kNote)
                {
                    StartNote(track, command, scan);
                }
                else if (command.op != n64::BgmOp::kReturn)
                {
                    RunCommand(track, command);
                }
            }
        }

        RunVoices(track);
    }

    return finished;
}

SbnPerformance Performer::Run()
{
    Performance& performance = out_.performance;
    performance.timebase = settings_.ticks_per_beat;
    SetTempo(tempo_);

    composition_offset_ = file_.CompositionOffset(settings_.variation);
    commands_ = file_.Composition(composition_offset_);
    for (std::size_t i = 0; i < commands_.size(); i++)
    {
        if (commands_[i].Op() == n64::BgmCompositionOp::kStartLoop)
        {
            labels_.at(commands_[i].Label()) = i + 1;
        }
    }

    bool fetch = true;
    bool cut = false;
    while (!ended_)
    {
        AdvanceClock();
        int passes = 0;
        do
        {
            if (fetch)
            {
                ReadComposition();
            }

            if (ended_)
            {
                break;
            }

            fetch = RunTracks();
            passes++;
        } while (fetch && passes < kMostPassesInATick);

        if (ended_)
        {
            break;
        }

        tick_++;
        if (seconds_ > kLongestSeconds)
        {
            recorder_.Approximate("a song cut at " + std::to_string(static_cast<int>(kLongestSeconds)) + " seconds");
            cut = true;
            break;
        }
    }

    for (Voice& voice : voices_)
    {
        ResetVoice(voice);
    }

    const uint32_t end_tick = tick_;
    if (loop_ && !cut)
    {
        midi::ClampNoteOffs(performance, end_tick);
        midi::CutAt(performance, end_tick);
        performance.loop = midi::Performance::Loop{loop_->first, end_tick};
        performance.loops = options_.loops;
    }

    performance.end_tick = end_tick;
    performance.seconds = seconds_;
    performance.approximations = recorder_.Approximations();
    performance.warnings = recorder_.Warnings();

    return std::move(out_);
}

double TimeOf(std::span<const int32_t> times, uint8_t index)
{
    return index < times.size() ? times[index] : 0.0;
}

// An envelope's press list as a shape in microseconds. No list of the game's holds a command. A command ends a list.
EnvelopeShape PressShape(std::span<const uint8_t> press, std::span<const int32_t> times)
{
    EnvelopeShape shape;
    shape.points.push_back({0.0, 0.0});
    double time = 0.0;
    for (std::size_t at = 0; at + 1 < press.size() && press[at] < kFirstEnvelopeCommand; at += 2)
    {
        const double level = (press[at + 1] & kLevelMask) / kEnvelopeScale;
        time += TimeOf(times, press[at]);
        shape.points.push_back({time, level * level});
    }

    return shape;
}

// The energy of a release list from a level of 1, in microseconds at that level. `from` is the amplitude that the
// release starts from, for a list whose levels aren't relative to it.
double ReleaseEnergy(std::span<const uint8_t> release, std::span<const int32_t> times, double from)
{
    bool relative = release.size() > 1 && (release[1] & kRelativeLevel) != 0;
    double before = 1.0;
    double energy = 0.0;
    for (std::size_t at = 0; at + 1 < release.size() && release[at] < kFirstEnvelopeCommand; at += 2)
    {
        const double level = (release[at + 1] & kLevelMask) / kEnvelopeScale;
        const double next = relative ? level * level : level * level / std::max(from, 1e-6);
        energy += TimeOf(times, release[at]) * (before * before + before * next + next * next) / 3.0;
        before = next;
    }

    return energy;
}

// What the game's code can change in a composition: the values that its branches read, and its linked tracks.
std::pair<int, bool> ChoicesOf(const n64::BgmFile& file, uint32_t composition)
{
    int mixes = 1;
    bool links = false;
    for (const n64::BgmCompositionCommand& command : file.Composition(composition))
    {
        if (command.Op() != n64::BgmCompositionOp::kPlayPhrase)
        {
            continue;
        }

        for (const n64::BgmTrackInfo& info : file.Phrase(command.PhraseOffset(composition)))
        {
            if (info.offset == 0 || info.disabled)
            {
                continue;
            }

            links = links || info.linked_track != 0;
            n64::BgmCursor cursor;
            cursor.position = info.offset;
            n64::BgmCommand read = n64::ReadBgmCommand(file.Bytes(), cursor);
            while (read.op != n64::BgmOp::kEnd)
            {
                if (read.op == n64::BgmOp::kBranch)
                {
                    mixes = std::max<int>(mixes, read.args[2]);
                    const n64::BgmBranchTarget target = n64::ReadBgmBranch(file.Bytes(), read, 0);
                    cursor.branch_end = cursor.position;
                    cursor.position = target.position;
                }

                read = n64::ReadBgmCommand(file.Bytes(), cursor);
            }
        }
    }

    return {mixes, links};
}

// A zone's envelope and level.
void FitSbnEnvelope(Zone& zone, const SbnSound& sound, std::span<const uint8_t> custom_press,
                    std::span<const int32_t> times, uint32_t output_rate)
{
    // The table's times count frames at the nominal rate.
    const double updates_per_second = 1e6 * output_rate / kNominalRate;
    const EnvelopeShape shape = PressShape(custom_press.empty() ? sound.press : custom_press, times);
    const double level = FitEnvelope(zone, shape, updates_per_second, 0.0);
    const double held = shape.points.back().level > 0.0 ? shape.points.back().level : level;

    // SoundFont's release falls 100 dB in a straight line of dB. A fall that takes T seconds holds T / (10 ln 10) of
    // energy at a level of 1. The zone's release holds the energy of the game's list.
    constexpr double kEnergyTo100Db = 10.0 * std::numbers::ln10;
    zone.release_ms = kEnergyTo100Db * ReleaseEnergy(sound.release, times, held) * 1000.0 / updates_per_second;
    zone.gain = level * kFullVoice * kFullVoice;
}

} // namespace

std::vector<int> SbnVariations(const n64::BgmFile& file)
{
    std::vector<int> variations;
    std::vector<std::vector<uint32_t>> seen;
    for (int variation = 0; variation < kCompositions; variation++)
    {
        const uint32_t composition = file.CompositionOffset(variation);
        if (composition == 0)
        {
            continue;
        }

        // A phrase's place in the file, which two compositions can share, and each other command as it stands.
        std::vector<uint32_t> list;
        bool plays = false;
        for (const n64::BgmCompositionCommand& command : file.Composition(composition))
        {
            const bool phrase = command.Op() == n64::BgmCompositionOp::kPlayPhrase;
            plays = plays || phrase;
            list.push_back(phrase ? command.PhraseOffset(composition) : command.raw);
        }

        if (plays && std::ranges::find(seen, list) == seen.end())
        {
            seen.push_back(std::move(list));
            variations.push_back(variation);
        }
    }

    return variations;
}

SbnPerformance PerformSbn(const n64::BgmFile& file, std::span<const n64::BgmDrum> per,
                          std::span<const n64::BgmInstrument> prg, const SbnFacts& facts, const SbnSettings& settings,
                          const PerformOptions& options)
{
    Performer performer(file, per, prg, facts, settings, options);

    return performer.Run();
}

double SbnReleaseSeconds(std::span<const uint8_t> release, std::span<const int32_t> times, uint32_t output_rate)
{
    double micros = 0.0;
    for (std::size_t at = 0; at + 1 < release.size() && release[at] < kFirstEnvelopeCommand; at += 2)
    {
        micros += TimeOf(times, release[at]);
    }

    return micros / 1e6 * kNominalRate / output_rate;
}

std::vector<Zone> SbnInstrumentZones(const SbnSound& sound, const SbnPresetKey& key, std::span<const int32_t> times,
                                     uint32_t output_rate)
{
    Zone zone;
    zone.exclusive_class = key.cuts ? 1 : 0;

    // A key's pitch stands 100 * key + base cents above the output's rate.
    const double base =
        1200.0 * std::log2(static_cast<double>(sound.sample_rate) / kNominalRate) - sound.key_base + key.fine_cents;
    zone.root_key = static_cast<int>(std::clamp(std::lround(-base / kCentsPerKey), 0L, 127L));
    zone.tune_cents = base + kCentsPerKey * zone.root_key;
    FitSbnEnvelope(zone, sound, key.press, times, output_rate);
    if (key.tremolo_depth != 0)
    {
        constexpr double kHzSteps = 16.0;
        zone.vibrato = Lfo{.hz = key.tremolo_hz_sixteenths / kHzSteps,
                           .delay_ms = static_cast<double>(key.tremolo_delay_ms),
                           .depth = static_cast<double>(key.tremolo_depth)};
    }

    const double limit_cents = 1200.0 * std::log2(kHighestRatio);
    const int limit_key = static_cast<int>(std::ceil((limit_cents - base) / kCentsPerKey));
    if (limit_key > 127)
    {
        return {zone};
    }

    Zone above = zone;
    above.low_key = std::max(limit_key, 0);
    above.follows_key = false;
    above.tune_cents = limit_cents;
    if (limit_key <= 0)
    {
        return {above};
    }

    zone.high_key = limit_key - 1;

    return {zone, above};
}

Zone SbnDrumZone(const SbnSound& sound, const n64::BgmDrum& drum, int key, int tune_cents,
                 std::span<const int32_t> times, uint32_t output_rate)
{
    Zone zone;
    zone.low_key = key;
    zone.high_key = key;
    zone.root_key = key;
    zone.follows_key = false;
    zone.pan = PanOf(drum.pan);
    const double cents = 1200.0 * std::log2(static_cast<double>(sound.sample_rate) / kNominalRate) + drum.key_base +
                         tune_cents - sound.key_base;
    zone.tune_cents = std::min(cents, 1200.0 * std::log2(kHighestRatio));
    FitSbnEnvelope(zone, sound, {}, times, output_rate);

    // A drum's volume counts out of 128, in place of the shift of the track's volume. The send's share comes off here,
    // because the reverb is the drum's.
    const double volume = drum.volume / kVelocityScale;
    zone.gain *= volume * volume * std::max(DryShare(drum.reverb), 0.0);

    return zone;
}

std::unique_ptr<SbnGame> SbnGame::Open(std::span<const uint8_t> image, const SbnProfile& profile)
{
    const std::size_t at = n64::FindSbnArchive(image);
    if (at >= image.size())
    {
        throw xsflib::FormatError("the image holds no SBN archive");
    }

    std::unique_ptr<SbnGame> game(new SbnGame(image, profile, n64::SbnArchive::Parse(image.subspan(at))));

    return game;
}

SbnGame::SbnGame(std::span<const uint8_t> image, const SbnProfile& profile, n64::SbnArchive archive)
    : image_(image), profile_(&profile), archive_(std::move(archive))
{
    for (std::size_t i = 0; i < profile.envelope_time_count; i++)
    {
        times_.push_back(static_cast<int32_t>(xsflib::LoadBe32(image.data() + profile.envelope_times + i * 4)));
    }

    // The first three of the other files: the sound effects, the drums and the instruments.
    const std::vector<uint16_t>& others = archive_.OtherFiles();
    per_ = n64::ParsePerFile(archive_.File(others.at(1)));
    prg_ = n64::ParsePrgFile(archive_.File(others.at(2)));
    for (const n64::SbnArchive::InitBank& bank : archive_.InitBanks())
    {
        init_files_.at(bank.bank_set).at(bank.bank) = bank.file;
    }

    // A song that repeats an earlier song's files adds nothing.
    std::set<std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>> seen;
    const std::vector<n64::SbnArchive::Song>& songs = archive_.Songs();
    for (std::size_t s = 0; s < songs.size(); s++)
    {
        const n64::SbnArchive::Song& song = songs[s];
        if (song.bgm >= archive_.Entries().size() ||
            !seen.insert({song.bgm, song.banks[0], song.banks[1], song.banks[2]}).second)
        {
            continue;
        }

        const std::span<const uint8_t> bytes = archive_.File(song.bgm);
        if (!n64::BgmFile::Matches(bytes))
        {
            continue;
        }

        const n64::BgmFile file = n64::BgmFile::Parse(bytes);
        const auto name_of = [&](int variation, const std::string& change)
        {
            std::vector<std::string> parts;
            const auto named = std::ranges::find(profile_->names, s, &SongName::index);
            if (named != profile_->names.end())
            {
                parts.emplace_back(named->name);
            }

            if (variation != 0)
            {
                parts.push_back("variation " + std::to_string(variation));
            }

            if (!change.empty())
            {
                parts.push_back(change);
            }

            char base[32];
            std::snprintf(base, sizeof(base), "Sequence 0x%02zX", s);
            std::string name = base;
            for (std::size_t i = 0; i < parts.size(); i++)
            {
                name += (i == 0 ? " (" : ", ") + parts[i];
            }

            return parts.empty() ? name : name + ")";
        };

        for (const int variation : SbnVariations(file))
        {
            const auto [mixes, links] = ChoicesOf(file, file.CompositionOffset(variation));
            for (int mix = 0; mix < mixes; mix++)
            {
                for (int linked = 0; linked <= (links ? 1 : 0); linked++)
                {
                    const auto change = std::ranges::find_if(
                        profile_->changes, [&](const SbnChange& c)
                        { return c.song == s && c.levels == 0 && c.mix == mix && c.linked == (linked != 0); });
                    const bool known = change != profile_->changes.end();

                    // The game's triggers pick a mix only in the song's first version, and only the mixes in its
                    // profile.
                    if (mix != 0 && (variation != 0 || !known))
                    {
                        continue;
                    }

                    std::string detail;
                    if (known && change->name != nullptr)
                    {
                        detail = change->name;
                    }
                    else if (mix != 0)
                    {
                        detail = "mix " + std::to_string(mix);
                    }
                    else if (linked != 0)
                    {
                        detail = "linked";
                    }

                    tracks_.push_back({.song = s,
                                       .variation = variation,
                                       .mix = mix,
                                       .linked = linked != 0,
                                       .near = known && change->near,
                                       .name = name_of(variation, detail)});
                }
            }

            if (variation != 0)
            {
                continue;
            }

            for (const SbnChange& change : profile_->changes)
            {
                if (change.song == s && change.levels != 0)
                {
                    tracks_.push_back({.song = s, .levels = change.levels, .name = name_of(0, change.name)});
                }
            }
        }
    }
}

std::vector<ntdlib::sequence_export::Sequence> SbnGame::Sequences() const
{
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        sequences.push_back({i, tracks_[i].name});
    }

    return sequences;
}

const n64::BkFile& SbnGame::Bank(uint16_t file)
{
    const auto found = banks_.find(file);
    if (found != banks_.end())
    {
        return found->second;
    }

    return banks_.emplace(file, n64::BkFile::Parse(archive_.File(file))).first->second;
}

std::optional<SbnGame::Found> SbnGame::Find(uint16_t bank_patch, const n64::SbnArchive::Song& song)
{
    // The number of a set of banks in a song's note, and the game's number of the same set: 0 and 7 are the song's
    // banks, 2 has no banks.
    constexpr std::array<int, 8> kSetOfNumber = {1, 2, 0, 3, 4, 5, 6, 1};
    constexpr int kSongSet = 1;
    const int bank = bank_patch >> 8;
    const std::size_t group = (bank_patch & 0xFF) >> 4;
    const std::size_t slot = bank_patch & 0xF;
    const int set = kSetOfNumber.at(static_cast<std::size_t>((bank & 0x70) >> 4));
    uint16_t file = init_files_.at(static_cast<std::size_t>(set)).at(group);
    if (set == kSongSet && group < song.banks.size() && song.banks[group] != 0)
    {
        file = song.banks[group];
    }

    if (file == 0 || file >= archive_.Entries().size())
    {
        return std::nullopt;
    }

    const n64::BkFile& bk = Bank(file);
    const int32_t index = bk.slots.at(slot);
    if (index < 0 || bk.instruments.at(static_cast<std::size_t>(index)).wave == 0)
    {
        return std::nullopt;
    }

    return Found{&bk, &bk.instruments[static_cast<std::size_t>(index)], file};
}

SbnSound SbnGame::SoundOf(const Found& found, uint16_t bank_patch) const
{
    const n64::BkInstrument& instrument = *found.instrument;
    SbnSound sound;
    sound.key_base = instrument.key_base;
    sound.sample_rate = instrument.sample_rate;

    const std::span<const uint8_t> bytes = found.bank->Bytes();
    const std::size_t choice = (bank_patch >> 8) & 3;
    if (instrument.envelopes != 0 && choice < bytes[instrument.envelopes])
    {
        const std::size_t entry = instrument.envelopes + 4 + choice * 4;
        sound.press = bytes.subspan(instrument.envelopes + xsflib::LoadBe16(bytes.data() + entry));
        sound.release = bytes.subspan(instrument.envelopes + xsflib::LoadBe16(bytes.data() + entry + 2));
    }
    else
    {
        // The game's default envelope: a list of press and a list of release, of one step each.
        constexpr std::size_t kDefaultListBytes = 4;
        sound.press = image_.subspan(profile_->default_envelope, kDefaultListBytes);
        sound.release = image_.subspan(profile_->default_envelope + kDefaultListBytes, kDefaultListBytes);
    }

    return sound;
}

std::shared_ptr<const Wave> SbnGame::WaveOf(const Found& found)
{
    const n64::BkInstrument& instrument = *found.instrument;
    const auto key = std::make_pair(found.file, instrument.offset);
    const auto cached = waves_.find(key);
    if (cached != waves_.end())
    {
        return cached->second;
    }

    constexpr uint8_t kVadpcm = 0;
    n64::VadpcmBook book;
    n64::BankLoop loop;
    const bool vadpcm = instrument.type == kVadpcm;
    if (vadpcm)
    {
        book = found.bank->Book(instrument);
    }

    const bool loops = instrument.loop_end != 0 && instrument.loop_count != 0;
    if (loops)
    {
        loop.start = instrument.loop_start;
        loop.end = instrument.loop_end;
        loop.count = instrument.loop_count;
        if (vadpcm && instrument.loop_state != 0)
        {
            loop.has_state = true;
            loop.state = found.bank->LoopState(instrument);
        }
    }

    std::span<const uint8_t> bytes = found.bank->Wave(instrument);
    if (vadpcm)
    {
        bytes = bytes.first(bytes.size() / n64::kVadpcmFrameSize * n64::kVadpcmFrameSize);
    }

    char name[32];
    std::snprintf(name, sizeof(name), "f%u_%04X", found.file, instrument.offset);
    std::shared_ptr<const Wave> wave =
        DecodeWave(bytes, vadpcm ? &book : nullptr, loops ? &loop : nullptr, name, profile_->output_rate);
    waves_[key] = wave;

    return wave;
}

Conversion SbnGame::Convert(std::size_t index, const PerformOptions& options)
{
    const Track& track = tracks_.at(index);
    const n64::SbnArchive::Song& song = archive_.Songs().at(track.song);
    const n64::BgmFile file = n64::BgmFile::Parse(archive_.File(song.bgm));

    SbnSettings settings;
    settings.output_rate = profile_->output_rate;
    settings.ticks_per_beat = image_[profile_->ticks_per_beat + file.timing_preset];
    settings.variation = track.variation;
    settings.mix = track.mix;
    settings.linked = track.linked;
    settings.proximity = track.near ? kNearLevel : kFullLevel;
    if (track.levels != 0)
    {
        settings.track_levels = image_.subspan(track.levels, kTrackLevelsBytes);
    }

    settings.custom_times = image_.subspan(profile_->custom_times, profile_->custom_time_count);

    const SbnFacts facts = [&](uint16_t bank_patch)
    {
        SbnInstrumentFacts out;
        const std::optional<Found> found = Find(bank_patch, song);
        if (!found)
        {
            return out;
        }

        const n64::BkInstrument& instrument = *found->instrument;
        const SbnSound sound = SoundOf(*found, bank_patch);
        out.exists = true;
        out.key_base = instrument.key_base;
        out.sample_rate = instrument.sample_rate;
        out.samples = static_cast<double>(instrument.wave_size / n64::kVadpcmFrameSize * n64::kVadpcmFrameSamples);
        out.loops = instrument.loop_end != 0 && instrument.loop_count != 0;
        out.release_seconds = SbnReleaseSeconds(sound.release, times_, profile_->output_rate);

        return out;
    };

    SbnPerformance played = PerformSbn(file, per_, prg_, facts, settings, options);
    for (const SbnPresetKey& key : played.presets)
    {
        Instrument preset;
        if (key.drums)
        {
            preset.name = "drums";
            for (const int drum_key : played.drum_keys)
            {
                const n64::BgmDrum* drum = DrumOf(file, per_, drum_key);
                if (drum == nullptr)
                {
                    continue;
                }

                const std::optional<Found> found = Find(drum->bank_patch, song);
                if (!found)
                {
                    continue;
                }

                Zone zone = SbnDrumZone(SoundOf(*found, drum->bank_patch), *drum, drum_key, key.fine_cents, times_,
                                        profile_->output_rate);
                zone.exclusive_class = key.cuts ? 1 : 0;
                zone.wave = WaveOf(*found);
                preset.zones.push_back(std::move(zone));
            }
        }
        else
        {
            char name[32];
            std::snprintf(name, sizeof(name), "%04X", key.bank_patch);
            preset.name = name;
            const std::optional<Found> found = Find(key.bank_patch, song);
            if (found)
            {
                for (Zone& zone :
                     SbnInstrumentZones(SoundOf(*found, key.bank_patch), key, times_, profile_->output_rate))
                {
                    zone.wave = WaveOf(*found);
                    preset.zones.push_back(std::move(zone));
                }
            }
        }

        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), track.name, options.program);
}

} // namespace ntdlib::sequence_export::n64

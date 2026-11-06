// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/libmus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <utility>

#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/rom_data.h"
#include "ntdlib/n64/vadpcm.h"
#include "ntdlib/sequence_export/n64/envelope.h"
#include "ntdlib/sequence_export/n64/libultra.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

namespace n64 = ntdlib::n64;
namespace midi = seqlib::midi;

// The commands of a song's lists. A byte below these is a note.
enum Command : uint8_t
{
    kStop = 0x80,
    kWave = 0x81,
    kPortamento = 0x82,
    kPortamentoOff = 0x83,
    kEnvelopeBytes = 0x84,
    kTempo = 0x85,
    kCutoff = 0x86,
    kEndIt = 0x87,
    kVibratoUp = 0x88,
    kVibratoDown = 0x89,
    kVibratoOff = 0x8A,
    kLength = 0x8B,
    kIgnoreLength = 0x8C,
    kTranspose = 0x8D,
    kIgnoreTranspose = 0x8E,
    kDistort = 0x8F,
    kEnvelope = 0x90,
    kEnvelopeOff = 0x91,
    kEnvelopeOn = 0x92,
    kTriggerOff = 0x93,
    kTriggerOn = 0x94,
    kFor = 0x95,
    kNext = 0x96,
    kWobble = 0x97,
    kWobbleOff = 0x98,
    kVelocityOn = 0x99,
    kVelocityOff = 0x9A,
    kDefaultVelocity = 0x9B,
    kPan = 0x9C,
    kStereo = 0x9D,
    kDrums = 0x9E,
    kDrumsOff = 0x9F,
    kPrint = 0xA0,
    kGoto = 0xA1,
    kReverb = 0xA2,
    kRandomNote = 0xA3,
    kRandomVolume = 0xA4,
    kRandomPan = 0xA5,
    kVolume = 0xA6,
    kStartEffect = 0xA7,
    kBendRange = 0xA8,
    kSweep = 0xA9,
    kChangeEffect = 0xAA,
    kMarker = 0xAB,
    kLengthOff = 0xAC,
};

// A For with this count repeats for ever.
constexpr uint8_t kForever = 0xFF;

// The player keeps this many loops open.
constexpr std::size_t kForDepth = 4;

constexpr int kRest = 96;

// The scale of a song's tempo that changes nothing.
constexpr int kFullTempoScale = 128;

// A note of this length plays until the channel stops.
constexpr int kEndlessLength = 0x7FFF;

// The player's callbacks a second, as its tempos count them. The player asks for its next callback in this many
// microseconds. The synthesizer rounds that to whole samples.
constexpr int kCallbacksPerSecond = 60;
constexpr double kCallbackMicroseconds = 1000000 / kCallbacksPerSecond;

// A channel's clock counts this much for a tick.
constexpr int kFramesPerTick = 256;

// A script's tempo before the song sets one.
constexpr int kDefaultTempo = 96 * kFramesPerTick / kCallbacksPerSecond;

// The file's ticks in a quarter note. The tempo gives their time.
constexpr uint32_t kTicksPerQuarter = 48;

// The highest ratio of a wave's rate to the output's. A higher note is silent.
constexpr double kHighestSemitones = 12.0;

// The library's name for a note of 48 is C4, which is key 60 in MIDI.
constexpr int kKeyOfNote = 12;

// The player's volume above which the voice's volume stops.
constexpr int kHighestVoiceVolume = 32767;
constexpr int kVolumeShift = 13;

// What's sent to each song's channels. The player shares its 16 MIDI tracks among them.
constexpr std::size_t kMidiTracks = 16;

// In the later form, a new note on a channel cuts the last one. Every zone therefore cuts the last note of its
// channel. In the early form, a channel sends its sounding voice into its release and allocates a new voice for the
// next note (the code at 0x8003AE98 and 0x8003B3F4 in Pokémon Stadium). Its zones cut nothing.
constexpr int kChannelClass = 1;

// A cut that takes this many commands without a tick has no end.
constexpr int kMostCommandsInATick = 100000;

constexpr double kFullVoice = 32768.0;

// A vibrato's amount counts fiftieths of a semitone.
constexpr double kVibratoUnitsPerSemitone = 50.0;

// The bend range that a channel starts with. In a song of the early form, each new pass of a loop sets it again.
constexpr double kDefaultBendRange = 2.0 / 64.0;

// A note of the early form starts at its peak if its attack takes less than this.
constexpr int kShortestEarlyAttack = 2;

constexpr double kPi = 3.14159260000000007;

// A main loop can't end later than this. The longest performance ends long before.
constexpr uint64_t kLongestLoopEnd = 200000000;

int TempoOfScript(int value)
{
    return value * kFramesPerTick * 96 / 120 / kCallbacksPerSecond;
}

double TicksPerSecond(int tempo, uint32_t output_rate)
{
    const double samples = std::floor(kCallbackMicroseconds * output_rate / 1000000.0 + 0.5);

    return tempo * (output_rate / samples) / kFramesPerTick;
}

// The bytes that a song's header points to. A song of the library's early form has no master track, and its channels
// name the bank's waves directly.
struct SongFile
{
    std::span<const uint8_t> bytes;
    bool early = false;
    uint32_t channels = 0;
    uint32_t waves = 0;
    uint32_t data_list = 0;
    uint32_t volume_list = 0;
    uint32_t bend_list = 0;
    uint32_t envelopes = 0;
    uint32_t drums = 0;
    uint32_t wave_table = 0;
    uint32_t master_track = 0;

    static SongFile Parse(std::span<const uint8_t> bytes, bool early)
    {
        constexpr std::size_t kHeaderSize = 0x2C;
        constexpr std::size_t kEarlyHeaderSize = 0x18;
        if (bytes.size() < (early ? kEarlyHeaderSize : kHeaderSize))
        {
            throw xsflib::FormatError("a song is too short for its header");
        }

        const auto word = [&](std::size_t at)
        {
            return xsflib::LoadBe32(bytes.data() + at);
        };

        SongFile file;
        file.bytes = bytes;
        file.early = early;
        if (early)
        {
            file.channels = word(0x00);
            file.data_list = word(0x04);
            file.volume_list = word(0x08);
            file.bend_list = word(0x0C);
            file.envelopes = word(0x10);
            file.drums = word(0x14);
        }
        else
        {
            file.channels = word(0x04);
            file.waves = word(0x08);
            file.data_list = word(0x0C);
            file.volume_list = word(0x10);
            file.bend_list = word(0x14);
            file.envelopes = word(0x18);
            file.drums = word(0x1C);
            file.wave_table = word(0x20);
            file.master_track = word(0x24);
        }

        if (file.data_list + std::size_t{4} * file.channels > bytes.size() ||
            file.volume_list + std::size_t{4} * file.channels > bytes.size() ||
            file.bend_list + std::size_t{4} * file.channels > bytes.size())
        {
            throw xsflib::FormatError("a song's lists don't fit in its file");
        }

        return file;
    }

    uint32_t Entry(uint32_t list, uint32_t channel) const
    {
        return xsflib::LoadBe32(bytes.data() + list + std::size_t{4} * channel);
    }

    int WaveOf(int wave) const
    {
        if (early)
        {
            return wave;
        }

        const std::size_t at = wave_table + std::size_t{2} * static_cast<std::size_t>(wave);
        if (at + 2 > bytes.size())
        {
            throw xsflib::FormatError("a song's wave is outside its table");
        }

        const uint16_t value = xsflib::LoadBe16(bytes.data() + at);
        return value == 0xFFFF ? -1 : value;
    }

    uint8_t At(std::size_t place) const
    {
        if (place >= bytes.size())
        {
            throw xsflib::FormatError("a song's list runs past its file");
        }

        return bytes[place];
    }
};

// A setting or a sound of a channel, at its tick.
struct Action
{
    enum class Kind : uint8_t
    {
        kOff,
        kVolume,
        kBend,
        kNote,
    };

    uint32_t tick = 0;
    Kind kind = Kind::kNote;
    uint32_t note = 0; // kOff and kNote: the note's number among the channel's
    uint8_t key = 0;
    uint8_t velocity = 0; // kVolume: the volume
    uint8_t pan = 64;
    uint8_t reverb = 0;
    float semitones = 0.0F;
    LibmusPresetKey preset;
};

// Where each of an envelope's bytes is.
constexpr std::size_t kEnvelopeFirst = 1;
constexpr std::size_t kEnvelopeRelease = 6;

// How a channel's list goes: the lists hold the settings that a note takes when it starts.
class ChannelReader
{
public:
    ChannelReader(const SongFile& file, std::span<const double> detunes, const LibmusSettings& settings, uint32_t data,
                  uint32_t volume, uint32_t bend, midi::Recorder& recorder)
        : file_(file),
          detunes_(detunes),
          settings_(settings),
          place_(data),
          base_(data),
          volume_list_(volume),
          bend_list_(bend),
          volume_base_(volume),
          bend_base_(bend),
          recorder_(recorder)
    {
        if (file_.early)
        {
            velocity_on_ = false;
            envelope_[kEnvelopeRelease] = 1;
        }
    }

    // Runs the channel's list up to tick `end`.
    void Run(uint32_t end);

    // Runs the channel's list up to the end of its first endless loop, or its stop.
    void RunToLoop();

    const std::vector<Action>& Actions() const
    {
        return actions_;
    }
    const std::vector<std::pair<uint32_t, int>>& Tempos() const
    {
        return tempos_;
    }
    std::optional<uint32_t> LoopStart() const
    {
        return loop_start_;
    }
    std::optional<uint32_t> LoopEnd() const
    {
        return loop_end_;
    }
    uint32_t EndTick() const
    {
        return note_end_;
    }

private:
    struct OpenFor
    {
        uint8_t count = 0;
        std::size_t place = 0;
        std::size_t volume_place = 0;
        std::size_t bend_place = 0;
        int volume = 0;
        double bend = 0.0;
        int volume_repeat = 0;
        int bend_repeat = 0;
    };

    void Step();
    bool RunCommand(uint8_t command);
    void StartNote(int note);
    void EndCurrentNote(uint32_t tick);
    void ReadVolume();
    void ReadBend();
    void SendChanges(uint32_t tick);
    double PitchOfNoteAt(uint32_t tick);
    void SetEnvelope(std::size_t at);
    int Sized();

    const SongFile& file_;
    std::span<const double> detunes_;
    const LibmusSettings& settings_;
    std::size_t place_ = 0;
    std::size_t base_ = 0;
    std::size_t volume_list_ = 0;
    std::size_t bend_list_ = 0;
    std::size_t volume_base_ = 0;
    std::size_t bend_base_ = 0;
    midi::Recorder& recorder_;

    bool stopped_ = false;
    bool stop_at_loop_ = false;
    std::vector<OpenFor> fors_;
    std::optional<uint32_t> loop_start_;
    std::optional<uint32_t> loop_end_;

    uint32_t note_end_ = 0;
    int length_ = 1;
    int fixed_length_ = 0;
    bool ignore_length_ = false;
    bool velocity_on_ = true;
    int default_velocity_ = 127;
    int velocity_ = 0;
    int wave_ = 0;
    std::optional<uint32_t> drums_;
    int pan_ = 64;
    int transpose_ = 0;
    bool ignore_transpose_ = false;
    std::array<uint8_t, 7> envelope_ = {1, 0, 1, 127, 255, 127, 15};
    int cutoff_ = 0;
    int end_it_ = 0;
    int volume_ = 127;
    int volume_repeat_ = 1;
    int bend_repeat_ = 1;
    double bend_ = 0.0;
    double bend_range_ = kDefaultBendRange;
    double bend_semitones_ = 0.0;
    int reverb_ = 0;
    int vibrato_delay_ = 0;
    int vibrato_speed_ = 0;
    double vibrato_amount_ = 0.0;
    double vibrato_ = 0.0;
    int portamento_ = 0;
    double portamento_from_ = 0.0;
    double portamento_base_ = 0.0;
    double note_pitch_ = 0.0;
    uint32_t note_start_ = 0;
    int sent_volume_ = 127;
    double sent_pitch_ = 0.0;

    uint32_t notes_ = 0;
    bool sounding_ = false;
    uint32_t off_tick_ = 0;
    std::vector<Action> actions_;
    std::vector<std::pair<uint32_t, int>> tempos_;
};

int ChannelReader::Sized()
{
    const int first = file_.At(place_++);
    if (first < 0x80)
    {
        return first;
    }

    return ((first & 0x7F) << 8) | file_.At(place_++);
}

// Runs one command. Returns false if the channel's list has ended.
bool ChannelReader::RunCommand(uint8_t command)
{
    const auto skip = [&](std::size_t bytes, const char* what)
    {
        place_ += bytes;
        recorder_.Approximate(std::string("the command for ") + what + " left out");
    };

    switch (command)
    {
    case kStop:
        stopped_ = true;
        return false;
    case kWave:
        wave_ = Sized();
        break;
    case kPortamento:
        portamento_ = file_.At(place_++);
        if (portamento_ != 0)
        {
            portamento_base_ = note_pitch_;
        }

        break;
    case kPortamentoOff:
        portamento_ = 0;
        break;
    case kVibratoOff:
        vibrato_speed_ = 0;
        vibrato_ = 0.0;
        break;
    case kWobbleOff:
    case kEnvelopeOn:
    case kTriggerOn:
        break;
    case kEnvelopeBytes:
        SetEnvelope(place_);
        place_ += envelope_.size();
        break;
    case kTempo:
        tempos_.emplace_back(note_end_, TempoOfScript(file_.At(place_++)));
        break;
    case kCutoff:
        cutoff_ = (file_.At(place_) << 8) | file_.At(place_ + 1);
        place_ += 2;
        end_it_ = 0;
        break;
    case kEndIt:
        end_it_ = file_.At(place_++);
        cutoff_ = 0;
        break;
    case kVibratoUp:
    case kVibratoDown:
        vibrato_delay_ = file_.At(place_);
        vibrato_speed_ = file_.At(place_ + 1);
        vibrato_amount_ = (command == kVibratoUp ? 1.0 : -1.0) * file_.At(place_ + 2) / kVibratoUnitsPerSemitone;
        place_ += 3;
        break;
    case kLength:
        fixed_length_ = Sized();
        break;
    case kIgnoreLength:
        ignore_length_ = true;
        break;
    case kTranspose:
        transpose_ = static_cast<int8_t>(file_.At(place_++));
        break;
    case kIgnoreTranspose:
        ignore_transpose_ = true;
        break;
    case kDistort:
        if (file_.At(place_) != 0)
        {
            recorder_.Approximate("a channel's detune left out");
        }

        place_++;
        break;
    case kEnvelope:
        SetEnvelope(file_.envelopes + std::size_t{7} * static_cast<std::size_t>(Sized()));
        break;
    case kEnvelopeOff:
        recorder_.Approximate("a note that keeps the last note's envelope");
        break;
    case kTriggerOff:
        recorder_.Approximate("a note that keeps the last note's sample");
        break;
    case kFor:
        {
            if (fors_.size() >= kForDepth)
            {
                throw xsflib::FormatError("a song's loops are too deep");
            }

            const uint8_t count = file_.At(place_++);
            fors_.push_back({count, place_, volume_list_, bend_list_, volume_, bend_, volume_repeat_, bend_repeat_});
            if (count == kForever && !loop_start_)
            {
                loop_start_ = note_end_;
            }

            break;
        }
    case kNext:
        {
            if (fors_.empty())
            {
                throw xsflib::FormatError("a song's Next has no For");
            }

            OpenFor& open = fors_.back();
            if (open.count == kForever)
            {
                if (!loop_end_)
                {
                    loop_end_ = note_end_;
                }

                if (stop_at_loop_)
                {
                    return false;
                }
            }
            else if (--open.count == 0)
            {
                fors_.pop_back();
                break;
            }

            place_ = open.place;
            volume_list_ = open.volume_place;
            bend_list_ = open.bend_place;
            volume_ = open.volume;
            bend_ = open.bend;
            volume_repeat_ = open.volume_repeat;
            bend_repeat_ = open.bend_repeat;
            if (file_.early)
            {
                bend_range_ = kDefaultBendRange;
            }

            bend_semitones_ = bend_ * bend_range_;
            break;
        }
    case kWobble:
        if (file_.At(place_) != 0 && file_.At(place_ + 1) != 0)
        {
            recorder_.Approximate("a pitch wobble left out");
        }

        place_ += 3;
        break;
    case kVelocityOn:
        velocity_on_ = true;
        break;
    case kVelocityOff:
        velocity_on_ = false;
        break;
    case kDefaultVelocity:
        default_velocity_ = file_.At(place_++);
        velocity_on_ = false;
        break;
    case kPan:
        pan_ = file_.At(place_++) / 2;
        break;
    case kStereo:
        place_ += 2;
        break;
    case kDrums:
        drums_ = file_.drums + uint32_t{6} * static_cast<uint32_t>(Sized());
        break;
    case kDrumsOff:
        drums_.reset();
        break;
    case kPrint:
        place_++;
        break;
    case kGoto:
        {
            const std::size_t to = static_cast<std::size_t>((file_.At(place_) << 8) | file_.At(place_ + 1));
            const std::size_t volume = static_cast<std::size_t>((file_.At(place_ + 2) << 8) | file_.At(place_ + 3));
            const std::size_t bend = static_cast<std::size_t>((file_.At(place_ + 4) << 8) | file_.At(place_ + 5));
            place_ = base_ + to;
            volume_list_ = volume_base_ + volume;
            bend_list_ = bend_base_ + bend;
            volume_repeat_ = 1;
            bend_repeat_ = 1;
            break;
        }
    case kReverb:
        reverb_ = file_.At(place_++);
        break;
    case kRandomNote:
    case kRandomVolume:
    case kRandomPan:
        skip(2, "random values");
        break;
    case kVolume:
        volume_ = file_.At(place_++);
        break;
    case kStartEffect:
        Sized();
        recorder_.Approximate("a song's start of a sound effect left out");
        break;
    case kBendRange:
        bend_range_ = file_.At(place_++) / 64.0;

        // The early form's code squares the bend here, up to the bend list's next value.
        bend_semitones_ = file_.early ? bend_ * bend_ : bend_ * bend_range_;
        break;
    case kSweep:
        if (file_.At(place_) != 0)
        {
            recorder_.Approximate("a pan's sweep left out");
        }

        place_++;
        break;
    case kChangeEffect:
        skip(1, "a change of the effect");
        break;
    case kMarker:
        place_++;
        Sized();
        break;
    case kLengthOff:
        fixed_length_ = 0;
        break;
    default:
        throw xsflib::FormatError("a song holds an unknown command");
    }

    return true;
}

void ChannelReader::SetEnvelope(std::size_t at)
{
    for (std::size_t i = 0; i < envelope_.size(); i++)
    {
        envelope_[i] = file_.At(at + i);
    }

    // The early form's notes all start at nothing.
    if (file_.early)
    {
        envelope_[kEnvelopeFirst] = 0;
    }
}

void ChannelReader::EndCurrentNote(uint32_t tick)
{
    if (sounding_)
    {
        Action off;
        off.tick = std::min(off_tick_, tick);
        off.kind = Action::Kind::kOff;
        off.note = notes_ - 1;
        actions_.push_back(off);
        sounding_ = false;
    }
}

void ChannelReader::StartNote(int note)
{
    // A portamento starts from where the last one ended.
    portamento_from_ = portamento_base_;

    int velocity = velocity_;
    if (velocity_on_)
    {
        int value = file_.At(place_++);
        if (value >= 0x80)
        {
            value &= 0x7F;
            velocity_on_ = false;
            default_velocity_ = value;
        }

        if (note != kRest)
        {
            velocity = value;
        }
    }
    else
    {
        velocity = default_velocity_;
    }

    velocity_ = velocity;

    if (fixed_length_ == 0)
    {
        length_ = Sized();
    }
    else if (ignore_length_)
    {
        ignore_length_ = false;
        length_ = Sized();
    }
    else
    {
        length_ = fixed_length_;
    }

    const uint32_t start = note_end_;
    note_end_ += static_cast<uint32_t>(length_);

    if (!drums_ && file_.WaveOf(wave_) < 0)
    {
        note = kRest;
    }

    // A rest lets the last note's release run. A note's end has started that release already.
    EndCurrentNote(start);
    if (note == kRest)
    {
        return;
    }

    if (drums_)
    {
        const std::size_t at = *drums_ + std::size_t{6} * static_cast<std::size_t>(note);
        wave_ = (file_.At(at) << 8) | file_.At(at + 1);
        const std::size_t envelope =
            file_.envelopes + std::size_t{7} * ((std::size_t{file_.At(at + 2)} << 8) | file_.At(at + 3));
        pan_ = file_.At(at + 4) / 2;
        note = file_.At(at + 5);
        for (std::size_t i = 0; i < envelope_.size(); i++)
        {
            envelope_[i] = file_.At(envelope + i);
        }
    }

    // Two notes in one frame leave the second.
    if (length_ == 0)
    {
        recorder_.Approximate("a note of no length left out");
        return;
    }

    const int bank_wave = file_.WaveOf(wave_);
    if (bank_wave < 0 || static_cast<std::size_t>(bank_wave) >= detunes_.size())
    {
        throw xsflib::FormatError("a song's wave isn't in its bank");
    }

    const int transposed = note + (ignore_transpose_ ? 0 : transpose_);
    const double detune = detunes_[static_cast<std::size_t>(bank_wave)];
    const double semitones = transposed + detune + bend_semitones_;
    note_pitch_ = note + detune;
    note_start_ = start;

    // The player silences a note above an octave, and a voice's volume stops at its top. The velocity takes that at the
    // envelope's top.
    int top = std::max({envelope_[1], envelope_[3], envelope_[5]});
    const long product = static_cast<long>(volume_) * top * settings_.song_volume;
    int heard = velocity;
    if (product > 0)
    {
        // The early form's volume stops at its top after the master volume. The later form's stops before.
        long limit = (long{kHighestVoiceVolume + 1} << kVolumeShift) / product;
        if (file_.early)
        {
            limit = limit * (kHighestVoiceVolume + 1) / settings_.master_volume;
        }

        if (heard > limit)
        {
            heard = static_cast<int>(limit);
            recorder_.Approximate("a voice held at the top of its volume, for as long as its envelope is at its top");
        }
    }

    if (semitones > kHighestSemitones)
    {
        heard = 0;
    }

    Action action;
    action.tick = start;
    action.kind = Action::Kind::kNote;
    action.note = notes_++;
    action.key = static_cast<uint8_t>(std::clamp(transposed + kKeyOfNote, 0, 127));

    // A silent note still cuts the last note. The lowest velocity keeps that.
    action.velocity = static_cast<uint8_t>(std::clamp(heard, 1, 127));
    action.pan = static_cast<uint8_t>(std::clamp(pan_, 0, 127));
    action.reverb = static_cast<uint8_t>(std::clamp(reverb_, 0, 127));
    action.preset = {bank_wave, envelope_};
    actions_.push_back(action);

    sounding_ = true;
    if (length_ == kEndlessLength)
    {
        off_tick_ = std::numeric_limits<uint32_t>::max();
    }
    else if (cutoff_ != 0)
    {
        off_tick_ = start + static_cast<uint32_t>(cutoff_);
    }
    else
    {
        // A MIDI note needs a tick to sound. A note whose release starts at once gets that.
        off_tick_ = std::max(note_end_ - std::min(static_cast<uint32_t>(end_it_), note_end_), start + 1);
    }
}

void ChannelReader::Step()
{
    // The commands up to the next note, and the note.
    for (int i = 0; i < kMostCommandsInATick; i++)
    {
        const uint8_t byte = file_.At(place_++);
        if (byte < 0x80)
        {
            StartNote(byte);

            return;
        }

        if (!RunCommand(byte))
        {
            return;
        }
    }

    throw xsflib::FormatError("a song's channel loops without a note");
}

void ChannelReader::ReadVolume()
{
    if (volume_base_ == 0 || --volume_repeat_ != 0)
    {
        return;
    }

    int value = file_.At(volume_list_++);
    volume_repeat_ = 1;
    if (value > 127)
    {
        value &= 0x7F;
        const int count = file_.At(volume_list_++);
        volume_repeat_ = count > 127 ? ((count & 0x7F) * 256) + file_.At(volume_list_++) + 2 : count + 2;
    }

    volume_ = value;
}

void ChannelReader::ReadBend()
{
    if (bend_base_ == 0 || --bend_repeat_ != 0)
    {
        return;
    }

    int value = file_.At(bend_list_++);
    bend_repeat_ = 1;
    if (value > 127)
    {
        value &= 0x7F;
        const int count = file_.At(bend_list_++);
        bend_repeat_ = count > 127 ? ((count & 0x7F) * 256) + file_.At(bend_list_++) + 2 : count + 2;
    }

    bend_ = value - 64.0;
    bend_semitones_ = bend_ * bend_range_;
}

// The pitch that a note's vibrato and portamento give it at a tick, in semitones from the note.
double ChannelReader::PitchOfNoteAt(uint32_t tick)
{
    const uint32_t ticks = tick - note_start_;

    double pitch = 0.0;
    if (portamento_ != 0)
    {
        double at = note_pitch_;
        if (ticks <= static_cast<uint32_t>(portamento_))
        {
            at = portamento_from_ + ((note_pitch_ - portamento_from_) * ticks / portamento_);
        }

        portamento_base_ = at;
        pitch += at - note_pitch_;
    }

    if (vibrato_speed_ != 0)
    {
        if (file_.early)
        {
            // The early form's count wraps before the delay's end. That holds the pitch at one value up to there. A
            // float keeps the value that the game's sinf takes.
            const uint32_t count = ticks - static_cast<uint32_t>(vibrato_delay_);
            if (count != 0)
            {
                const float turns = static_cast<float>(count) / static_cast<float>(vibrato_speed_) * 2.0F;
                vibrato_ = std::sin(static_cast<double>(static_cast<float>(static_cast<double>(turns) * kPi))) *
                           vibrato_amount_;
            }
        }
        else
        {
            const int count = static_cast<int>(ticks) - vibrato_delay_;
            vibrato_ =
                count > 0 ? std::sin(static_cast<double>(count) * 2.0 * kPi / vibrato_speed_) * vibrato_amount_ : 0.0;
        }

        pitch += vibrato_;
    }

    return pitch;
}

// Sends what a tick's reads changed. A voice takes no new pitch once its release has started.
void ChannelReader::SendChanges(uint32_t tick)
{
    if (volume_ != sent_volume_)
    {
        sent_volume_ = volume_;
        Action action;
        action.tick = tick;
        action.kind = Action::Kind::kVolume;
        action.velocity = static_cast<uint8_t>(volume_);
        actions_.push_back(action);
    }

    if (!sounding_ || tick >= off_tick_)
    {
        return;
    }

    const double pitch = bend_semitones_ + PitchOfNoteAt(tick);
    if (pitch != sent_pitch_)
    {
        sent_pitch_ = pitch;
        Action action;
        action.tick = tick;
        action.kind = Action::Kind::kBend;
        action.semitones = static_cast<float>(pitch);
        actions_.push_back(action);
    }
}

void ChannelReader::Run(uint32_t end)
{
    for (uint32_t tick = 0; tick < end; tick++)
    {
        while (!stopped_ && note_end_ <= tick && length_ != kEndlessLength)
        {
            Step();
        }

        if (stopped_)
        {
            EndCurrentNote(tick);
            return;
        }

        ReadVolume();
        ReadBend();
        SendChanges(tick);
    }

    EndCurrentNote(end);
}

void ChannelReader::RunToLoop()
{
    stop_at_loop_ = true;
    while (!stopped_ && !loop_end_ && length_ != kEndlessLength)
    {
        Step();
    }
}

} // namespace

// Scales a song's tempos, as the game's code does a few frames after it starts the song. The library multiplies in
// whole numbers.
void ScaleTempos(std::vector<std::pair<uint32_t, int>>& tempos, const LibmusSettings& settings)
{
    for (auto& [tick, tempo] : tempos)
    {
        tempo = tempo * settings.tempo_scale / kFullTempoScale;
    }
}

LibmusPerformance PerformLibmus(std::span<const uint8_t> song, std::span<const double> detunes,
                                const LibmusSettings& settings, const PerformOptions& options)
{
    const SongFile file = SongFile::Parse(song, settings.early_form);

    midi::Recorder recorder;
    LibmusPerformance out;
    Performance& performance = out.performance;
    performance.timebase = kTicksPerQuarter;

    const auto reader_of = [&](uint32_t channel)
    {
        const bool master = channel == file.channels;
        return ChannelReader(file, detunes, settings, master ? file.master_track : file.Entry(file.data_list, channel),
                             master ? 0 : file.Entry(file.volume_list, channel),
                             master ? 0 : file.Entry(file.bend_list, channel), recorder);
    };

    // A first walk finds the main loop: where every channel is inside its endless loop, and the shortest length that
    // brings them all back to where they were.
    const auto listed = [&](uint32_t channel)
    {
        return channel == file.channels ? file.master_track != 0 : file.Entry(file.data_list, channel) != 0;
    };
    uint32_t loop_start = 0;
    uint64_t loop_length = 0;
    bool loops = true;
    uint32_t last_end = 0;
    midi::Recorder walk_recorder;
    for (uint32_t channel = 0; channel <= file.channels; channel++)
    {
        if (!listed(channel))
        {
            continue;
        }

        const bool master = channel == file.channels;
        ChannelReader reader(file, detunes, settings, master ? file.master_track : file.Entry(file.data_list, channel),
                             0, 0, walk_recorder);
        reader.RunToLoop();
        last_end = std::max(last_end, reader.EndTick());
        if (reader.LoopStart() && reader.LoopEnd() && *reader.LoopEnd() > *reader.LoopStart())
        {
            loop_start = std::max(loop_start, *reader.LoopStart());
            const uint64_t length = *reader.LoopEnd() - *reader.LoopStart();
            loop_length = loop_length == 0 ? length : std::lcm(loop_length, length);
        }
        else
        {
            loops = false;
        }
    }

    // Each channel's actions, and the tempo that any of them sets.
    uint32_t end_tick =
        loops ? static_cast<uint32_t>(std::min<uint64_t>(loop_start + loop_length, kLongestLoopEnd)) : last_end;
    std::vector<std::vector<Action>> channels;
    std::vector<std::pair<uint32_t, int>> tempos;
    for (uint32_t channel = 0; channel <= file.channels; channel++)
    {
        if (!listed(channel))
        {
            channels.emplace_back();
            continue;
        }

        ChannelReader reader = reader_of(channel);
        reader.Run(end_tick);
        tempos.insert(tempos.end(), reader.Tempos().begin(), reader.Tempos().end());
        channels.push_back(reader.Actions());
    }

    // The tempo.
    std::ranges::stable_sort(tempos, {}, [](const auto& tempo) { return tempo.first; });
    if (tempos.empty() || tempos[0].first != 0)
    {
        tempos.insert(tempos.begin(), {0, kDefaultTempo});
    }

    tempos.erase(std::ranges::unique(tempos, {}, [](const auto& tempo) { return tempo.first; }).begin(), tempos.end());
    ScaleTempos(tempos, settings);
    const auto seconds_at = [&](uint32_t tick)
    {
        double seconds = 0.0;
        for (std::size_t i = 0; i < tempos.size(); i++)
        {
            const uint32_t from = tempos[i].first;
            if (from >= tick)
            {
                break;
            }

            const uint32_t to = i + 1 < tempos.size() ? std::min(tempos[i + 1].first, tick) : tick;
            seconds += (to - from) / TicksPerSecond(tempos[i].second, settings.output_rate);
        }

        return seconds;
    };

    if (loops && seconds_at(end_tick) > kLongestSeconds)
    {
        recorder.Approximate("a song cut at " + std::to_string(static_cast<int>(kLongestSeconds)) +
                             " seconds, before its channels' loops come back together");
        uint32_t tick = 0;
        while (seconds_at(tick) < kLongestSeconds)
        {
            tick += kTicksPerQuarter;
        }

        end_tick = tick;
        loops = false;
    }

    for (const auto& [tick, tempo] : tempos)
    {
        recorder.SetTempo(performance, tick, TicksPerSecond(tempo, settings.output_rate) * 60.0 / kTicksPerQuarter);
    }

    if (tempos.size() > 1)
    {
        recorder.Approximate("a song's change of tempo, which stays out of its envelopes' times");
    }

    out.ticks_per_second = TicksPerSecond(tempos[0].second, settings.output_rate);

    // Channels with notes get a MIDI track. More channels than tracks share: the two whose notes meet least.
    std::vector<std::vector<std::size_t>> tracks;
    for (std::size_t channel = 0; channel < channels.size(); channel++)
    {
        if (std::ranges::any_of(channels[channel], [](const Action& a) { return a.kind == Action::Kind::kNote; }))
        {
            tracks.push_back({channel});
        }
    }

    const auto spans_of = [&](const std::vector<std::size_t>& group)
    {
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        for (const std::size_t channel : group)
        {
            const std::vector<Action>& actions = channels[channel];
            for (std::size_t i = 0; i < actions.size(); i++)
            {
                if (actions[i].kind == Action::Kind::kNote)
                {
                    uint32_t until = end_tick;
                    for (std::size_t j = i + 1; j < actions.size(); j++)
                    {
                        if (actions[j].kind == Action::Kind::kNote)
                        {
                            until = actions[j].tick;
                            break;
                        }
                    }

                    spans.emplace_back(actions[i].tick, until);
                }
            }
        }

        return spans;
    };

    while (tracks.size() > kMidiTracks)
    {
        std::size_t best_a = 0;
        std::size_t best_b = 1;
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (std::size_t a = 0; a < tracks.size(); a++)
        {
            const auto spans_a = spans_of(tracks[a]);
            for (std::size_t b = a + 1; b < tracks.size(); b++)
            {
                uint64_t overlap = 0;
                for (const auto& [from_b, to_b] : spans_of(tracks[b]))
                {
                    for (const auto& [from_a, to_a] : spans_a)
                    {
                        const uint32_t from = std::max(from_a, from_b);
                        const uint32_t to = std::min(to_a, to_b);
                        overlap += to > from ? to - from : 0;
                    }
                }

                if (overlap < best)
                {
                    best = overlap;
                    best_a = a;
                    best_b = b;
                }
            }
        }

        tracks[best_a].insert(tracks[best_a].end(), tracks[best_b].begin(), tracks[best_b].end());
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(best_b));
        recorder.Approximate("more channels than a MIDI file holds: two share a track, and their notes meet for " +
                             std::to_string(best) + " ticks");
    }

    // The actions in time order: a note's end first, then a setting, then the next note.
    for (std::size_t index = 0; index < tracks.size(); index++)
    {
        midi::TrackOutput output;
        output.midi_track = static_cast<uint8_t>(index);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kVolume, 127);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kExpression, 127);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kReverb, 0);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kPan, 64);

        struct Placed
        {
            Action action;
            std::size_t channel = 0;
        };

        std::vector<Placed> placed;
        for (const std::size_t channel : tracks[index])
        {
            for (const Action& action : channels[channel])
            {
                placed.push_back({action, channel});
            }
        }

        std::ranges::stable_sort(placed,
                                 [](const Placed& a, const Placed& b)
                                 {
                                     if (a.action.tick != b.action.tick)
                                     {
                                         return a.action.tick < b.action.tick;
                                     }

                                     return a.action.kind < b.action.kind;
                                 });

        std::map<std::pair<std::size_t, uint32_t>, std::pair<uint8_t, uint32_t>> started;
        for (const Placed& entry : placed)
        {
            const Action& action = entry.action;
            switch (action.kind)
            {
            case Action::Kind::kVolume:
                recorder.SendSetting(performance, output, action.tick, midi::EventKind::kVolume, action.velocity);
                break;
            case Action::Kind::kBend:
                recorder.SendPitch(performance, output, action.tick, action.semitones);
                break;
            case Action::Kind::kNote:
                {
                    recorder.SendPreset(performance, output, action.tick,
                                        midi::PresetIndex(out.presets, action.preset));
                    recorder.SendSetting(performance, output, action.tick, midi::EventKind::kPan, action.pan);
                    recorder.SendSetting(performance, output, action.tick, midi::EventKind::kReverb, action.reverb);

                    // The mixer takes the send's share from the dry level, on the sin/cos law. The controller acts
                    // squared.
                    const double dry = std::cos(kPi / 2.0 * action.reverb / 127.0);
                    recorder.SendSetting(performance, output, action.tick, midi::EventKind::kExpression,
                                         static_cast<uint8_t>(std::lround(127.0 * std::sqrt(dry))));
                    const uint32_t id = recorder.NoteOn(performance, output, action.tick, action.key, action.velocity);
                    started[{entry.channel, action.note}] = {action.key, id};
                    break;
                }
            case Action::Kind::kOff:
                {
                    const auto& [key, id] = started.at({entry.channel, action.note});
                    recorder.NoteOff(performance, output.midi_track, action.tick, 0, key, id);
                    break;
                }
            }
        }
    }

    if (loops)
    {
        midi::ClampNoteOffs(performance, end_tick);
        midi::CutAt(performance, end_tick);
        performance.loop = midi::Performance::Loop{loop_start, end_tick};
        performance.loops = options.loops;
    }

    performance.end_tick = end_tick;
    performance.seconds = seconds_at(end_tick);
    performance.approximations = recorder.Approximations();
    performance.warnings = recorder.Warnings();

    return out;
}

Zone LibmusZone(const LibmusPresetKey& key, double detune, const LibmusSettings& settings, double ticks_per_second)
{
    Zone zone;
    zone.exclusive_class = settings.early_form ? 0 : kChannelClass;

    // A key's pitch is key - kKeyOfNote + detune semitones above the output's rate.
    const int root = static_cast<int>(std::lround(kKeyOfNote - detune));
    zone.root_key = std::clamp(root, 0, 127);
    zone.tune_cents = 100.0 * (zone.root_key - kKeyOfNote + detune);

    // The lines between the bytes, on a scale of 127. A decay's count starts where the attack's stops, less the
    // attack's units.
    const auto [speed, first_byte, attack, peak, decay, sustain, release] = key.envelope;
    const double unit = settings.early_form ? 1.0 : std::max<int>(speed, 1);
    const int first = settings.early_form && attack < kShortestEarlyAttack ? peak : first_byte;
    const double top_ticks = std::max<int>(attack, 1) * unit;
    const double decay_end = std::max<int>(attack, 1) + std::max<int>(decay, 1) * unit;

    EnvelopeShape shape;
    shape.squared_lines = true;
    shape.points.push_back({0.0, first / 127.0});
    shape.points.push_back({top_ticks, peak / 127.0});
    if (decay_end > top_ticks)
    {
        shape.points.push_back({decay_end, sustain / 127.0});
    }

    const double level = FitEnvelope(zone, shape, ticks_per_second, 0.0);
    zone.release_ms = kSquaredLineTo100Db * std::max<int>(release, 1) * unit * 1000.0 / ticks_per_second;

    // Each byte counts out of 127, and the volumes' product over 2^13 counts out of 32768.
    const double full = 127.0 * 127.0 * 127.0 * settings.song_volume / static_cast<double>(1 << kVolumeShift) /
                        kFullVoice * (settings.master_volume / kFullVoice);
    zone.gain = level * full * full;

    return zone;
}

std::vector<uint8_t> UnpackDirectoryFile(std::span<const uint8_t> file)
{
    constexpr std::size_t kHeaderSize = 4;
    constexpr uint8_t kPacked = 1;
    constexpr int kItemsInGroup = 8;
    constexpr std::size_t kLongLength = 0x11;
    if (file.size() < kHeaderSize || file[0] != kPacked)
    {
        throw xsflib::FormatError("a file of the directory isn't packed");
    }

    std::size_t at = kHeaderSize;
    const auto next = [&]() -> uint8_t
    {
        if (at >= file.size())
        {
            throw xsflib::FormatError("a packed file stops before its end");
        }

        return file[at++];
    };

    const std::size_t size = std::size_t{file[1]} << 16 | std::size_t{file[2]} << 8 | file[3];
    std::vector<uint8_t> out;
    out.reserve(size);
    while (true)
    {
        const uint8_t flags = next();
        for (int item = kItemsInGroup - 1; item >= 0; item--)
        {
            if (((flags >> item) & 1) == 0)
            {
                out.push_back(next());
                continue;
            }

            const uint8_t first = next();
            const uint8_t second = next();
            const std::size_t distance = std::size_t{first & 0xF0U} << 4 | second;
            if (distance == 0)
            {
                if (out.size() != size)
                {
                    throw xsflib::FormatError("a packed file doesn't hold the size that it gives");
                }

                return out;
            }

            if (distance > out.size())
            {
                throw xsflib::FormatError("a packed file copies from before its start");
            }

            const std::size_t nibble = first & 0x0FU;
            const std::size_t length = nibble != 0 ? nibble + 1 : next() + kLongLength;
            for (std::size_t i = 0; i < length; i++)
            {
                const uint8_t copied = out[out.size() - distance];
                out.push_back(copied);
            }
        }
    }
}

std::unique_ptr<LibmusGame> LibmusGame::Open(std::span<const uint8_t> image, const LibmusProfile& profile)
{
    std::unique_ptr<LibmusGame> game(new LibmusGame);
    game->image_ = image;
    game->profile_ = &profile;

    // The game's bank and its songs, compressed, in a directory: the places of the wave table and the samples, the
    // count of songs and the place of each.
    std::optional<Bank> directory_bank;
    if (profile.directory != 0)
    {
        constexpr std::size_t kSongPlaces = 12;

        const std::span<const uint8_t> directory = image.subspan(profile.directory);
        const auto word = [&](std::size_t at)
        {
            return xsflib::LoadBe32(directory.data() + at);
        };

        directory_bank.emplace();
        directory_bank->unpacked_pointers = n64::DecodeYay0(directory.subspan(word(0)));
        directory_bank->pointers = directory_bank->unpacked_pointers;
        directory_bank->samples = directory.subspan(word(4));

        const uint32_t count = word(8);
        game->unpacked_songs_.reserve(count);
        for (uint32_t i = 0; i < count; i++)
        {
            game->unpacked_songs_.push_back(n64::DecodeYay0(directory.subspan(word(kSongPlaces + std::size_t{4} * i))));
            game->songs_.push_back(game->unpacked_songs_.back());
        }
    }

    for (std::size_t i = 0; i < profile.song_count; i++)
    {
        const std::size_t at = profile.songs + i * 8;
        const uint32_t start = xsflib::LoadBe32(image.data() + at);
        const uint32_t end = xsflib::LoadBe32(image.data() + at + 4);
        if (end <= start || end > image.size())
        {
            throw xsflib::FormatError("a song isn't in the image");
        }

        game->songs_.push_back(image.subspan(start, end - start));
    }

    // The game's table of songs names a packed file and a name for each.
    if (profile.files.directory != 0)
    {
        constexpr std::size_t kDirectoryEntry = 8;
        constexpr std::size_t kTableEntry = 8;

        const auto unpack = [&](std::size_t number)
        {
            const std::size_t at = profile.files.directory + number * kDirectoryEntry;
            const uint32_t start = xsflib::LoadBe32(image.data() + at);
            const uint32_t end = xsflib::LoadBe32(image.data() + at + 4);
            if (end <= start || end > image.size())
            {
                throw xsflib::FormatError("a file isn't in the image");
            }

            return UnpackDirectoryFile(image.subspan(start, end - start));
        };

        const std::vector<uint8_t> table_file = unpack(profile.files.table_file);
        for (std::size_t i = 0; i < profile.files.table_count; i++)
        {
            const std::size_t at = profile.files.table + i * kTableEntry;
            if (at + kTableEntry > table_file.size())
            {
                throw xsflib::FormatError("the game's table of songs isn't in its file");
            }

            game->unpacked_songs_.push_back(unpack(xsflib::LoadBe32(table_file.data() + at)));
            game->songs_.push_back(game->unpacked_songs_.back());

            const std::size_t name = xsflib::LoadBe32(table_file.data() + at + 4) - profile.files.file_address;
            if (name >= table_file.size())
            {
                throw xsflib::FormatError("a song's name isn't in the table's file");
            }

            const std::span<const uint8_t> rest = std::span(table_file).subspan(name);
            game->names_.emplace_back(rest.begin(), std::ranges::find(rest, uint8_t{0}));
        }
    }

    const auto read_waves = [&](Bank& bank)
    {
        // The table's header: a name, flags, the name of the sample file, the count, and three lists.
        constexpr std::size_t kCount = 0x20;
        constexpr std::size_t kBaseNotes = 0x24;
        constexpr std::size_t kDetunes = 0x28;
        constexpr std::size_t kWaveList = 0x2C;
        constexpr int kBaseOffset = 48;

        const std::span<const uint8_t> pointers = bank.pointers;
        const auto word = [&](std::size_t at)
        {
            return xsflib::LoadBe32(pointers.data() + at);
        };
        const uint32_t count = word(kCount);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t wave = word(word(kWaveList) + std::size_t{4} * i);
            BankWave entry;
            entry.base = word(wave);
            entry.length = word(wave + std::size_t{4});
            entry.vadpcm = pointers[wave + std::size_t{8}] == 0;
            entry.loop = word(wave + std::size_t{12});
            entry.book = word(wave + std::size_t{16});
            bank.waves.push_back(entry);

            const int cents = static_cast<int8_t>(pointers[word(kDetunes) + std::size_t{4} * i]);
            const int base = static_cast<int8_t>(static_cast<uint8_t>(pointers[word(kBaseNotes) + i] - kBaseOffset));
            bank.detunes.push_back(cents / 100.0 + base);
        }
    };

    if (directory_bank)
    {
        read_waves(*directory_bank);
        game->banks_.push_back(std::move(*directory_bank));
    }

    for (const LibmusBankPlace& place : profile.banks)
    {
        Bank bank;
        bank.first_song = place.first_song;
        bank.pointers = image.subspan(place.pointers, place.samples - place.pointers);
        bank.samples = image.subspan(place.samples);
        read_waves(bank);
        game->banks_.push_back(std::move(bank));
    }

    game->repeats_.assign(game->songs_.size(), false);
    for (std::size_t later = 1; later < game->songs_.size(); later++)
    {
        for (std::size_t earlier = 0; earlier < later && !game->repeats_[later]; earlier++)
        {
            game->repeats_[later] = game->Repeats(later, earlier);
        }
    }

    return game;
}

std::vector<ntdlib::sequence_export::Sequence> LibmusGame::Sequences() const
{
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (std::size_t i = 0; i < songs_.size(); i++)
    {
        if (!repeats_[i])
        {
            sequences.push_back({i, NameOf(i)});
        }
    }

    std::size_t beside = songs_.size();
    for (const LibmusTempoChange& change : profile_->tempo_changes)
    {
        if (change.beside != nullptr)
        {
            sequences.push_back({beside++, NameOf(change.song, change.beside)});
        }
    }

    return sequences;
}

std::string LibmusGame::NameOf(std::size_t index, const char* beside) const
{
    char base[32];
    std::snprintf(base, sizeof(base), "Sequence 0x%02zX", index);
    std::string name = base;
    if (index < names_.size())
    {
        name += " (" + names_[index] + (beside != nullptr ? std::string(", ") + beside : "") + ")";
    }
    else if (beside != nullptr)
    {
        name += std::string(" (") + beside + ")";
    }

    return name;
}

// A song repeats an earlier one if the two hold the same data, take the same bank and have the same entry in the game's
// table of volumes.
bool LibmusGame::Repeats(std::size_t later, std::size_t earlier)
{
    if (!std::ranges::equal(songs_[later], songs_[earlier]) || &BankOfSong(later) != &BankOfSong(earlier))
    {
        return false;
    }

    if (!profile_->song_volume_list.empty())
    {
        return profile_->song_volume_list[later] == profile_->song_volume_list[earlier];
    }

    if (profile_->song_volumes == 0)
    {
        return true;
    }

    const auto entry = [&](std::size_t song)
    {
        return image_.subspan(profile_->song_volumes + song * profile_->song_volume_stride,
                              profile_->song_volume_stride);
    };

    return std::ranges::equal(entry(later), entry(earlier));
}

LibmusGame::Bank& LibmusGame::BankOfSong(std::size_t index)
{
    Bank* found = &banks_.at(0);
    for (Bank& bank : banks_)
    {
        if (bank.first_song <= index)
        {
            found = &bank;
        }
    }

    return *found;
}

std::shared_ptr<const Wave> LibmusGame::WaveAt(Bank& bank, int32_t index) const
{
    const auto found = bank.decoded.find(index);
    if (found != bank.decoded.end())
    {
        return found->second;
    }

    const BankWave& wave = bank.waves.at(static_cast<std::size_t>(index));
    const std::span<const uint8_t> pointers = bank.pointers;
    const auto word = [&](std::size_t at)
    {
        return xsflib::LoadBe32(pointers.data() + at);
    };

    n64::VadpcmBook book;
    if (wave.vadpcm)
    {
        book.order = static_cast<int32_t>(word(wave.book));
        book.predictor_count = static_cast<int32_t>(word(wave.book + std::size_t{4}));
        const std::size_t values =
            std::size_t{8} * static_cast<std::size_t>(book.order) * static_cast<std::size_t>(book.predictor_count);
        for (std::size_t i = 0; i < values; i++)
        {
            book.coefficients.push_back(
                static_cast<int16_t>(xsflib::LoadBe16(pointers.data() + wave.book + std::size_t{8} + i * 2)));
        }
    }

    n64::BankLoop loop;
    if (wave.loop != 0)
    {
        loop.start = word(wave.loop);
        loop.end = word(wave.loop + std::size_t{4});
        loop.count = word(wave.loop + std::size_t{8});
        loop.has_state = wave.vadpcm;
        for (std::size_t i = 0; i < loop.state.size(); i++)
        {
            loop.state[i] =
                static_cast<int16_t>(xsflib::LoadBe16(pointers.data() + wave.loop + std::size_t{12} + i * 2));
        }
    }

    char name[32];
    std::snprintf(name, sizeof(name), "w%08X", wave.base);
    std::shared_ptr<const Wave> out =
        DecodeWave(bank.samples.subspan(wave.base, wave.length), wave.vadpcm ? &book : nullptr,
                   wave.loop != 0 ? &loop : nullptr, name, profile_->output_rate);
    bank.decoded[index] = out;

    return out;
}

Conversion LibmusGame::Convert(std::size_t index, const PerformOptions& options)
{
    // A version that the game's code plays beside the song comes after the songs. Any other change replaces the song.
    const LibmusTempoChange* change = nullptr;
    std::size_t beside = songs_.size();
    for (const LibmusTempoChange& candidate : profile_->tempo_changes)
    {
        if (candidate.beside != nullptr ? beside++ == index : candidate.song == index)
        {
            change = &candidate;
        }
    }

    const std::string name =
        NameOf(change != nullptr ? change->song : index, change != nullptr ? change->beside : nullptr);

    if (change != nullptr)
    {
        index = change->song;
    }

    LibmusSettings settings;
    if (change != nullptr)
    {
        settings.tempo_scale = change->scale;
    }

    if (profile_->song_volume != 0)
    {
        settings.song_volume = profile_->song_volume;
    }
    else
    {
        settings.song_volume = profile_->song_volume_list.empty()
                                   ? image_[profile_->song_volumes + index * profile_->song_volume_stride + 1]
                                   : profile_->song_volume_list[index];
    }

    settings.master_volume = profile_->master_volume;
    settings.output_rate = profile_->output_rate;
    settings.early_form = profile_->early_form;

    Bank& bank = BankOfSong(index);
    LibmusPerformance played = PerformLibmus(songs_.at(index), bank.detunes, settings, options);
    for (const LibmusPresetKey& key : played.presets)
    {
        char preset_name[32];
        std::snprintf(preset_name, sizeof(preset_name), "w%d e%d-%d-%d-%d-%d", key.wave, key.envelope[1],
                      key.envelope[3], key.envelope[5], key.envelope[4], key.envelope[6]);

        Zone zone =
            LibmusZone(key, bank.detunes.at(static_cast<std::size_t>(key.wave)), settings, played.ticks_per_second);
        zone.wave = WaveAt(bank, key.wave);

        Instrument preset;
        preset.name = preset_name;
        preset.zones.push_back(std::move(zone));
        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), name, options.program);
}

} // namespace ntdlib::sequence_export::n64

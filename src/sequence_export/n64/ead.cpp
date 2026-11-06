// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/ead.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

#include "seqlib/midi/recorder.h"
#include "xsflib/bytes.h"

#include "ntdlib/n64/rom_data.h"
#include "ntdlib/n64/seq_file.h"
#include "ntdlib/n64/vadpcm.h"
#include "ntdlib/sequence_export/n64/envelope.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

namespace n64 = ntdlib::n64;
namespace midi = seqlib::midi;
using n64::EadEngine;
using n64::ScriptCommand;
using n64::ScriptLevel;
using n64::ScriptOp;

constexpr std::size_t kChannels = 16;
// The most layers that a channel holds in any game's build of the engines.
constexpr std::size_t kLayers = 6;

// The layers of a channel in the early engine.
constexpr std::size_t kEarlyLayers = 4;
constexpr int kStackDepth = 4;
constexpr int kTicksPerBeat = 48;

// How many commands a script may run in one tick before the performer takes it for a closed loop of commands without a
// delay. The game would hang there.
constexpr int kMostCommands = 4096;

// A script's note 39 plays a sample at its tuning. MIDI's key 60 stands for it.
constexpr int kKeyOffset = 21;
constexpr int kRootKey = 60;

// The programs of a channel's drums, of the later engine's sound effects and of the first of the engines' built-in
// waves.
constexpr int kEffectProgram = 0x7E;
constexpr int kDrumProgram = 0x7F;
constexpr int kFirstWaveProgram = 0x80;

// A layer's default for a note's gate, the defaults of a channel, and the tables of short notes.
constexpr int kDefaultGate = 0x80;
constexpr int kDefaultReleaseRate = 0x20;
constexpr int kDefaultVibratoRate = 0x800;
constexpr std::array<uint8_t, 16> kDefaultVelocities = {12, 25, 38, 51,  57,  64,  71,  76,
                                                        83, 89, 96, 102, 109, 115, 121, 127};
constexpr std::array<uint8_t, 16> kDefaultGates = {229, 203, 177, 151, 139, 126, 113, 100,
                                                   87,  74,  61,  48,  36,  23,  10,  0};

// The engines' default envelope, in the units of a bank's envelopes.
constexpr std::array<n64::EadEnvelope::Point, 3> kDefaultEnvelope = {
    {{4, 32000}, {1000, 32000}, {n64::kEnvelopeHang, 0}}};

// A channel's gain on its samples: a number with four bits below the point.
constexpr int kUnityGain = 16;

// The bit of a layer's flags, in Majora's Mask's build of the engine, that takes the channel's gain.
constexpr int kLayerUsesChannelGain = 0x40;

// The top of an envelope's scale.
constexpr double kEnvelopeTop = 32767.0;

// The highest resampling rate, against the output's rate.
constexpr double kHighestRate = 3.99992;

// An engine's scale of pitch: 127 steps make an octave. A vibrato's curve is a triangle of 64 steps that peaks at 120
// of them, and the depth counts out of 4096. Its position advances by the rate in each update, and 65536 is a period.
constexpr double kPitchScaleSteps = 127.0;
constexpr double kVibratoPeakSteps = 120.0;
constexpr double kVibratoDepthUnit = 4096.0;
constexpr double kVibratoPeriodUnits = 65536.0;

// The engine's tempos count bpm * 48. A tick falls due when the count reaches this much for each update of a frame,
// which the engine takes to last 16.713 ms.
constexpr double kTempoCountPerUpdate = 2880000.0 / 48.0 / 16.713;
constexpr double kEngineFrameSeconds = 0.016713;

// The special portamento's scale: the steps of its sweep are this over its note's delay, times its time.
constexpr double kSpecialPortamentoScale = 32512.0;

// A fine bend's scale in the later engine: 127 steps make two semitones.
constexpr double kFineBendSteps = 127.0 * 6.0;

// A later engine's longest note without a length, in ticks.
constexpr double kLongestUnmeasuredNote = 32766.0;

// What each row of the later engine's low-pass table does at 32006 Hz: where the table's 8 taps fall 3 dB below their
// gain at 0 Hz, and that gain. A two-pole filter stands in for the taps. The first row and the last pass everything
// below half the rate.
struct LaterLowPass
{
    double hz = 0.0;
    double gain = 1.0;
};

constexpr std::array<LaterLowPass, 16> kLaterLowPasses = {{{0.0, 1.0},
                                                           {1850.0, 1.0},
                                                           {1990.0, 1.0},
                                                           {2290.0, 1.0},
                                                           {2980.0, 0.951},
                                                           {4360.0, 0.854},
                                                           {5770.0, 0.707},
                                                           {6860.0, 0.670},
                                                           {7850.0, 0.640},
                                                           {8730.0, 0.610},
                                                           {9580.0, 0.639},
                                                           {10600.0, 0.624},
                                                           {11710.0, 0.602},
                                                           {12530.0, 0.603},
                                                           {13270.0, 0.670},
                                                           {0.0, 0.846}}};

// A note's amplitude when the velocity, the volumes and the envelope stand at their tops. Wave Race 64's code at
// 0x800BA758 multiplies the square of the velocity, without scaling it, by the pan. It writes the top 11 of 16 bits,
// and the mixer's unit is 32768. Mario Kart 64's code at 0x800BC7B0 multiplies a velocity's share of 1 by 4096, and the
// mixer's unit is 4096 times 16.
double FullScale(EadLevels levels)
{
    return levels == EadLevels::kEnvelopeSquared ? 127.0 * 127.0 / 32768.0 : 1.0;
}

bool IsLater(EadEngine engine)
{
    return engine == EadEngine::kLater || engine == EadEngine::kLaterMajora;
}

// How many frames a later engine's release takes from the top of the envelope, for each rate of its table. A rate of 0
// never falls.
double LaterReleaseFrames(int rate)
{
    constexpr std::array<double, 5> kShortest = {0.75, 0.66, 0.5, 0.33, 0.25};
    if (rate >= 251)
    {
        return kShortest[static_cast<std::size_t>(rate - 251)];
    }

    if (rate >= 128)
    {
        return 251.0 - rate;
    }

    if (rate >= 16)
    {
        return 4.0 * (143 - rate);
    }

    return rate >= 1 ? 60.0 * (23 - rate) : 0.0;
}

// How far a note's level falls in an update during its release, from a top of 1.
double ReleaseFall(EadEngine engine, int rate, int updates_per_frame)
{
    if (IsLater(engine))
    {
        const double frames = LaterReleaseFrames(rate);

        return frames > 0.0 ? 1.0 / (frames * updates_per_frame) : 0.0;
    }

    return engine == EadEngine::kEarly ? rate * 24.0 / kEnvelopeTop : rate * (3.0 / 1280.0) / updates_per_frame;
}

uint32_t Be16At(std::span<const uint8_t> bytes, std::size_t place)
{
    return place + 2 <= bytes.size() ? xsflib::LoadBe16(bytes.data() + place) : 0;
}

// The controller's value for a product of volumes. CC7 and CC11 act squared. A build that squares the volumes itself
// therefore takes them as they stand.
uint8_t ControllerFor(double volume, EadLevels levels)
{
    const double clamped = std::clamp(volume, 0.0, 1.0);

    return static_cast<uint8_t>(
        std::lround(127.0 * (levels == EadLevels::kVolumesSquared ? clamped : std::sqrt(clamped))));
}

struct Script
{
    uint32_t pc = 0;
    int depth = 0;
    std::array<uint32_t, kStackDepth> stack{};
    std::array<int, kStackDepth> loops{};
};

struct Portamento
{
    int mode = 0;   // the mode, with the special sweep's bit
    int target = 0; // the command's note, with the transpositions
    int time = 0;
    double steps = 0.0;
    double rate = 0.0; // the later engine's steps in each update
    double extent = 0.0;
};

struct Layer
{
    bool ignores_gain = false;
    bool used = false;
    bool enabled = false;
    bool finished = false;
    bool stopped = false;
    bool legato = false;
    bool no_drum_pan = false;
    Script script;
    int delay = 0;
    int duration = 0;
    int last_delay = 0;
    int short_delay = 0;
    int gate = kDefaultGate;
    int velocity = 0;
    int transposition = 0;
    int pan = 64;
    int program = -1;      // a program that the layer chose, or -1 for the channel's
    int program_bank = 0;  // the bank that the program was chosen in
    int envelope = -1;     // an envelope that the layer chose, as an offset in the sequence
    int release_rate = -1; // the release rate that goes with it
    double fine_bend = 0.0;
    Portamento portamento;

    // The note that sounds.
    bool sounding = false;
    bool alone = false; // the note's pitch moves apart from the channel's other notes
    uint8_t key = 0;
    uint8_t part = 0;
    uint8_t pan_value = 64;
    uint32_t note = 0;
    uint32_t preset = 0;
    int32_t sound = n64::kNoObject;
    double offset = 0.0; // semitones from the MIDI key to the note, before the channel's bend and the sweep
    double release_updates = 0.0;
};

struct Channel
{
    bool used = false;
    bool enabled = false;
    bool finished = false;
    bool stopped = false;
    bool large_notes = false;
    bool has_instrument = false;
    Script script;
    int delay = 0;
    int value = 0;
    int bank = 0;
    int program = -1;
    int program_bank = 0;
    int transposition = 0;
    double volume = 1.0;
    double volume_scale = 1.0;
    double frequency_scale = 1.0;
    uint8_t gain = 0; // a gain on the samples, in sixteenths
    int pan = 64;
    int pan_weight = 128;
    int reverb = 0;
    int envelope = -1;
    int release_rate = -1;
    int sustain = 0;
    int vibrato_rate = kDefaultVibratoRate;
    int vibrato_depth = 0;
    int vibrato_delay = 0;
    uint32_t dynamic_table = 0;
    uint32_t pointer = 0;
    int filter = -1; // the place in the sequence of the filter's taps, or -1
    std::array<int, 8> io{-1, -1, -1, -1, -1, -1, -1, -1};
    std::array<Layer, kLayers> layers;
};

// The program that the engine's lookup finds: past the end, the last one; without an instrument, the one before it.
// Returns -1 if there's none.
int FindProgram(const n64::EadBank& bank, int program)
{
    int chosen = std::min(program, static_cast<int>(bank.program_instruments.size()) - 1);
    while (chosen >= 0 && bank.program_instruments[static_cast<std::size_t>(chosen)] == n64::kNoObject)
    {
        chosen--;
    }

    return chosen;
}

// The later engine's lookup finds a program only if the bank holds it. Returns -1 otherwise.
int FindLaterProgram(const n64::EadBank& bank, int program)
{
    const std::size_t place = static_cast<std::size_t>(program);

    return place < bank.program_instruments.size() && bank.program_instruments[place] != n64::kNoObject ? program : -1;
}

// The sample that an instrument plays for a note, and a number that tells its sounds apart.
struct InstrumentSound
{
    const n64::EadTunedSample* tuned = nullptr;
    int32_t id = n64::kNoObject;
};

InstrumentSound SoundFor(const n64::EadBank& bank, int program, int note)
{
    const int32_t index = bank.program_instruments[static_cast<std::size_t>(program)];
    const n64::EadInstrument& instrument = bank.instruments[static_cast<std::size_t>(index)];
    if (note < instrument.normal_range_low)
    {
        return {&instrument.low, index * 3};
    }

    if (note <= instrument.normal_range_high)
    {
        return {&instrument.normal, index * 3 + 1};
    }

    return {&instrument.high, index * 3 + 2};
}

class Performer
{
public:
    Performer(const EadPerformSettings& settings, const PerformOptions& options)
        : settings_(settings),
          options_(options),
          bytes_(settings.sequence.begin(), settings.sequence.end()),
          bank_set_(settings.bank_set),
          velocities_(kDefaultVelocities.begin(), kDefaultVelocities.end()),
          gates_(kDefaultGates.begin(), kDefaultGates.end()),
          threshold_(static_cast<int>(settings.clock.updates_per_frame * kTempoCountPerUpdate)),
          later_(IsLater(settings.engine)),
          majora_(settings.engine == EadEngine::kLaterMajora)
    {
    }

    EadPerformance Run();

private:
    struct TempoSegment
    {
        uint32_t tick = 0;
        double seconds = 0.0;
        double bpm = 120.0;
    };

    ScriptCommand Read(uint32_t pc, ScriptLevel level, bool large_notes);
    bool Flow(const ScriptCommand& command, Script& script, int value);
    const n64::EadBank& BankOf(int number) const;
    double UpdatesPerTick() const;
    double SecondsAt(uint32_t tick) const;

    void InitChannel(std::size_t index);
    void DisableChannel(std::size_t index);
    void StartChannel(std::size_t index, uint32_t pc);
    void StartLayer(std::size_t channel_index, std::size_t layer_index, uint32_t pc);
    void FreeLayer(std::size_t channel_index, Layer& layer);
    void ChooseInstrument(Channel& channel, int program);
    void ChooseLayerInstrument(const Channel& channel, Layer& layer, int program);

    void RunSequence();
    void RunChannel(std::size_t index);
    void RunChannelCommand(std::size_t index, const ScriptCommand& command, bool& stop);
    void RunLayer(std::size_t channel_index, Layer& layer);
    bool RunLayerCommand(std::size_t channel_index, Layer& layer, const ScriptCommand& command);
    void PlayNote(std::size_t channel_index, Layer& layer, const ScriptCommand& command);
    void StartNote(std::size_t channel_index, Layer& layer, uint32_t preset, int number, double release_rate,
                   int32_t sound, bool drum);
    void ReleaseNote(std::size_t channel_index, Layer& layer);

    double LayerSemitones(const Channel& channel, const Layer& layer) const;
    uint8_t PanOf(const Channel& channel, const Layer& layer, bool drum) const;
    void SendPitches(std::size_t index);
    void SendPans(std::size_t index);
    void SendLevels(std::size_t index);
    void SendEveryLevel();
    void SendReverb(std::size_t index);
    void AdvanceTime();
    void SetVolume(int value);
    void SetTempo(int count);
    void StartSequence(int number);
    void RunFramesToTick();
    int ChannelRandom(int range) const;
    std::size_t LayerSlot(std::size_t low) const;
    void FinishLaterNote(Layer& layer, const n64::EadBank& bank, const n64::EadTunedSample& tuned, int pitch_note);

    uint64_t StateHash() const;
    void Tick();
    void SlideLoop();
    void Finish();

    // A script's load of another sequence into its bytes. It takes some frames, and a port then says it has finished.
    struct SlowLoad
    {
        int sequence = 0;
        uint32_t place = 0;
        std::size_t port = 0;
        int bytes_left = 0;
    };

    const EadPerformSettings& settings_;
    const PerformOptions& options_;
    std::vector<uint8_t> bytes_;
    std::map<uint32_t, uint8_t> written_;
    std::map<uint32_t, int> loaded_; // the sequences that scripts loaded, by their places
    std::map<int, int> filters_;     // the low-pass row that a script wrote at each place. Channels can share one.
    std::vector<SlowLoad> slow_loads_;
    std::vector<uint8_t> bank_set_;
    int sequence_number_ = -1; // the sequence that a script started, or -1 for the first
    int update_of_frame_ = 0;
    int tempo_count_ = 0;
    std::vector<uint8_t> velocities_;
    std::vector<uint8_t> gates_;
    int threshold_;
    bool later_;
    bool majora_;

    EadPerformance out_;
    midi::Recorder recorder_;
    std::array<midi::TrackOutput, kChannels> outputs_;
    std::array<Channel, kChannels> channels_;

    Script script_;
    bool enabled_ = true;
    int delay_ = 0;
    int value_ = 0;
    int variation_ = -1;
    int tempo_ = 0;        // bpm * 48
    int tempo_change_ = 0; // the later engine's change, on top of the tempo
    int transposition_ = 0;
    bool script_stopped_ = false;
    std::array<int, 8> ports_{-1, -1, -1, -1, -1, -1, -1, -1};

    // The state of the values that scripts read as random. The game takes them from the console's timer.
    uint32_t random_ = 0x2545F491;

    // The player's volume, and its fade in each update.
    double fade_ = 1.0;
    double fade_scale_ = 1.0;
    double fade_step_ = 0.0;
    int fade_updates_ = 0;
    int fade_timer_ = 0;
    int volume_state_ = 1;
    double update_carry_ = 0.0; // the part of an update left over from the ticks so far

    uint32_t tick_ = 0;
    uint32_t last_note_tick_ = 0;
    bool any_note_ = false;
    double last_release_end_ = 0.0;
    std::vector<TempoSegment> tempo_map_;
    std::unordered_map<uint64_t, uint32_t> seen_;
    std::optional<midi::Performance::Loop> loop_;
    std::optional<uint32_t> stop_tick_;
};

ScriptCommand Performer::Read(uint32_t pc, ScriptLevel level, bool large_notes)
{
    try
    {
        return n64::ReadScriptCommand(bytes_, pc, settings_.engine, level, large_notes);
    }
    catch (const std::exception&)
    {
        recorder_.Warn("a script ran off the end of its sequence");
        ScriptCommand end;
        end.offset = pc;
        end.next = pc;
        end.op = ScriptOp::kEnd;

        return end;
    }
}

// Runs a command that changes a script's flow, on any level. Returns false for any other command. The end of a script
// goes through the level's code when the script has nowhere to return to.
bool Performer::Flow(const ScriptCommand& command, Script& script, int value)
{
    const uint32_t target = static_cast<uint32_t>(command.args[0]);
    switch (command.op)
    {
    case ScriptOp::kEnd:
        if (script.depth == 0)
        {
            return false;
        }

        script.pc = script.stack[static_cast<std::size_t>(--script.depth)];
        return true;

    case ScriptOp::kCall:
        if (script.depth < kStackDepth)
        {
            script.stack[static_cast<std::size_t>(script.depth++)] = command.next;
            script.pc = target;
        }
        else
        {
            script.pc = command.next;
        }

        return true;

    case ScriptOp::kJump:
        script.pc = target;
        return true;

    case ScriptOp::kBranchIfZero:
        script.pc = value == 0 ? target : command.next;
        return true;

    case ScriptOp::kBranchIfNegative:
        script.pc = value < 0 ? target : command.next;
        return true;

    case ScriptOp::kBranchIfNotNegative:
        script.pc = value >= 0 ? target : command.next;
        return true;

    case ScriptOp::kLoop:
        if (script.depth < kStackDepth)
        {
            // A count of 0 wraps round through the engine's byte: 256 times.
            script.loops[static_cast<std::size_t>(script.depth)] = command.args[0] == 0 ? 256 : command.args[0];
            script.stack[static_cast<std::size_t>(script.depth++)] = command.next;
        }

        script.pc = command.next;
        return true;

    case ScriptOp::kLoopEnd:
        script.pc = command.next;
        if (script.depth > 0)
        {
            const std::size_t top = static_cast<std::size_t>(script.depth - 1);
            if (--script.loops[top] != 0)
            {
                script.pc = script.stack[top];
            }
            else
            {
                script.depth--;
            }
        }

        return true;

    case ScriptOp::kBreak:
        script.depth = std::max(script.depth - 1, 0);
        script.pc = command.next;
        return true;

    default:
        return false;
    }
}

const n64::EadBank& Performer::BankOf(int number) const
{
    return settings_.banks->at(number);
}

double Performer::UpdatesPerTick() const
{
    return static_cast<double>(threshold_) / std::clamp(tempo_ + tempo_change_, 1, threshold_);
}

// A value that a channel's script reads as random. A frame's scripts all read one value.
int Performer::ChannelRandom(int range) const
{
    return static_cast<int>(range == 0 ? random_ & 0xFFFF : random_ % static_cast<uint32_t>(range));
}

// The later engine's numbers of a channel's layers past its last stand for the first.
std::size_t Performer::LayerSlot(std::size_t low) const
{
    if (!later_)
    {
        return low & (kEarlyLayers - 1);
    }

    return low < settings_.channel_layers ? low : 0;
}

double Performer::SecondsAt(uint32_t tick) const
{
    const TempoSegment& last = tempo_map_.back();

    return last.seconds + (static_cast<double>(tick) - last.tick) * 60.0 / (last.bpm * kTicksPerBeat);
}

void Performer::SetTempo(int count)
{
    tempo_ = std::clamp(count, 1, threshold_);
    const double scale = settings_.clock.frame_rate * kEngineFrameSeconds;
    const double bpm = std::clamp(tempo_ + tempo_change_, 1, threshold_) * scale / kTicksPerBeat;
    const double seconds = tempo_map_.empty() ? 0.0 : SecondsAt(tick_);
    tempo_map_.push_back({tick_, seconds, bpm});
    recorder_.SetTempo(out_.performance, tick_, bpm);
}

void Performer::InitChannel(std::size_t index)
{
    if (channels_[index].used)
    {
        DisableChannel(index);
    }

    const std::array<Layer, kLayers> old_layers = channels_[index].layers;
    Channel channel;
    channel.used = true;
    channel.bank = bank_set_.empty() ? 0 : bank_set_.back();
    channel.program_bank = channel.bank;
    channel.layers = old_layers;
    channels_[index] = channel;
}

void Performer::FreeLayer(std::size_t channel_index, Layer& layer)
{
    ReleaseNote(channel_index, layer);
    layer.used = false;
    layer.enabled = false;
    layer.finished = true;
}

void Performer::DisableChannel(std::size_t index)
{
    Channel& channel = channels_[index];
    for (Layer& layer : channel.layers)
    {
        if (layer.used)
        {
            FreeLayer(index, layer);
        }
    }

    channel.enabled = false;
    channel.finished = true;
}

void Performer::StartChannel(std::size_t index, uint32_t pc)
{
    Channel& channel = channels_[index];
    if (!channel.used)
    {
        return;
    }

    for (Layer& layer : channel.layers)
    {
        if (layer.used)
        {
            FreeLayer(index, layer);
        }
    }

    channel.enabled = true;
    channel.finished = false;
    channel.stopped = channel.stopped && later_;
    channel.script = Script();
    channel.script.pc = pc;
    channel.delay = 0;
}

void Performer::StartLayer(std::size_t channel_index, std::size_t layer_index, uint32_t pc)
{
    Layer& layer = channels_[channel_index].layers[layer_index];
    ReleaseNote(channel_index, layer);

    // The fields of notes' delays stay as they were: the engine doesn't reset them.
    const int last_delay = layer.last_delay;
    const int short_delay = layer.short_delay;
    layer = Layer();
    layer.used = true;
    layer.enabled = true;
    layer.last_delay = last_delay;
    layer.short_delay = short_delay;
    layer.script.pc = pc;
    if (later_)
    {
        // The later engine gives a new layer the channel's envelope, for when the layer sets only its release.
        layer.envelope = channels_[channel_index].envelope;
    }
}

void Performer::ChooseInstrument(Channel& channel, int program)
{
    channel.has_instrument = true;
    if (program >= (later_ ? kEffectProgram : kDrumProgram))
    {
        channel.program = program;
        return;
    }

    const n64::EadBank& bank = BankOf(channel.bank);
    const int chosen = later_ ? FindLaterProgram(bank, program) : FindProgram(bank, program);
    channel.has_instrument = chosen >= 0;
    if (chosen >= 0)
    {
        channel.program = chosen;
        channel.program_bank = channel.bank;
        channel.envelope = -1;
        channel.release_rate = -1;
    }
}

void Performer::ChooseLayerInstrument(const Channel& channel, Layer& layer, int program)
{
    if (settings_.engine == EadEngine::kEarly && program >= kDrumProgram)
    {
        return;
    }

    if (program == 0xFF)
    {
        layer.program = -1;
        layer.release_rate = -1;
        layer.envelope = -1;
        return;
    }

    if (program >= (later_ ? kEffectProgram : kDrumProgram))
    {
        layer.program = program;
        return;
    }

    const n64::EadBank& bank = BankOf(channel.bank);
    const int chosen = later_ ? FindLaterProgram(bank, program) : FindProgram(bank, program);
    if (chosen >= 0)
    {
        layer.program = chosen;
        layer.program_bank = channel.bank;
        layer.envelope = -1;
        layer.release_rate = -1;
    }
    else if (later_)
    {
        layer.program = -1;
    }
}

void Performer::ReleaseNote(std::size_t channel_index, Layer& layer)
{
    if (!layer.sounding)
    {
        return;
    }

    recorder_.NoteOff(out_.performance, static_cast<uint8_t>(channel_index), tick_, layer.part, layer.key, layer.note);
    last_release_end_ = std::max(
        last_release_end_,
        SecondsAt(tick_) + layer.release_updates / (settings_.clock.updates_per_frame * settings_.clock.frame_rate));
    layer.sounding = false;
    layer.alone = false;
    layer.sound = n64::kNoObject;
}

double Performer::LayerSemitones(const Channel& channel, const Layer& layer) const
{
    double semitones = 12.0 * std::log2(std::max(channel.frequency_scale, 1.0 / 65536.0)) + layer.offset;
    if (layer.portamento.mode != 0)
    {
        const double steps = std::min(layer.portamento.steps, kPitchScaleSteps);
        const double scale = 1.0 + layer.portamento.extent * (std::pow(2.0, steps / kPitchScaleSteps) - 1.0);
        semitones += 12.0 * std::log2(std::max(scale, 1.0 / 65536.0));
    }

    return semitones;
}

uint8_t Performer::PanOf(const Channel& channel, const Layer& layer, bool drum) const
{
    // A drum's pan goes in its zone. The rest of the position comes through the controller.
    const double layer_pan = drum && !layer.no_drum_pan ? 64.0 : layer.pan;
    const double weight = channel.pan_weight / 128.0;
    const double pan = layer_pan * (1.0 - weight) + channel.pan * weight;

    return static_cast<uint8_t>(std::clamp<long>(std::lround(pan), 0, 127));
}

void Performer::SendPitches(std::size_t index)
{
    const Channel& channel = channels_[index];
    for (const Layer& layer : channel.layers)
    {
        if (layer.sounding)
        {
            recorder_.SendPitch(out_.performance, outputs_[index], tick_,
                                static_cast<float>(LayerSemitones(channel, layer)), layer.part);
        }
    }
}

void Performer::SendPans(std::size_t index)
{
    Channel& channel = channels_[index];
    for (Layer& layer : channel.layers)
    {
        if (layer.sounding)
        {
            layer.pan_value = PanOf(channel, layer, channel.program == kDrumProgram);
            recorder_.SendSetting(out_.performance, outputs_[index], tick_, midi::EventKind::kPan, layer.pan_value,
                                  layer.part);
        }
    }
}

void Performer::SendReverb(std::size_t index)
{
    const Channel& channel = channels_[index];
    for (const Layer& layer : channel.layers)
    {
        if (layer.sounding)
        {
            recorder_.SendSetting(out_.performance, outputs_[index], tick_, midi::EventKind::kReverb,
                                  static_cast<uint8_t>(std::clamp(channel.reverb, 0, 127)), layer.part);
        }
    }
}

void Performer::SendLevels(std::size_t index)
{
    const Channel& channel = channels_[index];
    const uint8_t volume = ControllerFor(channel.volume, settings_.levels);
    const uint8_t expression = ControllerFor(channel.volume_scale * fade_ * fade_scale_, settings_.levels);
    for (const Layer& layer : channel.layers)
    {
        if (layer.sounding)
        {
            recorder_.SendSetting(out_.performance, outputs_[index], tick_, midi::EventKind::kVolume, volume,
                                  layer.part);
            recorder_.SendSetting(out_.performance, outputs_[index], tick_, midi::EventKind::kExpression, expression,
                                  layer.part);
        }
    }
}

void Performer::SendEveryLevel()
{
    for (std::size_t index = 0; index < kChannels; index++)
    {
        if (channels_[index].used)
        {
            SendLevels(index);
        }
    }
}

void Performer::StartNote(std::size_t channel_index, Layer& layer, uint32_t preset, int number, double release_rate,
                          int32_t sound, bool drum)
{
    Channel& channel = channels_[channel_index];
    midi::TrackOutput& output = outputs_[channel_index];
    Performance& performance = out_.performance;

    const int full_key = number + kKeyOffset;
    layer.key = static_cast<uint8_t>(std::clamp(full_key, 0, 127));
    layer.offset = full_key - layer.key + layer.fine_bend;
    layer.preset = preset;
    layer.sound = sound;
    layer.alone = layer.legato || layer.portamento.mode != 0 || layer.offset != 0.0;
    layer.pan_value = PanOf(channel, layer, drum);

    // A part holds one set of settings. A note that needs others from those of a note on the part goes on a free part.
    const double semitones = LayerSemitones(channel, layer);
    const auto apart = [&](const Layer& other)
    {
        return other.sounding && other.part == output.part && &other != &layer &&
               (other.alone || layer.alone || other.preset != preset || other.pan_value != layer.pan_value);
    };

    if (std::ranges::any_of(channel.layers, apart))
    {
        const bool moved =
            recorder_.Switch(performance, output, tick_,
                             [&](uint8_t part)
                             {
                                 return std::ranges::any_of(channel.layers, [&](const Layer& other)
                                                            { return other.sounding && other.part == part; });
                             });
        if (!moved)
        {
            recorder_.Approximate("layers of a channel with different settings share a part");
        }
    }

    layer.part = output.part;
    layer.sounding = true;
    recorder_.SendPreset(performance, output, tick_, preset);
    recorder_.SendSetting(performance, output, tick_, midi::EventKind::kVolume,
                          ControllerFor(channel.volume, settings_.levels));
    recorder_.SendSetting(performance, output, tick_, midi::EventKind::kExpression,
                          ControllerFor(channel.volume_scale * fade_ * fade_scale_, settings_.levels));
    recorder_.SendSetting(performance, output, tick_, midi::EventKind::kPan, layer.pan_value);
    recorder_.SendSetting(performance, output, tick_, midi::EventKind::kReverb,
                          static_cast<uint8_t>(std::clamp(channel.reverb, 0, 127)));
    recorder_.SendPitch(performance, output, tick_, static_cast<float>(semitones));
    if (channel.volume > 1.0 || channel.volume_scale * fade_ * fade_scale_ > 1.0)
    {
        recorder_.Approximate("a channel's volume above full held at full");
    }

    layer.note = recorder_.NoteOn(performance, output, tick_, layer.key,
                                  static_cast<uint8_t>(std::clamp(layer.velocity, 1, 127)));
    last_note_tick_ = tick_;
    any_note_ = true;

    // How long the note takes to fall from the top of the envelope.
    layer.release_updates = release_rate > 0.0 ? 1.0 / release_rate : 0.0;
}

void Performer::PlayNote(std::size_t channel_index, Layer& layer, const ScriptCommand& command)
{
    Channel& channel = channels_[channel_index];
    int delay = layer.last_delay;
    const int form = command.opcode >> 6;
    if (channel.large_notes)
    {
        if (form != 2)
        {
            delay = command.args[0];
            layer.last_delay = delay;
        }

        layer.velocity = command.args[1];
        layer.gate = form == 1 ? 0 : command.args[2];
    }
    else if (form == 0)
    {
        delay = command.args[0];
        layer.last_delay = delay;
    }
    else if (form == 1)
    {
        delay = layer.short_delay;
    }

    layer.delay = delay;
    layer.duration = layer.gate * delay / 256;
    if ((!channel.has_instrument && (!later_ || layer.program < 0)) ||
        ((settings_.silent_channels >> channel_index) & 1) != 0)
    {
        layer.stopped = true;
        return;
    }

    const bool early = settings_.engine == EadEngine::kEarly;
    const int program = !early && layer.program >= 0 ? layer.program : channel.program;
    const int updates_per_frame = settings_.clock.updates_per_frame;
    EadPresetKey key;
    key.vibrato_rate = channel.vibrato_depth != 0 ? channel.vibrato_rate : 0;
    key.vibrato_depth = channel.vibrato_depth;
    key.vibrato_delay = channel.vibrato_depth != 0 ? channel.vibrato_delay : 0;
    if (later_ && program == kEffectProgram)
    {
        recorder_.Approximate("a note on a bank's sound effects left out");
        layer.stopped = true;
        return;
    }

    if (channel.filter >= 0)
    {
        const auto filter = filters_.find(channel.filter);
        key.low_pass = filter != filters_.end() ? filter->second : 0;
    }

    // The engine reads a gain below 1 as 1.
    key.sample_gain = !layer.ignores_gain && channel.gain > kUnityGain ? channel.gain : 0;

    const n64::EadTunedSample* tuned_sound = nullptr;
    const n64::EadBank* sound_bank = nullptr;

    int number = 0;
    int32_t sound = n64::kNoObject;
    double release = 0.0;
    if (program == kDrumProgram)
    {
        const n64::EadBank& bank = BankOf(channel.bank);
        number = (command.low_bits + channel.transposition + layer.transposition) & 0xFF;
        const int count = static_cast<int>(bank.drum_notes.size());
        if (number >= count)
        {
            if (!early || count == 0)
            {
                layer.stopped = true;
                return;
            }

            number = count - 1;
        }

        const int32_t drum = bank.drum_notes[static_cast<std::size_t>(number)];
        if (drum == n64::kNoObject)
        {
            layer.stopped = true;
            return;
        }

        sound = drum;
        release =
            ReleaseFall(settings_.engine, bank.drums[static_cast<std::size_t>(drum)].release_rate, updates_per_frame);
        tuned_sound = &bank.drums[static_cast<std::size_t>(drum)].sound;
        sound_bank = &bank;
        key.bank = channel.bank;
        key.program = -1;
        key.drum_pan_weight = layer.no_drum_pan ? 128 : channel.pan_weight;
    }
    else
    {
        number = (command.low_bits + transposition_ + channel.transposition + layer.transposition) & 0xFF;
        if (number >= 0x80)
        {
            layer.stopped = true;
            return;
        }

        const int chosen = layer.program >= 0 && layer.program < kDrumProgram ? layer.program : channel.program;
        if (chosen >= kFirstWaveProgram || chosen < 0)
        {
            recorder_.Approximate("a note on one of the engine's built-in waves left out");
            layer.stopped = true;
            return;
        }

        const int bank_number = layer.program >= 0 ? layer.program_bank : channel.program_bank;
        const n64::EadBank& bank = BankOf(bank_number);

        // A sweep plays the sample of the higher of its two notes.
        Portamento& portamento = layer.portamento;
        const int range_note = portamento.mode != 0 ? std::max(number, portamento.target) : number;
        const InstrumentSound found = SoundFor(bank, chosen, range_note);
        if (found.tuned->sample == n64::kNoObject)
        {
            layer.stopped = true;
            return;
        }

        sound = found.id;
        tuned_sound = found.tuned;
        sound_bank = &bank;
        const int32_t index = bank.program_instruments[static_cast<std::size_t>(chosen)];
        const int instrument_release = bank.instruments[static_cast<std::size_t>(index)].release_rate;
        key.bank = bank_number;
        key.program = chosen;
        if (later_ ? layer.release_rate > 0 : layer.envelope >= 0)
        {
            key.envelope = layer.envelope;
            key.release_rate = layer.release_rate;
        }
        else if (layer.program < 0)
        {
            key.envelope = channel.envelope;
            key.release_rate = channel.release_rate;
        }

        release = ReleaseFall(settings_.engine, key.release_rate >= 0 ? key.release_rate : instrument_release,
                              updates_per_frame);
    }

    const uint32_t preset = midi::PresetIndex(out_.presets, key);

    // A sweep starts at one of its two notes. The bend moves it to the other.
    int base = number;
    Portamento& portamento = layer.portamento;
    if (portamento.mode != 0 && program != kDrumProgram)
    {
        const int mode = portamento.mode & 0x7F;
        const double to_note = std::pow(2.0, (number - portamento.target) / 12.0);
        if (mode == 1 || mode == 3 || mode == 5)
        {
            base = portamento.target;
            portamento.extent = to_note - 1.0;
        }
        else
        {
            portamento.extent = 1.0 / to_note - 1.0;
        }

        portamento.steps = 0.0;
        if (mode == 5)
        {
            portamento.target = number;
        }
    }

    if (later_)
    {
        FinishLaterNote(layer, *sound_bank, *tuned_sound, program == kDrumProgram ? 39 : base);
    }

    // A legato layer keeps its note while the sound stays the same. Only the pitch moves.
    if (layer.legato && layer.sounding && layer.sound == sound)
    {
        layer.offset = base + kKeyOffset - layer.key;
        SendPitches(channel_index);
        return;
    }

    ReleaseNote(channel_index, layer);
    if (layer.velocity <= 0)
    {
        return;
    }

    StartNote(channel_index, layer, preset, base, release, sound, program == kDrumProgram);
}

// The later engine's rules for a note's length and its sweep. A note without a length plays until its sample ends. The
// sweep's speed depends on that length.
void Performer::FinishLaterNote(Layer& layer, const n64::EadBank& bank, const n64::EadTunedSample& tuned,
                                int pitch_note)
{
    const bool unmeasured = layer.delay == 0;
    if (unmeasured)
    {
        double time = 0.0;
        const n64::EadSample& sample = bank.samples.at(static_cast<std::size_t>(tuned.sample));
        if (sample.loop != n64::kNoObject)
        {
            time = bank.loops.at(static_cast<std::size_t>(sample.loop)).sample_end;
        }

        const double frequency = tuned.tuning * std::pow(2.0, (pitch_note - 39) / 12.0);
        time *= tempo_ * (60.0 * settings_.clock.updates_per_frame / settings_.clock.output_rate / threshold_);
        time = std::min(time / frequency, kLongestUnmeasuredNote);
        layer.duration = 0;
        layer.delay = static_cast<int>(time) + 1;
    }

    Portamento& portamento = layer.portamento;
    if (portamento.mode == 0)
    {
        return;
    }

    int speed = 0x7FFF;
    if ((portamento.mode & 0x80) != 0)
    {
        speed = tempo_ * 0x8000 / threshold_;
        if (layer.delay * portamento.time != 0)
        {
            speed = speed * 0x100 / (layer.delay * portamento.time);
        }
    }
    else
    {
        const int denominator = portamento.time * settings_.clock.updates_per_frame / 3;
        speed = denominator != 0 ? 0x20000 / denominator : 0x7FFF;
    }

    portamento.rate = std::clamp(speed, 1, 0x7FFF) / 256.0;
}

// Runs a layer's command. Returns true when the layer has what it needs for this tick.
bool Performer::RunLayerCommand(std::size_t channel_index, Layer& layer, const ScriptCommand& command)
{
    Channel& channel = channels_[channel_index];
    switch (command.op)
    {
    case ScriptOp::kLayerUnknownF0:
        if (settings_.engine == EadEngine::kLaterMajora)
        {
            constexpr int kOtherFlags = 0xFFFF ^ kLayerUsesChannelGain;
            layer.ignores_gain = layer.ignores_gain || (command.args[0] & kLayerUsesChannelGain) != 0;
            if ((command.args[0] & kOtherFlags) != 0)
            {
                recorder_.Approximate("a layer's flags that drop its channel's values left out");
            }
        }

        break;

    case ScriptOp::kShortVelocity:
        layer.velocity = command.args[0];
        break;

    case ScriptOp::kNotePan:
        layer.pan = command.args[0];
        break;

    case ScriptOp::kLayerTranspose:
        layer.transposition = command.args[0];
        break;

    case ScriptOp::kShortGate:
        layer.gate = command.args[0];
        break;

    case ScriptOp::kLegato:
    case ScriptOp::kNoLegato:
        layer.legato = command.op == ScriptOp::kLegato;
        ReleaseNote(channel_index, layer);
        break;

    case ScriptOp::kShortDelay:
        layer.short_delay = command.args[0];
        break;

    case ScriptOp::kLayerInstrument:
        ChooseLayerInstrument(channel, layer, command.args[0]);
        break;

    case ScriptOp::kPortamento:
        {
            const int target = (command.args[1] + channel.transposition + layer.transposition + transposition_) & 0xFF;
            layer.portamento.mode = command.args[0];
            layer.portamento.target = target >= 0x80 ? 0 : target;
            layer.portamento.time = command.args[2];
            break;
        }

    case ScriptOp::kNoPortamento:
        layer.portamento = Portamento();
        break;

    case ScriptOp::kNoDrumPan:
        layer.no_drum_pan = true;
        break;

    case ScriptOp::kLayerEnvelope:
        layer.envelope = command.args[0];
        layer.release_rate = command.args[1];
        break;

    case ScriptOp::kShortVelocityFromTable:
        layer.velocity = velocities_[command.low_bits & 0x0F];
        break;

    case ScriptOp::kShortGateFromTable:
        layer.gate = gates_[command.low_bits & 0x0F];
        break;

    case ScriptOp::kLayerReleaseRate:
        layer.release_rate = command.args[0];
        break;

    case ScriptOp::kLayerBendFine:
        layer.fine_bend = 12.0 * static_cast<int8_t>(command.args[0]) / kFineBendSteps;
        break;

    case ScriptOp::kLayerDelay:
        layer.delay = command.args[0];
        layer.stopped = true;
        ReleaseNote(channel_index, layer);

        // Majora's Mask reads on after a rest without a length.
        return !majora_ || layer.delay != 0;

    case ScriptOp::kNote:
        layer.stopped = false;
        PlayNote(channel_index, layer, command);
        if (layer.stopped)
        {
            ReleaseNote(channel_index, layer);
        }

        return true;

    default:
        break;
    }

    return false;
}

void Performer::RunLayer(std::size_t channel_index, Layer& layer)
{
    if (!layer.enabled)
    {
        return;
    }

    if (layer.delay > 1)
    {
        layer.delay--;
        if (!layer.stopped && layer.delay <= layer.duration)
        {
            ReleaseNote(channel_index, layer);
            layer.stopped = true;
        }

        return;
    }

    if (!layer.legato)
    {
        ReleaseNote(channel_index, layer);
    }

    const int mode = layer.portamento.mode & 0x7F;
    if (mode == 1 || mode == 2)
    {
        layer.portamento.mode = 0;
    }

    const bool large = channels_[channel_index].large_notes;
    for (int step = 0; step < kMostCommands; step++)
    {
        const ScriptCommand command = Read(layer.script.pc, ScriptLevel::kLayer, large);
        if (Flow(command, layer.script, 0))
        {
            continue;
        }

        if (command.op == ScriptOp::kEnd)
        {
            ReleaseNote(channel_index, layer);
            layer.enabled = false;
            layer.finished = true;
            return;
        }

        layer.script.pc = command.next;
        if (RunLayerCommand(channel_index, layer, command))
        {
            return;
        }
    }

    recorder_.Warn("a layer's script runs without a delay");
    layer.enabled = false;
}

void Performer::RunChannelCommand(std::size_t index, const ScriptCommand& command, bool& stop)
{
    Channel& channel = channels_[index];
    Script& script = channel.script;
    const int arg = command.args[0];
    const std::size_t low = command.low_bits;
    const auto table_entry = [&](uint32_t table)
    {
        return Be16At(bytes_, table + static_cast<uint32_t>(channel.value) * 2);
    };
    switch (command.op)
    {
    case ScriptOp::kDelay1:
        stop = true;
        break;

    case ScriptOp::kDelay:
        channel.delay = arg;
        stop = true;
        break;

    case ScriptOp::kChannelDelayShort:
        channel.delay = command.low_bits;
        stop = true;
        break;

    case ScriptOp::kHang:
    case ScriptOp::kStopChannel:
        channel.stopped = true;
        stop = true;
        break;

    case ScriptOp::kInstrument:
        ChooseInstrument(channel, arg);
        break;

    case ScriptOp::kFont:
    case ScriptOp::kFontInstrument:
        {
            // The numbers of the sequence's banks count from the end of its list.
            const int count = static_cast<int>(bank_set_.size());
            if (arg < count)
            {
                channel.bank = bank_set_[static_cast<std::size_t>(count - 1 - arg)];
            }
            else if (later_)
            {
                recorder_.Warn("a channel names a bank past the end of its sequence's list");
            }

            if (command.op == ScriptOp::kFontInstrument)
            {
                ChooseInstrument(channel, command.args[1]);
            }

            break;
        }

    case ScriptOp::kShortNotes:
        channel.large_notes = false;
        break;

    case ScriptOp::kLargeNotes:
        channel.large_notes = true;
        break;

    case ScriptOp::kChannelVolume:
        channel.volume = arg / 127.0;
        SendLevels(index);
        break;

    case ScriptOp::kVolumeExpression:
        channel.volume_scale = arg / 128.0;
        SendLevels(index);
        break;

    case ScriptOp::kFrequencyScale:
        channel.frequency_scale = arg / 32768.0;
        SendPitches(index);
        break;

    case ScriptOp::kBend:
        channel.frequency_scale = std::pow(2.0, arg / kPitchScaleSteps);
        SendPitches(index);
        break;

    case ScriptOp::kBendFine:
        channel.frequency_scale = std::pow(2.0, arg / kFineBendSteps);
        SendPitches(index);
        break;

    case ScriptOp::kPan:
        channel.pan = arg;
        SendPans(index);
        break;

    case ScriptOp::kPanWeight:
        channel.pan_weight = arg;
        SendPans(index);
        break;

    case ScriptOp::kParameters:
        channel.transposition = static_cast<int8_t>(command.bytes[3]);
        channel.pan = command.bytes[4];
        channel.pan_weight = command.bytes[5];
        channel.reverb = command.bytes[6];
        SendPans(index);
        SendReverb(index);
        break;

    case ScriptOp::kLoadParameters:
        if (static_cast<std::size_t>(arg) + 8 <= bytes_.size())
        {
            const uint8_t* parameters = bytes_.data() + arg;
            channel.transposition = static_cast<int8_t>(parameters[3]);
            channel.pan = parameters[4];
            channel.pan_weight = parameters[5];
            channel.reverb = parameters[6];
            SendPans(index);
            SendReverb(index);
        }

        break;

    case ScriptOp::kChannelTranspose:
        channel.transposition = arg;
        break;

    case ScriptOp::kEnvelope:
        channel.envelope = arg;
        break;

    case ScriptOp::kReleaseRate:
        channel.release_rate = arg;
        break;

    case ScriptOp::kSustain:
        channel.sustain = arg;
        if (arg != 0)
        {
            recorder_.Approximate("a channel's sustain after a note's release left out");
        }

        break;

    case ScriptOp::kVibratoDepth:
        channel.vibrato_depth = arg * 8;
        break;

    case ScriptOp::kVibratoRate:
        channel.vibrato_rate = arg * 32;
        break;

    case ScriptOp::kVibratoDepthGradient:
        // A note reaches the target over the time in args[2]. The LFO starts halfway through it.
        channel.vibrato_depth = command.args[1] * 8;
        channel.vibrato_delay = std::max(channel.vibrato_delay, command.args[2] * 8);
        break;

    case ScriptOp::kVibratoRateGradient:
        channel.vibrato_rate = command.args[1] * 32;
        break;

    case ScriptOp::kVibratoDelay:
        channel.vibrato_delay = arg * 16;
        break;

    case ScriptOp::kVibratoReset:
        channel.vibrato_depth = 0;
        channel.vibrato_rate = 0;
        channel.vibrato_delay = 0;
        channel.frequency_scale = 1.0;
        channel.sustain = later_ ? 0 : channel.sustain;
        channel.filter = -1;
        channel.gain = 0;
        SendPitches(index);
        break;

    case ScriptOp::kReverb:
        channel.reverb = arg;
        SendReverb(index);
        break;

    case ScriptOp::kDynamicTable:
        channel.dynamic_table = static_cast<uint32_t>(arg);
        break;

    case ScriptOp::kDynamicTableLookup:
        if (channel.value != -1)
        {
            if (majora_)
            {
                script.pc = table_entry(channel.dynamic_table);
            }
            else
            {
                channel.dynamic_table = table_entry(channel.dynamic_table);
            }
        }

        break;

    case ScriptOp::kLoadPointer:
        channel.pointer = static_cast<uint32_t>(arg);
        break;

    case ScriptOp::kStorePointer:
        if (static_cast<std::size_t>(arg) + 2 <= bytes_.size())
        {
            for (std::size_t i = 0; i < 2; i++)
            {
                const uint32_t place = static_cast<uint32_t>(arg) + static_cast<uint32_t>(i);
                bytes_[place] = static_cast<uint8_t>(channel.pointer >> (8 - i * 8));
                written_[place] = bytes_[place];
            }
        }

        break;

    case ScriptOp::kAddPointer:
        channel.pointer = (channel.pointer + static_cast<uint32_t>(arg)) & 0xFFFF;
        break;

    case ScriptOp::kPointerFromSequence:
        channel.pointer = Be16At(bytes_, static_cast<std::size_t>(arg + channel.value));
        break;

    case ScriptOp::kPointerToTable:
        channel.dynamic_table = channel.pointer;
        break;

    case ScriptOp::kTableToPointer:
        channel.pointer = table_entry(channel.dynamic_table);
        break;

    case ScriptOp::kTableValue:
        {
            const std::size_t place = static_cast<std::size_t>(channel.dynamic_table) +
                                      static_cast<std::size_t>(static_cast<int64_t>(channel.value));
            channel.value = place < bytes_.size() ? static_cast<int8_t>(bytes_[place]) : 0;
            break;
        }

    case ScriptOp::kRandom:
        if (later_)
        {
            channel.value = static_cast<int8_t>(ChannelRandom(arg));
            recorder_.Approximate("a script's random choices taken from a fixed generator");
        }
        break;

    case ScriptOp::kRandomToPointer:
        channel.pointer = static_cast<uint32_t>(ChannelRandom(arg));
        recorder_.Approximate("a script's random choices taken from a fixed generator");
        break;

    case ScriptOp::kRandomPointer:
        {
            recorder_.Approximate("a script's random choices taken from a fixed generator");
            const uint32_t sum = static_cast<uint32_t>(ChannelRandom(arg) + command.args[1]) & 0xFFFF;
            channel.pointer = (((sum / 0x100 + 0x80) << 8) | (sum % 0x100)) & 0xFFFF;
            break;
        }

    case ScriptOp::kRandomVelocity:
    case ScriptOp::kRandomGate:
        if (arg != 0)
        {
            recorder_.Approximate("a channel's random change of velocity or gate left out");
        }

        break;

    case ScriptOp::kFilter:
        channel.filter = arg;
        break;

    case ScriptOp::kFreeFilter:
        channel.filter = -1;
        break;

    case ScriptOp::kLoadFilter:
        if (channel.filter >= 0)
        {
            filters_[channel.filter] = (arg >> 4) & 0xF;
            if ((arg & 0xF) != 0)
            {
                recorder_.Approximate("a channel's high-pass filter left out");
            }
        }

        break;

    case ScriptOp::kGain:
        channel.gain = static_cast<uint8_t>(arg);
        break;

    case ScriptOp::kCombFilter:
        if (command.args[1] != 0)
        {
            recorder_.Approximate("a channel's comb filter left out");
        }

        break;

    case ScriptOp::kDynamicCall:
        if (channel.value != -1 && script.depth < kStackDepth)
        {
            script.stack[static_cast<std::size_t>(script.depth++)] = script.pc;
            script.pc = table_entry(channel.dynamic_table);
        }

        break;

    case ScriptOp::kLoadValue:
        channel.value = static_cast<int8_t>(arg);
        break;

    case ScriptOp::kAndValue:
        channel.value &= static_cast<int8_t>(arg);
        break;

    case ScriptOp::kSubtractValue:
        channel.value = static_cast<int8_t>(channel.value - static_cast<int8_t>(arg));
        break;

    case ScriptOp::kStoreToSequence:
        {
            const uint32_t place = static_cast<uint32_t>(command.args[1]);
            if (place < bytes_.size())
            {
                bytes_[place] = static_cast<uint8_t>(channel.value + arg);
                written_[place] = bytes_[place];
            }

            break;
        }

    case ScriptOp::kLoadFromSequence:
        {
            const std::size_t place = static_cast<std::size_t>(arg + channel.value);
            channel.value = place < bytes_.size() ? static_cast<int8_t>(bytes_[place]) : 0;
            break;
        }

    case ScriptOp::kTestLayer:
        {
            const Layer& layer = channel.layers[LayerSlot(low)];
            if (layer.used)
            {
                channel.value = layer.finished ? 1 : 0;
            }
            else if (settings_.engine != EadEngine::kEarly)
            {
                channel.value = -1;
            }

            break;
        }

    case ScriptOp::kWriteIo:
        channel.io[low & 7] = channel.value;
        break;

    case ScriptOp::kReadIo:
        channel.value = channel.io[low & 7];
        if ((low & 7) < (later_ ? 2U : 4U))
        {
            channel.io[low & 7] = -1;
        }

        break;

    case ScriptOp::kSubtractIo:
        channel.value =
            later_ ? static_cast<int8_t>(channel.value - channel.io[low & 7]) : channel.value - channel.io[low & 7];
        break;

    case ScriptOp::kWriteOtherChannel:
        channels_[low & 15].io[static_cast<std::size_t>(arg & 7)] = channel.value;
        break;

    case ScriptOp::kReadOtherChannel:
        channel.value = channels_[low & 15].io[static_cast<std::size_t>(arg & 7)];
        break;

    case ScriptOp::kStartLayer:
        StartLayer(index, LayerSlot(low), static_cast<uint32_t>(arg));
        break;

    case ScriptOp::kStartLayerRel:
        StartLayer(index, LayerSlot(low),
                   static_cast<uint32_t>(static_cast<int>(command.next) + static_cast<int16_t>(arg)));
        break;

    case ScriptOp::kDynamicStartLayer:
        if (channel.value != -1)
        {
            StartLayer(index, LayerSlot(low), table_entry(channel.dynamic_table));
        }

        break;

    case ScriptOp::kFreeLayer:
        if (channel.layers[LayerSlot(low)].used)
        {
            FreeLayer(index, channel.layers[LayerSlot(low)]);
        }

        break;

    case ScriptOp::kStartOtherChannel:
        StartChannel(low & 15, static_cast<uint32_t>(arg));
        break;

    // In the early engine, the command contains the channel number in its low bits. In the later engine, 0xCD takes
    // the number as its argument.
    case ScriptOp::kStopOtherChannel:
        DisableChannel(later_ ? static_cast<std::size_t>(arg) & 15 : low & 15);
        break;

    default:
        break;
    }
}

void Performer::RunChannel(std::size_t index)
{
    Channel& channel = channels_[index];
    if (!channel.enabled)
    {
        return;
    }

    if (!channel.stopped)
    {
        if (channel.delay != 0)
        {
            channel.delay--;
        }

        if (channel.delay == 0)
        {
            bool stop = false;
            int step = 0;
            for (; step < kMostCommands && !stop && channel.enabled; step++)
            {
                const ScriptCommand command = Read(channel.script.pc, ScriptLevel::kChannel, false);
                if (Flow(command, channel.script, channel.value))
                {
                    continue;
                }

                if (command.op == ScriptOp::kEnd)
                {
                    DisableChannel(index);
                    break;
                }

                channel.script.pc = command.next;
                RunChannelCommand(index, command, stop);
            }

            if (step == kMostCommands)
            {
                recorder_.Warn("a channel's script runs without a delay");
                channel.stopped = true;
            }
        }
    }

    for (Layer& layer : channel.layers)
    {
        if (layer.used)
        {
            RunLayer(index, layer);
        }
    }
}

void Performer::SetVolume(int value)
{
    const double target = value / 127.0;
    if (settings_.song_volume >= 0.0)
    {
        return;
    }

    if (settings_.engine == EadEngine::kEarly)
    {
        fade_ = target;
        SendEveryLevel();
        return;
    }

    if (volume_state_ == 2)
    {
        return;
    }

    if (volume_state_ == 1)
    {
        volume_state_ = 0;
        fade_ = 0.0;
    }

    fade_updates_ = fade_timer_;
    if (fade_timer_ != 0)
    {
        fade_step_ = (target - fade_) / fade_updates_;
    }
    else
    {
        fade_ = target;
        SendEveryLevel();
    }
}

void Performer::RunSequence()
{
    if (script_stopped_)
    {
        return;
    }

    if (delay_ > 1)
    {
        delay_--;
        return;
    }

    for (int step = 0; step < kMostCommands; step++)
    {
        const ScriptCommand command = Read(script_.pc, ScriptLevel::kSequence, false);
        if (Flow(command, script_, value_))
        {
            continue;
        }

        if (command.op == ScriptOp::kEnd)
        {
            for (std::size_t channel = 0; channel < kChannels; channel++)
            {
                if (channels_[channel].used)
                {
                    DisableChannel(channel);
                }
            }

            enabled_ = false;
            return;
        }

        script_.pc = command.next;
        const int arg = command.args[0];
        const std::size_t low = command.low_bits;
        switch (command.op)
        {
        case ScriptOp::kDelay1:
            delay_ = 1;
            return;

        case ScriptOp::kDelay:
            delay_ = arg;
            return;

        case ScriptOp::kSetTranspose:
            transposition_ = arg;
            break;

        case ScriptOp::kAddTranspose:
            transposition_ += arg;
            break;

        case ScriptOp::kTempo:
            SetTempo(arg * kTicksPerBeat);
            break;

        case ScriptOp::kAddTempo:
            if (later_)
            {
                tempo_change_ = static_cast<int8_t>(arg) * kTicksPerBeat;
                SetTempo(tempo_);
            }
            else
            {
                SetTempo(tempo_ + arg * kTicksPerBeat);
            }

            break;

        case ScriptOp::kVolume:
            SetVolume(arg);
            break;

        case ScriptOp::kAddVolume:
            if (settings_.song_volume < 0.0)
            {
                fade_ += arg / 127.0;
                SendEveryLevel();
            }

            break;

        case ScriptOp::kVolumeMode:
            if (arg <= 1)
            {
                if (volume_state_ != 2)
                {
                    fade_timer_ = command.args[1];
                    volume_state_ = arg;
                }
            }
            else if (arg == 2 && command.args[1] != 0)
            {
                fade_updates_ = command.args[1];
                volume_state_ = 2;
                fade_step_ = -fade_ / fade_updates_;
            }

            break;

        case ScriptOp::kVolumeScale:
            fade_scale_ = static_cast<int8_t>(arg) / 127.0;
            SendEveryLevel();
            break;

        case ScriptOp::kInitChannels:
            for (std::size_t channel = 0; channel < kChannels; channel++)
            {
                if (((arg >> channel) & 1) != 0)
                {
                    if (later_)
                    {
                        // The later engine sets up every channel when it starts a sequence.
                        channels_[channel].bank = bank_set_.empty() ? 0 : bank_set_.back();
                    }
                    else
                    {
                        InitChannel(channel);
                    }
                }
            }

            break;

        case ScriptOp::kDisableChannels:
            for (std::size_t channel = 0; channel < kChannels && !later_; channel++)
            {
                if (((arg >> channel) & 1) != 0 && channels_[channel].used)
                {
                    DisableChannel(channel);
                    channels_[channel].used = false;
                }
            }

            break;

        case ScriptOp::kShortVelocityTable:
        case ScriptOp::kShortGateTable:
            {
                std::vector<uint8_t>& table = command.op == ScriptOp::kShortVelocityTable ? velocities_ : gates_;
                const std::size_t place = static_cast<std::size_t>(arg);
                if (place + table.size() <= bytes_.size())
                {
                    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(place), table.size(), table.begin());
                }

                break;
            }

        case ScriptOp::kLoadValue:
            value_ = later_ ? static_cast<int8_t>(arg) : arg;
            break;

        case ScriptOp::kAndValue:
            value_ = later_ ? static_cast<int8_t>(value_ & arg) : value_ & arg;
            break;

        case ScriptOp::kSubtractValue:
            value_ = later_ ? static_cast<int8_t>(value_ - arg) : value_ - arg;
            break;

        case ScriptOp::kRandom:
            if (later_)
            {
                value_ = static_cast<int8_t>((random_ >> 2) % static_cast<uint32_t>(arg == 0 ? 256 : arg));
                recorder_.Approximate("a script's random choices taken from a fixed generator");
            }

            break;

        case ScriptOp::kStoreToSequence:
            {
                const uint32_t place = static_cast<uint32_t>(command.args[1]);
                if (place < bytes_.size())
                {
                    bytes_[place] = static_cast<uint8_t>(value_ + arg);
                    written_[place] = bytes_[place];
                }

                break;
            }

        case ScriptOp::kDynamicJumpTable:
            if (value_ != -1)
            {
                const uint32_t target = Be16At(bytes_, static_cast<uint32_t>(arg + value_ * 2));
                if (script_.depth < kStackDepth)
                {
                    script_.stack[static_cast<std::size_t>(script_.depth++)] = script_.pc;
                    script_.pc = target;
                }
            }

            break;

        case ScriptOp::kTestChannel:
            if (later_)
            {
                value_ = channels_[low & 15].enabled ? 0 : 1;
            }
            else if (channels_[low & 15].used)
            {
                value_ = channels_[low & 15].finished ? 1 : 0;
            }

            break;

        case ScriptOp::kReadIo:
            if (later_)
            {
                value_ = ports_[low & 7];
                if (low < 2)
                {
                    ports_[low] = -1;
                }
            }
            else
            {
                value_ = variation_;
            }

            break;

        case ScriptOp::kWriteIo:
            if (later_)
            {
                ports_[low & 7] = value_;
            }
            else
            {
                variation_ = value_;
            }

            break;

        case ScriptOp::kSubtractIo:
            value_ = later_ ? static_cast<int8_t>(value_ - ports_[low & 7]) : value_ - variation_;
            break;

        case ScriptOp::kStopOtherChannel:
            DisableChannel(low & 15);
            break;

        case ScriptOp::kStopSequence:
            script_stopped_ = true;
            return;

        case ScriptOp::kLoadSequence:
            if (static_cast<std::size_t>(arg) < settings_.sequences.size())
            {
                const std::size_t size = settings_.sequences[static_cast<std::size_t>(arg)].size();
                slow_loads_.push_back({arg, static_cast<uint32_t>(command.args[1]), low & 7,
                                       static_cast<int>((size + 0xF) & ~std::size_t{0xF})});
            }
            else
            {
                ports_[low & 7] = 0;
            }

            break;

        case ScriptOp::kRunSequence:
            // A script starts the next sequence on its player. One that names another player plays beside it.
            if (arg != 0xFF && arg != 0)
            {
                recorder_.Approximate("a sequence that a script starts on another player left out");
            }
            else if ((!majora_ || volume_state_ != 2) &&
                     static_cast<std::size_t>(command.args[1]) < settings_.sequences.size())
            {
                StartSequence(command.args[1]);
                return;
            }

            break;

        case ScriptOp::kStartChannel:
            StartChannel(low & 15, static_cast<uint32_t>(arg));
            break;

        case ScriptOp::kStartChannelRel:
            StartChannel(low & 15, static_cast<uint32_t>(static_cast<int>(command.next) + static_cast<int16_t>(arg)));
            break;

        default:
            break;
        }
    }

    recorder_.Warn("the sequence's script runs without a delay");
    enabled_ = false;
}

// A script starts another sequence on its player. Only the ports stay. Notes that sound go on to their releases.
void Performer::StartSequence(int number)
{
    const std::size_t place = static_cast<std::size_t>(number);
    const std::span<const uint8_t> sequence = settings_.sequences[place];
    for (std::size_t channel = 0; channel < kChannels; channel++)
    {
        DisableChannel(channel);
    }

    sequence_number_ = number;
    bytes_.assign(sequence.begin(), sequence.end());
    written_.clear();
    loaded_.clear();
    filters_.clear();
    slow_loads_.clear();
    bank_set_ = place < settings_.bank_sets.size() ? settings_.bank_sets[place] : std::vector<uint8_t>();
    std::ranges::copy(kDefaultVelocities, velocities_.begin());
    std::ranges::copy(kDefaultGates, gates_.begin());
    script_ = Script();
    script_stopped_ = false;
    delay_ = 0;
    transposition_ = 0;
    fade_ = 1.0;
    fade_step_ = 0.0;
    fade_updates_ = 0;
    fade_timer_ = 0;
    volume_state_ = 1;
    update_carry_ = 0.0;
    tempo_change_ = 0;
    tempo_count_ = 0;
    SetTempo(120 * kTicksPerBeat);
    for (std::size_t channel = 0; channel < kChannels; channel++)
    {
        InitChannel(channel);
    }
}

// The later engine's clock. Each update adds the tempo to a count, and a tick falls due when the count passes a limit.
// Loads move on at the start of each frame. What a script sees of a load depends on the frame that its tick falls in.
void Performer::RunFramesToTick()
{
    const int step = std::clamp(tempo_ + tempo_change_, 1, threshold_);
    while (tempo_count_ < threshold_)
    {
        if (update_of_frame_ == 0)
        {
            random_ ^= random_ << 13;
            random_ ^= random_ >> 17;
            random_ ^= random_ << 5;
            std::erase_if(slow_loads_,
                          [&](SlowLoad& load)
                          {
                              if (load.bytes_left > 0)
                              {
                                  load.bytes_left = load.bytes_left < 0x400 ? 0 : load.bytes_left - 0x400;
                                  return false;
                              }

                              const std::span<const uint8_t> source =
                                  settings_.sequences[static_cast<std::size_t>(load.sequence)];
                              const std::size_t count = load.place < bytes_.size()
                                                            ? std::min(source.size(), bytes_.size() - load.place)
                                                            : std::size_t{0};
                              std::copy_n(source.begin(), count, bytes_.begin() + load.place);
                              std::erase_if(written_, [&](const auto& entry)
                                            { return entry.first >= load.place && entry.first < load.place + count; });
                              loaded_[load.place] = load.sequence;
                              ports_[load.port] = 1;

                              return true;
                          });
        }

        update_of_frame_ = (update_of_frame_ + 1) % settings_.clock.updates_per_frame;
        tempo_count_ += step;
    }

    tempo_count_ -= threshold_;
}

// What goes on between ticks: the player's fade and the sweeps of notes, which run in each update.
void Performer::AdvanceTime()
{
    const double updates = UpdatesPerTick();

    // The fade moves on once in each whole update. A tick can take part of an update, and the part counts towards the
    // next tick.
    update_carry_ += updates;
    const int whole = static_cast<int>(update_carry_);
    update_carry_ -= whole;
    if (fade_updates_ > 0)
    {
        const int spent = std::min(fade_updates_, whole);
        fade_ = std::clamp(fade_ + fade_step_ * spent, 0.0, 1.0);
        fade_updates_ -= spent;
        SendEveryLevel();
        if (fade_updates_ == 0 && volume_state_ == 2)
        {
            enabled_ = false;
        }
    }

    for (std::size_t index = 0; index < kChannels; index++)
    {
        Channel& channel = channels_[index];
        bool moved = false;
        for (Layer& layer : channel.layers)
        {
            if (layer.sounding && layer.portamento.mode != 0 && layer.portamento.steps < kPitchScaleSteps)
            {
                const double time = std::max(layer.portamento.time, 1);
                if (later_)
                {
                    layer.portamento.steps += layer.portamento.rate * updates;
                }
                else
                {
                    layer.portamento.steps += (layer.portamento.mode & 0x80) != 0
                                                  ? kSpecialPortamentoScale / (std::max(layer.last_delay, 1) * time)
                                                  : kPitchScaleSteps / time * updates;
                }

                moved = true;
            }
        }

        if (moved)
        {
            SendPitches(index);
        }
    }
}

uint64_t Performer::StateHash() const
{
    uint64_t hash = 14695981039346656037ULL;
    const auto add = [&](int64_t value)
    {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ULL;
    };

    const auto add_script = [&](const Script& script)
    {
        add(script.pc);
        add(script.depth);
        for (int i = 0; i < script.depth && i < kStackDepth; i++)
        {
            add(script.stack[static_cast<std::size_t>(i)]);
            add(script.loops[static_cast<std::size_t>(i)]);
        }
    };

    add_script(script_);
    add(delay_);
    add(value_);
    add(variation_);
    add(tempo_);
    add(tempo_change_);
    add(transposition_);
    add(script_stopped_ ? 1 : 0);
    add(sequence_number_);
    for (const int port : ports_)
    {
        add(port);
    }

    for (const auto& [place, sequence] : loaded_)
    {
        add(place);
        add(sequence);
    }

    for (const auto& [place, row] : filters_)
    {
        add(place);
        add(row);
    }

    for (const SlowLoad& load : slow_loads_)
    {
        add(load.sequence);
        add(load.place);
        add(load.bytes_left);
    }

    add(std::lround(fade_ * 4096.0));
    add(std::lround(fade_scale_ * 4096.0));
    add(fade_updates_);
    for (const auto& [place, value] : written_)
    {
        add(place);
        add(value);
    }

    for (const Channel& channel : channels_)
    {
        add(channel.used ? 1 : 0);
        if (!channel.enabled)
        {
            continue;
        }

        add_script(channel.script);
        add(channel.stopped ? 1 : 0);
        add(channel.delay);
        add(channel.value);
        add(channel.bank);
        add(channel.program);
        add(channel.transposition);
        add(channel.pan);
        add(channel.pan_weight);
        add(channel.reverb);
        add(channel.envelope);
        add(channel.release_rate);
        add(channel.vibrato_rate);
        add(channel.vibrato_depth);
        add(channel.vibrato_delay);
        add(channel.dynamic_table);
        add(channel.pointer);
        add(channel.filter);
        add(channel.large_notes ? 1 : 0);
        add(std::lround(channel.volume * 4096.0));
        add(std::lround(channel.volume_scale * 4096.0));
        add(std::lround(channel.frequency_scale * 65536.0));
        for (const int port : channel.io)
        {
            add(port);
        }

        for (const Layer& layer : channel.layers)
        {
            if (!layer.enabled)
            {
                add(-2);
                continue;
            }

            add_script(layer.script);
            add(layer.delay);
            add(layer.duration);
            add(layer.last_delay);
            add(layer.short_delay);
            add(layer.gate);
            add(layer.velocity);
            add(layer.transposition);
            add(layer.pan);
            add(layer.program);
            add(layer.legato ? 1 : 0);
            add(layer.sounding ? 1 : 0);
            add(layer.key);
            add(layer.portamento.mode);
            add(layer.portamento.target);
            add(std::lround(std::min(layer.portamento.steps, kPitchScaleSteps) * 16.0));
        }
    }

    return hash;
}

void Performer::Tick()
{
    if (later_)
    {
        RunFramesToTick();
    }

    RunSequence();
    for (std::size_t channel = 0; channel < kChannels && enabled_; channel++)
    {
        if (channels_[channel].used)
        {
            RunChannel(channel);
        }
    }

    if (enabled_)
    {
        AdvanceTime();
    }
}

// A state repeats only after its first tick has run: the first tick sets up the channels. The notes of that tick play
// again at the end of each pass. A loop that ended after them would cut them off. The loop moves back for as long as
// the notes before its start match those before its end.
void Performer::SlideLoop()
{
    using Note = std::tuple<std::size_t, midi::EventKind, uint8_t>;
    std::map<uint32_t, std::vector<Note>> notes;
    for (std::size_t track = 0; track < kChannels; track++)
    {
        for (const midi::TrackEvent& event : out_.performance.tracks[track])
        {
            if (event.kind == midi::EventKind::kNoteOn || event.kind == midi::EventKind::kNoteOff)
            {
                notes[event.tick].emplace_back(track, event.kind, event.key);
            }
        }
    }

    for (std::vector<Note>& list : notes | std::views::values)
    {
        std::ranges::sort(list);
    }

    const auto notes_at = [&](uint32_t tick)
    {
        const auto found = notes.find(tick);

        return found != notes.end() ? found->second : std::vector<Note>();
    };

    while (loop_->start > 0 && notes_at(loop_->start - 1) == notes_at(loop_->end - 1))
    {
        loop_->start--;
        loop_->end--;
        tick_--;
    }
}

void Performer::Finish()
{
    Performance& performance = out_.performance;
    for (std::size_t index = 0; index < kChannels; index++)
    {
        for (Layer& layer : channels_[index].layers)
        {
            ReleaseNote(index, layer);
        }
    }

    if (loop_)
    {
        SlideLoop();

        // The next pass starts at the end of the last. What it plays stays out, and the notes stop there.
        midi::ClampNoteOffs(performance, tick_);
        midi::CutAt(performance, tick_);
        performance.loop = loop_;
        performance.loops = options_.loops;
        performance.end_tick = tick_;
        performance.seconds = SecondsAt(tick_);
    }
    else
    {
        // The file ends once the last release has ended.
        const TempoSegment& last = tempo_map_.back();
        const double seconds = std::max(last_release_end_, SecondsAt(tick_));
        const double ticks = (seconds - last.seconds) * last.bpm * kTicksPerBeat / 60.0 + last.tick;
        performance.end_tick = std::max(tick_, static_cast<uint32_t>(std::ceil(ticks)));
        performance.seconds = seconds;
    }

    performance.approximations = recorder_.Approximations();
    performance.warnings = recorder_.Warnings();
}

EadPerformance Performer::Run()
{
    Performance& performance = out_.performance;
    performance.timebase = kTicksPerBeat;
    for (std::size_t channel = 0; channel < kChannels; channel++)
    {
        outputs_[channel].midi_track = static_cast<uint8_t>(channel);
    }

    fade_ = settings_.song_volume >= 0.0 ? settings_.song_volume : 1.0;
    fade_scale_ = settings_.player_scale;
    tempo_change_ = settings_.tempo_change * kTicksPerBeat;
    variation_ = settings_.variation;
    std::ranges::copy(settings_.ports, ports_.begin());
    SetTempo(120 * kTicksPerBeat);
    for (std::size_t channel = 0; channel < kChannels && later_; channel++)
    {
        InitChannel(channel);
    }

    while (enabled_ && (!stop_tick_ || tick_ < *stop_tick_))
    {
        if (SecondsAt(tick_) > kLongestSeconds)
        {
            performance.truncated = true;
            break;
        }

        Tick();
        tick_++;
        if (stop_tick_ || !enabled_)
        {
            continue;
        }

        // A script that waits for a load can pass ticks with nothing changed. The load moves on between them.
        if (!slow_loads_.empty())
        {
            continue;
        }

        const auto [found, is_new] = seen_.try_emplace(StateHash(), tick_);
        if (!is_new)
        {
            const uint32_t start = found->second;
            if (!any_note_ || last_note_tick_ < start)
            {
                // A loop without a note: the music has ended.
                break;
            }

            loop_ = midi::Performance::Loop{start, tick_};
            stop_tick_ = start + (tick_ - start) * static_cast<uint32_t>(std::max(options_.loops, 1));
        }
    }

    Finish();

    return std::move(out_);
}

// The straight lines of an envelope, from a level of 0, up to the end of the list. A list that goes back holds its last
// level there. The later engine scales each of an envelope's delays by its updates in a frame, against 4. The two
// builds in between keep the shortest delays as they stand. Pokémon Stadium 2's code at 0x8000E810 starts a note whose
// list opens with a skipped point at its first update's level, with no ramp from silence.
EnvelopeShape ShapeOf(std::span<const n64::EadEnvelope::Point> envelope, EadEngine engine, EadLevels levels,
                      int updates_per_frame, bool envelope_skip)
{
    EnvelopeShape shape;
    shape.squared_lines = levels != EadLevels::kVolumesSquared;
    shape.points.push_back({0.0, 0.0});
    double time = 0.0;
    bool start_at_target = false;
    for (const n64::EadEnvelope::Point& point : envelope)
    {
        if (envelope_skip && point.delay == n64::kEnvelopeSkip)
        {
            start_at_target = start_at_target || shape.points.size() == 1;
            continue;
        }

        if (point.delay <= 0)
        {
            shape.ends = point.delay == n64::kEnvelopeDisable;
            break;
        }

        const int delay = IsLater(engine) || (engine != EadEngine::kEarly && point.delay >= 4)
                              ? point.delay * updates_per_frame / 4
                              : point.delay;
        const double top = std::clamp(static_cast<double>(point.target) / kEnvelopeTop, 0.0, 1.0);
        const auto level_of = [&](double value)
        {
            return levels == EadLevels::kEarly ? value : value * value;
        };

        if (start_at_target && shape.points.size() == 1)
        {
            const double first = level_of(top / std::max(delay, 1));
            shape.points[0].level = first;
            if (delay > 1)
            {
                shape.points.push_back({1.0, first});
            }
        }

        time += std::max(delay, 1);
        shape.points.push_back({time, level_of(top)});
    }

    return shape;
}

// A vibrato's LFO. The engine's curve moves further up than down. The LFO takes the middle of the curve as the note's
// pitch, and half its span as its depth. Returns the shift in cents of that middle.
double FitVibrato(Lfo& lfo, const EadPresetKey& key, double updates_per_second, EadEngine engine, bool inverse_vibrato)
{
    lfo.hz = updates_per_second * key.vibrato_rate / kVibratoPeriodUnits;
    lfo.delay_ms = key.vibrato_delay * 1000.0 / updates_per_second;
    const double extent = key.vibrato_depth / kVibratoDepthUnit;
    if (inverse_vibrato)
    {
        // Pokémon Stadium 2's code at 0x8001BC5C: the scale is 65536 / (wave * depth + 65536), for a wave of up to
        // 32767 either way. The curve moves further up than down.
        const double up = -1200.0 * std::log2(std::max(1.0 - extent / 2.0, 1.0 / 64.0));
        const double down = -1200.0 * std::log2(1.0 + extent / 2.0);
        lfo.depth = (up - down) / 2.0;

        return (up + down) / 2.0;
    }

    if (IsLater(engine))
    {
        // The later engine's curve is a sine in the scale between one plus the depth and its inverse. Its middle stays
        // within a cent of the note for any depth that a sequence uses.
        lfo.depth = 1200.0 * std::log2(1.0 + extent);
        return 0.0;
    }

    const auto cents_at = [&](double steps)
    {
        return 1200.0 * std::log2(std::max(1.0 + extent * (std::pow(2.0, steps / kPitchScaleSteps) - 1.0), 1.0 / 64.0));
    };

    const double up = cents_at(kVibratoPeakSteps);
    const double down = cents_at(-kVibratoPeakSteps);
    lfo.depth = (up - down) / 2.0;

    return (up + down) / 2.0;
}

std::string SequenceName(std::size_t index, std::span<const SongName> names)
{
    char name[32];
    std::snprintf(name, sizeof(name), "Sequence 0x%02zX", index);
    for (const SongName& song : names)
    {
        if (song.index == index)
        {
            return std::string(name) + " (" + song.name + ")";
        }
    }

    return name;
}

} // namespace

EadPerformance PerformEad(const EadPerformSettings& settings, const PerformOptions& options)
{
    Performer performer(settings, options);

    return performer.Run();
}

std::vector<Zone> EadZones(const EadPresetKey& key, const n64::EadBank& bank, const EadPerformSettings& settings,
                           std::vector<int32_t>& sample_of_zone)
{
    std::vector<Zone> zones;
    sample_of_zone.clear();
    const int updates_per_frame = settings.clock.updates_per_frame;
    const double updates = updates_per_frame * settings.clock.frame_rate;
    const double rate_ratio = settings.clock.wave_rate / settings.clock.output_rate;
    const auto fade_of = [&](int rate)
    {
        return ReleaseFall(settings.engine, rate, updates_per_frame);
    };

    Lfo lfo;
    double vibrato_shift = 0.0;
    if (key.vibrato_depth != 0)
    {
        vibrato_shift = FitVibrato(lfo, key, updates, settings.engine, settings.inverse_vibrato);
    }

    const auto add = [&](int low_key, int high_key, const n64::EadTunedSample& tuned,
                         std::span<const n64::EadEnvelope::Point> envelope, int release_rate, double pan, bool drum)
    {
        if (tuned.sample == n64::kNoObject || tuned.tuning <= 0.0F || low_key > high_key)
        {
            return;
        }

        Zone zone;
        zone.low_key = low_key;
        zone.high_key = high_key;
        zone.root_key = drum ? low_key : kRootKey;
        zone.tune_cents = 1200.0 * std::log2(static_cast<double>(tuned.tuning)) + vibrato_shift;
        zone.follows_key = !drum;
        zone.pan = pan;
        if (key.vibrato_depth != 0)
        {
            zone.vibrato = lfo;
        }

        const EnvelopeShape shape =
            ShapeOf(envelope, settings.engine, settings.levels, updates_per_frame, settings.envelope_skip);
        const LaterLowPass& low_pass = kLaterLowPasses[static_cast<std::size_t>(key.low_pass & 0xF)];
        zone.low_pass_hz = low_pass.hz;
        zone.gain =
            FitEnvelope(zone, shape, updates, fade_of(release_rate)) * FullScale(settings.levels) * low_pass.gain;
        if (drum)
        {
            zones.push_back(zone);
            sample_of_zone.push_back(tuned.sample);
            return;
        }

        // Keys past the resampler's limit play at the limit.
        const double limit =
            kRootKey + 12.0 * std::log2(kHighestRate / (static_cast<double>(tuned.tuning) * rate_ratio));
        const int limit_key = static_cast<int>(std::ceil(limit));
        if (low_key < limit_key)
        {
            Zone below = zone;
            below.high_key = std::min(high_key, limit_key - 1);
            zones.push_back(below);
            sample_of_zone.push_back(tuned.sample);
        }

        if (high_key >= limit_key)
        {
            zone.low_key = std::max(low_key, limit_key);
            zone.follows_key = false;
            zone.tune_cents = 1200.0 * std::log2(kHighestRate / rate_ratio) + vibrato_shift;
            zones.push_back(zone);
            sample_of_zone.push_back(tuned.sample);
        }
    };

    if (key.program < 0)
    {
        for (std::size_t note = 0; note < bank.drum_notes.size() && note + kKeyOffset <= 127; note++)
        {
            const int32_t index = bank.drum_notes[note];
            if (index == n64::kNoObject)
            {
                continue;
            }

            const n64::EadDrum& drum = bank.drums[static_cast<std::size_t>(index)];
            if (drum.envelope == n64::kNoObject)
            {
                continue;
            }

            const double weight = key.drum_pan_weight / 128.0;
            const double pan = std::clamp((drum.pan / 64.0 - 1.0) * (1.0 - weight), -1.0, 1.0);
            const int midi_key = static_cast<int>(note) + kKeyOffset;
            add(midi_key, midi_key, drum.sound, bank.envelopes[static_cast<std::size_t>(drum.envelope)].points,
                drum.release_rate, pan, true);
        }

        return zones;
    }

    const n64::EadInstrument& instrument =
        bank.instruments[static_cast<std::size_t>(bank.program_instruments[static_cast<std::size_t>(key.program)])];
    std::vector<n64::EadEnvelope::Point> channel_envelope;
    if (key.envelope >= 0)
    {
        // A channel's envelope lies in the sequence.
        for (std::size_t place = static_cast<std::size_t>(key.envelope); place + 4 <= settings.sequence.size();
             place += 4)
        {
            const n64::EadEnvelope::Point point = {static_cast<int16_t>(Be16At(settings.sequence, place)),
                                                   static_cast<int16_t>(Be16At(settings.sequence, place + 2))};
            channel_envelope.push_back(point);
            if (point.delay <= 0)
            {
                break;
            }
        }
    }

    std::span<const n64::EadEnvelope::Point> envelope = channel_envelope;
    if (key.envelope < 0)
    {
        envelope = instrument.envelope != n64::kNoObject
                       ? std::span<const n64::EadEnvelope::Point>(
                             bank.envelopes[static_cast<std::size_t>(instrument.envelope)].points)
                       : std::span<const n64::EadEnvelope::Point>(kDefaultEnvelope);
    }

    int release = key.release_rate >= 0 ? key.release_rate : instrument.release_rate;
    if (release == 0 && !IsLater(settings.engine))
    {
        release = kDefaultReleaseRate;
    }

    const int top_key = 127;
    const int low_end = std::min(instrument.normal_range_low + kKeyOffset, top_key + 1);
    const int high_start = std::min(instrument.normal_range_high + 1 + kKeyOffset, top_key + 1);
    add(0, low_end - 1, instrument.low, envelope, release, 0.0, false);
    add(low_end, high_start - 1, instrument.normal, envelope, release, 0.0, false);
    add(high_start, top_key, instrument.high, envelope, release, 0.0, false);

    return zones;
}

std::unique_ptr<EadGame> EadGame::Open(std::span<const uint8_t> image, const EadProfile& profile)
{
    if (IsLater(profile.engine))
    {
        std::optional<n64::EadLaterFiles> files = n64::EadLaterFiles::Read(image);
        if (!files)
        {
            return nullptr;
        }

        std::unique_ptr<EadGame> game(new EadGame());
        game->image_ = image;
        game->profile_ = profile;
        game->later_files_ = std::move(files);
        game->bank_sets_ = game->later_files_->bank_sets;
        for (const std::vector<uint8_t>& sequence : game->later_files_->sequences)
        {
            game->sequences_.emplace_back(sequence);
        }

        return game;
    }

    const std::optional<n64::EarlySoundData> data = n64::FindEarlySoundData(image);
    if (!data)
    {
        return nullptr;
    }

    std::unique_ptr<EadGame> game(new EadGame());
    game->image_ = image;
    game->profile_ = profile;
    const auto entries = [&](std::size_t offset, uint16_t revision, std::vector<std::span<const uint8_t>>& out)
    {
        const n64::SeqFile file = n64::SeqFile::Parse(image.subspan(offset), revision);
        for (const n64::SeqFile::Entry& entry : file.Entries())
        {
            out.push_back(image.subspan(offset + entry.offset, entry.length));
        }
    };

    entries(data->sequences, n64::kEarlySequenceFileRevision, game->sequences_);
    entries(data->banks, n64::kEarlyBankFileRevision, game->bank_files_);
    entries(data->samples, n64::kEarlySampleFileRevision, game->sample_files_);
    if (data->bank_sets)
    {
        game->bank_sets_ = n64::ParseEarlyBankSets(image.subspan(*data->bank_sets), game->sequences_.size());
    }

    return game;
}

std::vector<ntdlib::sequence_export::Sequence> EadGame::Sequences() const
{
    // A track that the profile lists comes after the sequences in the numbering.
    std::vector<ntdlib::sequence_export::Sequence> list;
    for (std::size_t index = 0; index < sequences_.size(); index++)
    {
        if (std::ranges::find(profile_.effect_sequences, index) != profile_.effect_sequences.end() ||
            sequences_[index].empty())
        {
            continue;
        }

        // A later engine's table can name one sequence twice. Without a track of its own, the second adds nothing.
        const bool has_track =
            std::ranges::any_of(profile_.tracks, [&](const EadTrack& track) { return track.sequence == index; });
        const auto same_as = [&](std::size_t earlier)
        {
            return std::ranges::equal(sequences_[earlier], sequences_[index]) &&
                   bank_sets_[earlier] == bank_sets_[index];
        };
        if (!has_track && later_files_ && std::ranges::any_of(std::views::iota(std::size_t{0}, index), same_as))
        {
            continue;
        }

        if (!has_track)
        {
            list.push_back({index, SequenceName(index, profile_.names)});
        }

        for (std::size_t i = 0; i < profile_.tracks.size(); i++)
        {
            if (profile_.tracks[i].sequence == index)
            {
                const char* track_name = profile_.tracks[i].name;
                if (track_name != nullptr)
                {
                    const std::vector<SongName> name = {{index, track_name}};
                    list.push_back({sequences_.size() + i, SequenceName(index, name)});
                }
                else
                {
                    list.push_back({sequences_.size() + i, SequenceName(index, profile_.names)});
                }
            }
        }
    }

    return list;
}

const n64::EadBank& EadGame::BankAt(int number)
{
    const auto found = banks_.find(number);
    if (found != banks_.end())
    {
        return found->second;
    }

    if (later_files_)
    {
        return banks_.emplace(number, later_files_->banks.at(static_cast<std::size_t>(number))).first->second;
    }

    return banks_.emplace(number, n64::EadBank::ParseEarly(bank_files_.at(static_cast<std::size_t>(number))))
        .first->second;
}

std::shared_ptr<const Wave> EadGame::WaveAt(int bank_number, int32_t sample, int gain)
{
    if (gain != 0)
    {
        const auto key = std::make_tuple(bank_number, sample, gain);
        const auto found = gained_waves_.find(key);
        if (found != gained_waves_.end())
        {
            return found->second;
        }

        // The engine multiplies a note's samples by the gain and limits them to 16 bits.
        auto out = std::make_shared<Wave>(*WaveAt(bank_number, sample, 0));
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "g%02X", static_cast<unsigned>(gain));
        out->name += suffix;
        for (std::vector<int16_t>& channel : out->pcm.channels)
        {
            for (int16_t& value : channel)
            {
                value = static_cast<int16_t>(std::clamp((value * gain) >> 4, -32768, 32767));
            }
        }

        gained_waves_[key] = out;

        return out;
    }

    const auto key = std::make_tuple(bank_number, sample);
    const auto found = waves_.find(key);
    if (found != waves_.end())
    {
        return found->second;
    }

    const n64::EadBank& bank = BankAt(bank_number);
    const n64::EadSample& object = bank.samples.at(static_cast<std::size_t>(sample));
    std::span<const uint8_t> file;
    if (later_files_)
    {
        const std::array<uint32_t, 2>& places = later_files_->sample_places.at(static_cast<std::size_t>(bank_number));
        file = image_.subspan(places[object.medium < 2 ? object.medium : 0]);
    }
    else
    {
        file = sample_files_.at(static_cast<std::size_t>(bank_number));
    }

    const std::span<const uint8_t> bytes = file.subspan(object.address, object.size);
    const bool small = object.codec == n64::kEadCodecSmallAdpcm;
    const std::size_t frame_size = small ? n64::kVadpcmSmallFrameSize : n64::kVadpcmFrameSize;
    const auto decode = [&](std::span<const uint8_t> frames, const n64::VadpcmBook& book, n64::VadpcmState& state)
    {
        return small ? n64::DecodeVadpcmSmall(frames, book, state) : n64::DecodeVadpcm(frames, book, state);
    };

    auto out = std::make_shared<Wave>();
    char name[32];
    std::snprintf(name, sizeof(name), "b%02Xs%06X", static_cast<unsigned>(bank_number), object.address);
    out->name = name;
    out->rate = static_cast<uint32_t>(profile_.wave_rate);

    if (object.codec != n64::kEadCodecAdpcm && !small)
    {
        // The engines decode nothing else from an image, except signed bytes. Their other kinds play as silence.
        std::vector<int16_t> pcm;
        if (object.codec == n64::kEadCodecS8)
        {
            pcm.reserve(bytes.size());
            for (const uint8_t byte : bytes)
            {
                pcm.push_back(static_cast<int16_t>(static_cast<int8_t>(byte) * 256));
            }
        }

        if (pcm.empty())
        {
            pcm.assign(n64::kVadpcmFrameSamples, 0);
        }

        out->pcm.channels.push_back(std::move(pcm));
        waves_[key] = out;

        return out;
    }

    const n64::VadpcmBook& book = bank.books.at(static_cast<std::size_t>(object.book)).book;
    n64::VadpcmState start_state{};
    std::vector<int16_t> samples = decode(bytes, book, start_state);
    if (object.loop != n64::kNoObject)
    {
        const n64::BankLoop& loop = bank.loops.at(static_cast<std::size_t>(object.loop));
        if (loop.count != 0 && loop.end > loop.start && loop.end <= samples.size())
        {
            samples.resize(loop.end);
            out->pcm.loop = true;
            out->pcm.loop_start = loop.start;
            out->pcm.loop_end = loop.end;
            if (loop.has_state)
            {
                // A wave's repeats start from the state that the loop holds, at the frame of the loop's start. Where
                // they differ from the first pass, the file keeps a second pass to loop on.
                const std::size_t first_frame = loop.start / n64::kVadpcmFrameSamples;
                const std::size_t skip = loop.start - first_frame * n64::kVadpcmFrameSamples;
                const std::size_t length = loop.end - loop.start;
                n64::VadpcmState state = loop.state;
                const std::size_t next_place = (first_frame + 1) * frame_size;
                if (next_place <= bytes.size())
                {
                    const std::vector<int16_t> rest = decode(bytes.subspan(next_place), book, state);
                    std::vector<int16_t> repeat(loop.state.begin(), loop.state.end());
                    repeat.insert(repeat.end(), rest.begin(), rest.end());
                    if (repeat.size() >= skip + length &&
                        !std::equal(samples.begin() + static_cast<std::ptrdiff_t>(loop.start), samples.end(),
                                    repeat.begin() + static_cast<std::ptrdiff_t>(skip)))
                    {
                        samples.insert(samples.end(), repeat.begin() + static_cast<std::ptrdiff_t>(skip),
                                       repeat.begin() + static_cast<std::ptrdiff_t>(skip + length));
                        out->pcm.loop_start = loop.end;
                        out->pcm.loop_end = loop.end + static_cast<uint32_t>(length);
                    }
                }
            }
        }
        else if (loop.count == 0 && loop.end > 0 && loop.end < samples.size())
        {
            samples.resize(loop.end);
        }
    }

    out->pcm.channels.push_back(std::move(samples));
    waves_[key] = out;

    return out;
}

Conversion EadGame::Convert(std::size_t index, const PerformOptions& options)
{
    if (index >= sequences_.size())
    {
        const EadTrack& track = profile_.tracks[index - sequences_.size()];
        if (track.name == nullptr)
        {
            return ConvertTrack(track, SequenceName(track.sequence, profile_.names), options);
        }

        const std::vector<SongName> name = {{track.sequence, track.name}};

        return ConvertTrack(track, SequenceName(track.sequence, name), options);
    }

    return ConvertTrack({.sequence = index, .variation = profile_.variation}, SequenceName(index, profile_.names),
                        options);
}

Conversion EadGame::ConvertTrack(const EadTrack& track, const std::string& name, const PerformOptions& options)
{
    const std::size_t index = track.sequence;
    EadPerformSettings settings;
    settings.engine = profile_.engine;
    settings.levels = profile_.levels;
    settings.envelope_skip = profile_.envelope_skip;
    settings.channel_layers = profile_.channel_layers;
    settings.inverse_vibrato = profile_.inverse_vibrato;
    settings.sequence = sequences_.at(index);
    if (index < bank_sets_.size())
    {
        settings.bank_set = bank_sets_[index];
    }

    for (const uint8_t number : settings.bank_set)
    {
        BankAt(number);
    }

    settings.banks = &banks_;
    settings.clock.updates_per_frame = profile_.updates_per_frame;
    settings.clock.output_rate = profile_.output_rate;
    settings.clock.wave_rate = profile_.wave_rate;
    settings.player_scale = profile_.player_volume;
    settings.variation = track.variation;
    settings.silent_channels = track.silent_channels;
    settings.tempo_change = track.tempo_change;
    std::ranges::copy(track.ports, settings.ports.begin());
    settings.sequences = sequences_;
    settings.bank_sets = bank_sets_;
    if (later_files_)
    {
        // A script may start or load any other sequence.
        for (std::size_t bank = 0; bank < later_files_->banks.size(); bank++)
        {
            BankAt(static_cast<int>(bank));
        }
    }

    // The game's code scales the player by a level for the sequence.
    if (profile_.player_level_count != 0 && track.sequence >= profile_.first_player_level &&
        track.sequence - profile_.first_player_level < profile_.player_level_count)
    {
        constexpr int kHighestLevel = 127;
        int level = image_[profile_.player_levels + track.sequence - profile_.first_player_level];
        if (level > kHighestLevel)
        {
            level = image_[profile_.player_levels_indirect + static_cast<std::size_t>(level - kHighestLevel - 1)];
        }

        settings.player_scale = profile_.player_volume * level / kHighestLevel;
    }

    // A table in the game's code gives the song's volume, or the script does.
    if (index < profile_.song_volumes_count)
    {
        settings.song_volume = image_[profile_.song_volumes + index] / 127.0;
    }

    EadPerformance played = PerformEad(settings, options);
    for (const EadPresetKey& key : played.presets)
    {
        char preset_name[32];
        std::snprintf(preset_name, sizeof(preset_name), "b%d p%d", key.bank, key.program);

        Instrument preset;
        preset.name = preset_name;
        std::vector<int32_t> samples;
        preset.zones = EadZones(key, BankAt(key.bank), settings, samples);
        for (std::size_t i = 0; i < preset.zones.size(); i++)
        {
            preset.zones[i].wave = WaveAt(key.bank, samples[i], key.sample_gain);
        }

        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), name, options.program);
}

} // namespace ntdlib::sequence_export::n64

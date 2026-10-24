// SPDX-License-Identifier: MIT

// The model of the DSP's audio firmware (see firmware_model.h).

#include "ntdlib/ctr/dsp/firmware_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/memory_image.h"

#include "ctr/dsp/firmware_audio.h"
#include "ntdlib/ctr/dsp/device.h"
#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/dsp/fcram.h"

namespace ntdlib::ctr::dsp
{
namespace
{

// The firmware's times on Teakra, in DSP cycles. Teakra runs an instruction a cycle.

// LoadComponent: the glue waits for the boot's replies in two slices of 16,384 cycles. The boot starts the output's
// FIFO with 16 words of silence, and two of its samples come out before the boot ends.
constexpr uint64_t kBootCycles = 32768;
constexpr uint64_t kBootFifoStart = 22900;

// The glue's slices while the ARM11 waits for command register 2, and the most that it waits.
constexpr uint64_t kWaitSlice = 16384;
constexpr uint64_t kWaitLimit = 134'055'928;

// The output's FIFO holds 16 words, and sends a stereo sample every 4,096 cycles while it runs.
constexpr std::size_t kFifoWords = 16;

// From the FIFO's interrupt to the refill (0x3293). A refill that comes while the audio thread works takes 396 cycles
// from it.
constexpr uint64_t kRefillLatency = 141;
constexpr uint64_t kRefillCost = 396;

// From a value in command register 2 to the firmware's taking it.
constexpr uint64_t kCommandLatency = 300;

// The audio thread's start-up, from the Initialize command's taking: the output's FIFO stops, and starts again with 16
// words of silence (0x3373). The number of the structures and their addresses go out through the audio pipe (0x29a7).
constexpr uint64_t kFifoStop = 1148;
constexpr uint64_t kFifoStart = 1361;
constexpr uint64_t kCount = 17142;
constexpr uint64_t kAddresses = 283; // after the number

// From the ARM11's second 0x4000 to the thread's loop.
constexpr uint64_t kHandshakeLoop = 722;

// From the ARM11's 0x2000 to the semaphore's handler (0x333d): while the thread waits for the handshake, while it
// polls, and while it waits for the output's refill.
constexpr uint64_t kSemaphoreStartUp = 336;
constexpr uint64_t kSemaphorePoll = 491;
constexpr uint64_t kSemaphoreWait = 219;

// The poll (0x2c5d): its timer runs 10,000 cycles. It starts kPollArm cycles after the loop's start, and again
// kPollRearm cycles after the timer's interrupt. The poll ends kPollEnd cycles after the semaphore's handler.
constexpr uint64_t kPollTimer = 10000;
constexpr uint64_t kPollArm = 200;
constexpr uint64_t kPollRearm = 250;
constexpr uint64_t kPollEnd = 813;

// The polls after which the firmware sends the last frame's counter again (0x2c84).
constexpr uint16_t kPollsBeforeResend = 0x1a2e;

// The thread's loop (0x2c0e), from the output's refill that wakes it, with the refill's samples silent and not.
constexpr uint64_t kWakeLoopMuted = 887;
constexpr uint64_t kWakeLoopUnmuted = 915;

// From the loop's start to take_params (0x3076): forced and not, and from the poll's end.
constexpr uint64_t kForcedTakeParams = 150;
constexpr uint64_t kLoopTakeParams = 110;
constexpr uint64_t kPollTakeParams = 95;

// take_params with nothing to take, and the extra cycles for the mixer's parameters.
constexpr uint64_t kTakeParamsBase = 235;
constexpr uint64_t kTakeMixerParams = 346;

// From finish_frame (0x30e9) to the counter's store (0x30bf), from there to the interrupt, and from the interrupt to
// the next loop when the frame skips its mix, or to the mix (0x2e0d).
constexpr uint64_t kFinishFrame = 14425;
constexpr uint64_t kCounterInterrupt = 97;
constexpr uint64_t kSkipLoop = 45;
constexpr uint64_t kMixStart = 99;

// From the loop's start to the mix when the ARM11 has sent no new frame, and the time a mix takes. Neither shows to
// the ARM11.
constexpr uint64_t kIdleMix = 120;
constexpr uint64_t kMixCycles = 17000;

// The bits of the ARM11's semaphore that the firmware reads.
constexpr uint16_t kFrameSemaphore = 0x2000;
constexpr uint16_t kStartSemaphore = 0x4000;

// The audio pipe's entries in the firmware's pipe table: the DSP-to-ARM11 ring and the ARM11-to-DSP ring.
constexpr uint16_t kAudioToArm = 2 * static_cast<uint16_t>(Pipe::kAudio);
constexpr uint16_t kAudioToDsp = kAudioToArm + 1;

// The audio pipe's command that starts the audio thread.
constexpr uint8_t kInitializeCommand = 0;

// What the audio thread does next.
enum class Step : uint8_t
{
    kNone,          // the thread hasn't started
    kStopFifo,      // the output's start stops the FIFO
    kStartFifo,     // ... and starts it again
    kSendCount,     // sends the number of the structures
    kStartWait,     // waits for the ARM11's first 0x4000 before it
    kSendAddresses, // sends their addresses
    kHandshake,     // waits for the ARM11's second 0x4000
    kLoop,          // starts the loop
    kPoll,          // waits for the semaphore or the poll's timer
    kTakeParams,
    kFinishFrame,
    kWriteCounter,
    kInterrupt,
    kMix,
    kWait, // waits for the output's refill
};

// Whether the thread works towards step `step`, rather than waiting.
bool Working(Step step)
{
    return step != Step::kNone && step != Step::kStartWait && step != Step::kHandshake && step != Step::kPoll &&
           step != Step::kWait;
}

// A FirmwareModel's state as a DeviceState. The enhanced mode's sources come with it.
struct FirmwareModelState : DeviceState
{
    xsflib::MemoryImage ram;
    std::shared_ptr<const FirmwareModel::Schedule> schedule;
    std::shared_ptr<const firmware::HostSources> host;
};

} // namespace

struct FirmwareModel::Schedule
{
    uint64_t time = 0;   // DSP cycles since LoadComponent's reset
    uint64_t cycles = 0; // what Cycles returns
    bool running = false;

    // The output's FIFO: its words, whether it runs and when it sends its next sample, or the cycles since its last
    // sample while it's stopped. Its interrupt starts a refill once the output's start has set the handler.
    std::deque<int16_t> fifo;
    bool fifo_on = false;
    uint64_t fifo_next = 0;
    uint64_t fifo_stopped_timer = 0;
    bool refill_handler = false;
    std::optional<uint64_t> refill;

    // Command register 2: when the firmware takes its value, and the value.
    std::optional<uint64_t> command;
    uint16_t command_value = 0;

    // The audio pipe: the bytes waiting for the ARM11, and those from the ARM11 that the firmware hasn't read.
    std::deque<uint8_t> to_arm;
    std::deque<uint8_t> to_dsp;

    // The ARM11's semaphore, whether the firmware has set its handler, and when the handler runs.
    uint16_t semaphore = 0;
    bool semaphore_on = false;
    std::optional<uint64_t> semaphore_handler;

    // The audio thread: its next step and when, and its variables (0x2bd1).
    Step step = Step::kNone;
    uint64_t step_at = 0;
    bool first = false;
    bool skip = false;
    bool force = false;
    bool new_frame = false;
    uint16_t idle = 0;
    uint16_t polls = 0;
    bool wake = false;     // the refill has signalled the thread's event while the thread worked
    bool starting = false; // the handshake has ended, and the loop hasn't started

    // The poll's timer, and whether its handler wakes the thread.
    std::optional<uint64_t> poll_timer;
    bool poll_handler = false;
};

FirmwareModel::FirmwareModel(Fcram& fcram, bool enhanced)
    : ram_(kRamSize, 0),
      audio_(std::make_unique<firmware::Audio>(ram_.data() + kDataMemoryOffset, fcram, enhanced)),
      schedule_(std::make_unique<Schedule>())
{
}

FirmwareModel::~FirmwareModel() = default;

void FirmwareModel::SetSemaphore(uint16_t bits)
{
    Schedule& s = *schedule_;
    const uint16_t before = s.semaphore;
    s.semaphore |= bits;
    if ((bits & kStartSemaphore) && s.step == Step::kStartWait)
    {
        s.step = Step::kSendCount;
        s.step_at = s.time;
    }
    else if ((bits & kStartSemaphore) && s.step == Step::kHandshake)
    {
        s.semaphore &= ~kStartSemaphore;
        audio_->FinishStartUp();
        s.step = Step::kLoop;
        s.step_at = s.time + kHandshakeLoop;
        s.starting = true;
    }

    if ((bits & kFrameSemaphore) && !(before & kFrameSemaphore) && s.semaphore_on && !s.semaphore_handler)
    {
        uint64_t latency = kSemaphoreWait;
        if (s.step <= Step::kHandshake || s.starting)
        {
            latency = kSemaphoreStartUp;
        }
        else if (s.step == Step::kPoll)
        {
            latency = kSemaphorePoll;
        }

        s.semaphore_handler = s.time + latency;
    }
}

std::size_t FirmwareModel::PipeReadable(Pipe pipe) const
{
    return pipe == Pipe::kAudio ? schedule_->to_arm.size() : 0;
}

std::vector<uint8_t> FirmwareModel::ReadPipe(Pipe pipe, std::size_t size)
{
    const uint16_t count = static_cast<uint16_t>(size); // the dsp::DSP service's sizes are 16-bit
    if (count == 0)
    {
        return {};
    }

    Schedule& s = *schedule_;
    if (!s.running)
    {
        throw std::runtime_error("DSP: the pipes open once the model of the firmware has started");
    }

    if (pipe != Pipe::kAudio || s.to_arm.size() < count)
    {
        throw std::runtime_error("DSP: pipe " + std::to_string(static_cast<unsigned>(pipe)) + " holds fewer than " +
                                 std::to_string(count) + " bytes");
    }

    std::vector<uint8_t> data(s.to_arm.begin(), s.to_arm.begin() + count);
    s.to_arm.erase(s.to_arm.begin(), s.to_arm.begin() + count);
    SendCommand(kAudioToArm);

    return data;
}

void FirmwareModel::WritePipe(Pipe pipe, std::span<const uint8_t> data)
{
    if (data.empty())
    {
        return;
    }

    Schedule& s = *schedule_;
    if (!s.running)
    {
        throw std::runtime_error("DSP: the pipes open once the model of the firmware has started");
    }

    if (pipe != Pipe::kAudio)
    {
        throw std::runtime_error("DSP: the model of the audio firmware has no pipe " +
                                 std::to_string(static_cast<unsigned>(pipe)));
    }

    s.to_dsp.insert(s.to_dsp.end(), data.begin(), data.end());
    SendCommand(kAudioToDsp);
}

void FirmwareModel::SetInterruptHandler(InterruptHandler handler)
{
    interrupt_handler_ = std::move(handler);
}

void FirmwareModel::SetSampleSink(SampleSink sink)
{
    sample_sink_ = std::move(sink);
}

void FirmwareModel::SetFrameSink(FrameSink sink)
{
    frame_sink_ = std::move(sink);
}

bool FirmwareModel::Enhanced() const
{
    return audio_->Enhanced();
}

int FirmwareModel::SourceCount() const
{
    return audio_->SourceCount();
}

int FirmwareModel::MaxSources() const
{
    return audio_->Enhanced() ? kEnhancedSources : kSources;
}

void FirmwareModel::GrowSources(int count)
{
    audio_->GrowSources(count);
}

uint8_t* FirmwareModel::SourceStructure(int structure, int region)
{
    return audio_->SourceStructure(structure, region);
}

int32_t* FirmwareModel::AuxStructure(int region)
{
    return audio_->AuxStructure(region);
}

void FirmwareModel::LoadComponent(std::span<const uint8_t> /*image*/)
{
    Schedule& s = *schedule_;
    if (s.running)
    {
        return;
    }

    std::fill(ram_.begin(), ram_.end(), 0);
    const uint64_t cycles = s.cycles;
    s = Schedule{};
    s.cycles = cycles;
    audio_->Load();

    // The boot's FIFO: 16 words of silence, without an interrupt handler.
    s.fifo.assign(kFifoWords, 0);
    s.fifo_on = true;
    s.fifo_next = kBootFifoStart + kCyclesPerSample;
    RunTo(kBootCycles);
    s.cycles += kBootCycles;
    s.running = true;
}

uint8_t* FirmwareModel::Ram()
{
    return ram_.data();
}

void FirmwareModel::Run(uint64_t cycles)
{
    Schedule& s = *schedule_;
    if (s.running)
    {
        RunTo(s.time + cycles);
    }

    s.cycles += cycles;
}

uint64_t FirmwareModel::Cycles() const
{
    return schedule_->cycles;
}

std::shared_ptr<const DeviceState> FirmwareModel::SaveState(const DeviceState* previous)
{
    const auto* model_previous = dynamic_cast<const FirmwareModelState*>(previous);
    auto state = std::make_shared<FirmwareModelState>();
    state->ram = xsflib::MemoryImage(ram_.data(), ram_.size(), model_previous ? &model_previous->ram : nullptr);
    state->schedule = std::make_shared<const Schedule>(*schedule_);
    if (audio_->Enhanced())
    {
        state->host = std::make_shared<const firmware::HostSources>(audio_->Host());
    }

    return state;
}

void FirmwareModel::RestoreState(const DeviceState& state)
{
    const auto* model_state = dynamic_cast<const FirmwareModelState*>(&state);
    if (!model_state)
    {
        throw std::invalid_argument("DSP: a state from another kind of device");
    }

    if (model_state->ram.Size() != kRamSize)
    {
        throw std::invalid_argument("DSP: a snapshot without DSP RAM");
    }

    if (audio_->Enhanced() != (model_state->host != nullptr))
    {
        throw std::invalid_argument("DSP: a snapshot from another mixing mode");
    }

    model_state->ram.Restore(ram_.data());
    *schedule_ = *model_state->schedule;
    if (model_state->host)
    {
        audio_->RestoreHost(*model_state->host);
    }
}

void FirmwareModel::SetMeters(int count)
{
    audio_->SetMeters(count);
}

void FirmwareModel::SetSourceMeter(int source, int meter)
{
    audio_->SetSourceMeter(source, meter);
}

std::array<int32_t, 2> FirmwareModel::MeterPeaks(int meter) const
{
    return audio_->MeterPeaks(meter);
}

std::span<const int16_t> FirmwareModel::MixedFrame() const
{
    return audio_->MixedFrame();
}

// Runs the firmware's work up to DSP time `end`, in the order of its times.
void FirmwareModel::RunTo(uint64_t end)
{
    Schedule& s = *schedule_;
    while (true)
    {
        // The events in the order that the firmware takes them when they fall on the same cycle.
        enum class Event
        {
            kNone,
            kSample,
            kRefill,
            kCommand,
            kSemaphore,
            kTimer,
            kThread,
        };
        Event event = Event::kNone;
        uint64_t at = end + 1;
        const auto consider = [&](bool pending, uint64_t time, Event which)
        {
            if (pending && time < at)
            {
                at = time;
                event = which;
            }
        };

        consider(s.fifo_on, s.fifo_next, Event::kSample);
        consider(s.refill.has_value(), s.refill.value_or(0), Event::kRefill);
        consider(s.command.has_value(), s.command.value_or(0), Event::kCommand);
        consider(s.semaphore_handler.has_value(), s.semaphore_handler.value_or(0), Event::kSemaphore);
        consider(s.poll_timer.has_value(), s.poll_timer.value_or(0), Event::kTimer);
        consider(Working(s.step), s.step_at, Event::kThread);
        if (event == Event::kNone)
        {
            break;
        }

        s.time = at;
        switch (event)
        {
        case Event::kSample:
            Emit(at);
            break;

        case Event::kRefill:
            s.refill.reset();
            Refill(at);
            break;

        case Event::kCommand:
            s.command.reset();
            TakeCommand(at);
            break;

        case Event::kSemaphore:
            s.semaphore_handler.reset();
            HandleSemaphore(at);
            break;

        case Event::kTimer:
            s.poll_timer.reset();
            PollTimer(at);
            break;

        case Event::kThread:
            RunLoop(at);
            break;

        case Event::kNone:
            break;
        }
    }

    s.time = end;
}

// The FIFO sends a stereo sample. A word that it lacks goes out as 0. The pop that empties it starts the refill, once
// the firmware has set its handler. Teakra doesn't pass on a sample without a word either.
void FirmwareModel::Emit(uint64_t time)
{
    Schedule& s = *schedule_;
    std::array<int16_t, 2> sample = {0, 0};
    int missing = 0;
    for (int16_t& value : sample)
    {
        if (s.fifo.empty())
        {
            missing++;
            continue;
        }

        value = s.fifo.front();
        s.fifo.pop_front();
        if (s.fifo.empty() && s.refill_handler && !s.refill)
        {
            s.refill = time + kRefillLatency;
        }
    }

    s.fifo_next = time + kCyclesPerSample;
    if (sample_sink_ && missing < 2)
    {
        sample_sink_(sample);
    }
}

// The output's refill (0x3293): 16 words for the FIFO, and the thread's wake once a frame's samples have gone.
void FirmwareModel::Refill(uint64_t time)
{
    Schedule& s = *schedule_;
    const bool muted = audio_->Word(firmware::internal::kMute) != 0;
    std::array<int16_t, 16> words;
    const firmware::Audio::Refill refill = audio_->RefillOutput(words);
    if (refill.pushed)
    {
        for (const int16_t word : words)
        {
            if (s.fifo.size() < kFifoWords)
            {
                s.fifo.push_back(word);
            }
        }
    }

    // The refill takes its time from the thread's work.
    if (Working(s.step) && s.step_at > time)
    {
        s.step_at += kRefillCost;
    }

    if (!refill.wake)
    {
        return;
    }

    if (s.step == Step::kWait)
    {
        s.step = Step::kLoop;
        s.step_at = time + (muted ? kWakeLoopMuted : kWakeLoopUnmuted);
    }
    else
    {
        s.wake = true;
    }
}

// The firmware takes the value in command register 2. A notification of the audio pipe from the ARM11 holds its
// commands. The firmware starts the audio thread for the Initialize command.
void FirmwareModel::TakeCommand(uint64_t time)
{
    Schedule& s = *schedule_;
    if (s.command_value != kAudioToDsp)
    {
        return;
    }

    // A command takes four bytes.
    while (s.to_dsp.size() >= 4)
    {
        const uint8_t command = s.to_dsp.front();
        s.to_dsp.erase(s.to_dsp.begin(), s.to_dsp.begin() + 4);
        if (command == kInitializeCommand && s.step == Step::kNone)
        {
            audio_->StartThread();
            s.first = true;
            s.skip = true;
            s.idle = 0;
            s.step = Step::kStopFifo;
            s.step_at = time + kFifoStop;
        }
    }
}

void FirmwareModel::SendStartUp(bool addresses)
{
    Schedule& s = *schedule_;
    std::vector<uint8_t> bytes;
    if (!addresses)
    {
        bytes.resize(2);
        xsflib::StoreLe16(bytes.data(), static_cast<uint16_t>(firmware::kSharedStructures.size()));
    }
    else
    {
        bytes.resize(2 * firmware::kSharedStructures.size());
        for (std::size_t i = 0; i < firmware::kSharedStructures.size(); i++)
        {
            xsflib::StoreLe16(bytes.data() + 2 * i, firmware::kSharedStructures[i]);
        }
    }

    s.to_arm.insert(s.to_arm.end(), bytes.begin(), bytes.end());
    Raise(Interrupt::kPipe, Pipe::kAudio);
}

// The semaphore's handler (0x333d) takes the ARM11's frame counter, and wakes the thread from its poll.
void FirmwareModel::HandleSemaphore(uint64_t time)
{
    Schedule& s = *schedule_;
    if (!(s.semaphore & kFrameSemaphore))
    {
        return;
    }

    s.semaphore &= ~kFrameSemaphore;
    audio_->TakeArmCounter();
    if (audio_->Word(firmware::internal::kPolling) == 1 && s.step == Step::kPoll)
    {
        ResumeFromPoll(time);
    }
}

// The thread wakes in its poll: after the semaphore's handler, or the timer's.
void FirmwareModel::ResumeFromPoll(uint64_t time)
{
    Schedule& s = *schedule_;
    if (s.polls > kPollsBeforeResend)
    {
        audio_->StoreCounter(audio_->Word(firmware::internal::kLastCounter));
        Raise(Interrupt::kPipe, Pipe::kAudio);
        s.polls = 0;
    }
    else
    {
        s.polls++;
    }

    if (audio_->Word(firmware::internal::kArmCounter) == audio_->Word(firmware::internal::kCounter))
    {
        s.poll_timer = time + kPollRearm + kPollTimer;
        s.poll_handler = true;
        return;
    }

    audio_->SetWord(firmware::internal::kPolling, 0);
    s.poll_handler = false;
    s.step = Step::kTakeParams;
    s.step_at = time + kPollEnd + kPollTakeParams;
}

// The poll's timer runs out. Its handler wakes the thread while the thread polls. A timer that runs out after the
// poll's end does nothing.
void FirmwareModel::PollTimer(uint64_t time)
{
    const Schedule& s = *schedule_;
    if (s.poll_handler && s.step == Step::kPoll)
    {
        ResumeFromPoll(time);
    }
}

// The audio thread's next step.
void FirmwareModel::RunLoop(uint64_t time)
{
    Schedule& s = *schedule_;
    using firmware::internal::kArmCounter;
    using firmware::internal::kCounter;
    switch (s.step)
    {
    case Step::kStopFifo:
        s.fifo_stopped_timer = s.fifo_on ? kCyclesPerSample - (s.fifo_next - time) : s.fifo_stopped_timer;
        s.fifo_on = false;
        s.step = Step::kStartFifo;
        s.step_at = time - kFifoStop + kFifoStart;
        break;

    case Step::kStartFifo:
        s.fifo.assign(kFifoWords, 0);
        s.fifo_on = true;
        s.fifo_next = time + kCyclesPerSample - s.fifo_stopped_timer;
        s.refill_handler = true;
        s.semaphore_on = true;
        s.step = Step::kSendCount;
        s.step_at = time - kFifoStart + kCount;
        break;

    case Step::kSendCount:
        if (!(s.semaphore & kStartSemaphore))
        {
            s.step = Step::kStartWait;
            break;
        }

        s.semaphore &= ~kStartSemaphore;
        SendStartUp(false);
        s.step = Step::kSendAddresses;
        s.step_at = time + kAddresses;
        break;

    case Step::kSendAddresses:
        SendStartUp(true);
        if (s.semaphore & kStartSemaphore)
        {
            s.semaphore &= ~kStartSemaphore;
            audio_->FinishStartUp();
            s.step = Step::kLoop;
            s.step_at = time + kHandshakeLoop;
        }
        else
        {
            s.step = Step::kHandshake;
        }
        break;

    case Step::kLoop:
        {
            s.starting = false;
            const bool sync = audio_->Word(firmware::internal::kSyncMode) != 0;
            s.force = s.first || !sync || s.idle > 0x3f;
            s.new_frame = s.force || audio_->Word(kArmCounter) != audio_->Word(kCounter);
            if (s.force)
            {
                s.polls = 0;
                audio_->UpdateLate();
                if (audio_->Word(kArmCounter) == audio_->Word(kCounter))
                {
                    audio_->SetWord(firmware::internal::kPolling, 1);
                    s.poll_handler = true;
                    s.poll_timer = time + kPollArm + kPollTimer;
                    s.step = Step::kPoll;
                    break;
                }

                s.step = Step::kTakeParams;
                s.step_at = time + kForcedTakeParams;
                break;
            }

            if (s.new_frame)
            {
                s.step = Step::kTakeParams;
                s.step_at = time + kLoopTakeParams;
                break;
            }

            s.idle = static_cast<uint16_t>(std::min(s.idle + 1, 0x40));
            if (s.skip)
            {
                s.skip = false;
                s.step_at = time + kIdleMix;
                break;
            }

            s.step = Step::kMix;
            s.step_at = time + kIdleMix;
            break;
        }

    case Step::kTakeParams:
        {
            const int region = audio_->Word(kArmCounter) & 1;
            const bool mixer = audio_->Word(firmware::Shared(firmware::kMixerParams, region)) != 0 ||
                               audio_->Word(firmware::Shared(firmware::kMixerParams, region) + 1) != 0;
            audio_->TakeParams(region);
            s.step = Step::kFinishFrame;
            s.step_at = time + kTakeParamsBase + (mixer ? kTakeMixerParams : 0);
            break;
        }

    case Step::kFinishFrame:
        audio_->FinishFrame(audio_->Word(kCounter) & 1);
        s.step = Step::kWriteCounter;
        s.step_at = time + kFinishFrame;
        break;

    case Step::kWriteCounter:
        audio_->StoreCounter(audio_->Word(kCounter));
        s.step = Step::kInterrupt;
        s.step_at = time + kCounterInterrupt;
        break;

    case Step::kInterrupt:
        Raise(Interrupt::kPipe, Pipe::kAudio);
        audio_->SetWord(firmware::internal::kLastCounter, audio_->Word(kCounter));
        audio_->SetWord(kCounter, audio_->Word(kArmCounter));
        s.idle = 0;
        if (s.skip)
        {
            s.skip = false;
            s.step = Step::kLoop;
            s.step_at = time + kSkipLoop;
            break;
        }

        s.step = Step::kMix;
        s.step_at = time + kMixStart;
        break;

    case Step::kMix:
        audio_->MixFrame();
        if (audio_->Enhanced() && frame_sink_)
        {
            frame_sink_(audio_->EnhancedFrame());
        }

        audio_->AfterMix(s.force, s.first);
        if (s.wake)
        {
            s.wake = false;
            s.step = Step::kLoop;
            s.step_at = time + kMixCycles;
        }
        else
        {
            s.step = Step::kWait;
        }
        break;

    case Step::kNone:
    case Step::kStartWait:
    case Step::kHandshake:
    case Step::kPoll:
    case Step::kWait:
        break;
    }
}

// Sends `value` on command register 2 once the firmware has taken what's there. The ARM11 waits for the DSP meanwhile,
// in the glue's slices.
void FirmwareModel::SendCommand(uint16_t value)
{
    Schedule& s = *schedule_;
    uint64_t ran = 0;
    while (s.command)
    {
        if (ran >= kWaitLimit)
        {
            throw std::runtime_error("DSP: the firmware has stopped answering");
        }

        RunTo(s.time + kWaitSlice);
        s.cycles += kWaitSlice;
        ran += kWaitSlice;
    }

    s.command = s.time + kCommandLatency;
    s.command_value = value;
}

// Passes an interrupt to the handler, if there is one.
void FirmwareModel::Raise(Interrupt interrupt, Pipe pipe)
{
    if (!interrupt_handler_)
    {
        return;
    }

    // Copy the handler because it may replace itself through SetInterruptHandler.
    const InterruptHandler handler = interrupt_handler_;
    handler(interrupt, pipe);
}

} // namespace ntdlib::ctr::dsp

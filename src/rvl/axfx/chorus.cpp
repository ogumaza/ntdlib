// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/chorus.h"

#include <cstddef>

#include "ntdlib/fused.h"
#include "ntdlib/rvl/axfx/util.h"

namespace ntdlib::rvl::axfx
{
namespace
{

// The lines' length in samples: 100 ms.
constexpr uint32_t kLineSize = 0xc80;

// The LFO's table: half a period of a sine in 128 steps, close to sin(pi * n / 128) * 2^24. __AXFXGetLfoSinTable
// (0x8010a9c0; October 2006, 0x80034b14) returns its address: 0x803aeba0 in Wii Sports, 0x80344600 in Wii Play and
// 0x805335d0 in Smooth Moves. The three hold the same 128 words.
constexpr std::array<int32_t, 128> kLfoSine = {
    0,        411733,   823219,   1234208,  1644454,  2053710,  2461728,  2868264,  3273072,  3675908,  4076531,
    4474698,  4870168,  5262706,  5652074,  6038037,  6420363,  6798821,  7173184,  7543226,  7908725,  8269460,
    8625213,  8975771,  9320922,  9660459,  9994176,  10321874, 10643354, 10958422, 11266890, 11568571, 11863283,
    12150850, 12431098, 12703857, 12968963, 13226258, 13475586, 13716797, 13949746, 14174291, 14390299, 14597637,
    14796184, 14985817, 15166424, 15337895, 15500126, 15653022, 15796488, 15930440, 16054795, 16169479, 16274424,
    16369565, 16454847, 16530216, 16595628, 16651044, 16696429, 16731757, 16757007, 16772163, 16777216, 16772163,
    16757007, 16731757, 16696429, 16651043, 16595628, 16530216, 16454846, 16369565, 16274424, 16169479, 16054794,
    15930439, 15796488, 15653022, 15500126, 15337894, 15166424, 14985817, 14796183, 14597638, 14390298, 14174290,
    13949744, 13716797, 13475586, 13226257, 12968964, 12703856, 12431096, 12150848, 11863283, 11568570, 11266888,
    10958422, 10643353, 10321872, 9994173,  9660458,  9320921,  8975768,  8625213,  8269458,  7908723,  7543227,
    7173183,  6798819,  6420360,  6038037,  5652073,  5262704,  4870169,  4474697,  4076529,  3675906,  3273072,
    2868263,  2461726,  2053710,  1644454,  1234206,  823215,   411733,
};

// The interpolator's taps: 4 for each of 128 fractions of a sample. The game stores them as floats, all multiples of
// 1/32,768. This table holds the multiples. __AXFXGetSrcCoef (0x8010a9cc; October 2006, 0x80034b20) returns the address
// of a fraction's taps in the table at 0x803aeda0 in Wii Sports, 0x80344800 in Wii Play and 0x805337d0 in Smooth Moves.
// The three hold the same 2,048 bytes.
constexpr std::array<std::array<int16_t, 4>, 128> kSrcMultiples = {{
    {3195, 26287, 3329, -32},   {3064, 26281, 3467, -34},   {2936, 26270, 3608, -38},   {2811, 26253, 3751, -42},
    {2688, 26230, 3897, -46},   {2568, 26202, 4046, -50},   {2451, 26169, 4199, -54},   {2338, 26130, 4354, -58},
    {2227, 26085, 4512, -63},   {2120, 26035, 4673, -67},   {2015, 25980, 4837, -72},   {1912, 25919, 5004, -76},
    {1813, 25852, 5174, -81},   {1716, 25780, 5347, -87},   {1622, 25704, 5522, -92},   {1531, 25621, 5701, -98},
    {1442, 25533, 5882, -103},  {1357, 25440, 6066, -109},  {1274, 25342, 6253, -115},  {1193, 25239, 6442, -121},
    {1115, 25131, 6635, -127},  {1040, 25018, 6830, -133},  {967, 24899, 7027, -140},   {897, 24776, 7227, -146},
    {829, 24648, 7430, -153},   {764, 24516, 7635, -159},   {701, 24379, 7842, -166},   {641, 24237, 8052, -174},
    {583, 24091, 8264, -181},   {526, 23940, 8478, -187},   {472, 23785, 8695, -194},   {420, 23626, 8914, -202},
    {371, 23462, 9135, -209},   {324, 23295, 9358, -215},   {279, 23123, 9583, -222},   {236, 22948, 9809, -230},
    {194, 22769, 10038, -237},  {154, 22586, 10269, -243},  {117, 22399, 10501, -250},  {81, 22208, 10735, -258},
    {47, 22015, 10970, -265},   {15, 21818, 11206, -271},   {-16, 21618, 11444, -277},  {-44, 21415, 11684, -283},
    {-71, 21208, 11924, -290},  {-97, 20999, 12166, -296},  {-121, 20786, 12409, -302}, {-143, 20571, 12653, -306},
    {-163, 20354, 12898, -311}, {-183, 20134, 13143, -316}, {-201, 19911, 13389, -321}, {-218, 19686, 13635, -325},
    {-234, 19459, 13882, -328}, {-248, 19230, 14130, -332}, {-261, 18998, 14377, -335}, {-273, 18765, 14625, -337},
    {-284, 18531, 14873, -339}, {-294, 18295, 15121, -341}, {-302, 18057, 15369, -341}, {-310, 17817, 15617, -341},
    {-317, 17577, 15864, -340}, {-323, 17335, 16111, -340}, {-328, 17092, 16357, -338}, {-332, 16848, 16603, -336},
    {-336, 16603, 16848, -332}, {-338, 16357, 17092, -328}, {-340, 16111, 17335, -323}, {-340, 15864, 17577, -317},
    {-341, 15617, 17817, -310}, {-341, 15369, 18057, -302}, {-341, 15121, 18295, -294}, {-339, 14873, 18531, -284},
    {-337, 14625, 18765, -273}, {-335, 14377, 18998, -261}, {-332, 14130, 19230, -248}, {-328, 13882, 19459, -234},
    {-325, 13635, 19686, -218}, {-321, 13389, 19911, -201}, {-316, 13143, 20134, -183}, {-311, 12898, 20354, -163},
    {-306, 12653, 20571, -143}, {-302, 12409, 20786, -121}, {-296, 12166, 20999, -97},  {-290, 11924, 21208, -71},
    {-283, 11684, 21415, -44},  {-277, 11444, 21618, -16},  {-271, 11206, 21818, 15},   {-265, 10970, 22015, 47},
    {-258, 10735, 22208, 81},   {-250, 10501, 22399, 117},  {-243, 10269, 22586, 154},  {-237, 10038, 22769, 194},
    {-230, 9809, 22948, 236},   {-222, 9583, 23123, 279},   {-215, 9358, 23295, 324},   {-209, 9135, 23462, 371},
    {-202, 8914, 23626, 420},   {-194, 8695, 23785, 472},   {-187, 8478, 23940, 526},   {-181, 8264, 24091, 583},
    {-174, 8052, 24237, 641},   {-166, 7842, 24379, 701},   {-159, 7635, 24516, 764},   {-153, 7430, 24648, 829},
    {-146, 7227, 24776, 897},   {-140, 7027, 24899, 967},   {-133, 6830, 25018, 1040},  {-127, 6635, 25131, 1115},
    {-121, 6442, 25239, 1193},  {-115, 6253, 25342, 1274},  {-109, 6066, 25440, 1357},  {-103, 5882, 25533, 1442},
    {-98, 5701, 25621, 1531},   {-92, 5522, 25704, 1622},   {-87, 5347, 25780, 1716},   {-81, 5174, 25852, 1813},
    {-76, 5004, 25919, 1912},   {-72, 4837, 25980, 2015},   {-67, 4673, 26035, 2120},   {-63, 4512, 26085, 2227},
    {-58, 4354, 26130, 2338},   {-54, 4199, 26169, 2451},   {-50, 4046, 26202, 2568},   {-46, 3897, 26230, 2688},
    {-42, 3751, 26253, 2811},   {-38, 3608, 26270, 2936},   {-34, 3467, 26281, 3064},   {-32, 3329, 26287, 3195},
}};

// The taps as the game's floats. Each multiple of 1/32,768 converts exactly.
constexpr std::array<std::array<float, 4>, 128> kSrcTaps = []
{
    std::array<std::array<float, 4>, 128> taps{};
    for (std::size_t fraction = 0; fraction < taps.size(); fraction++)
    {
        for (std::size_t tap = 0; tap < 4; tap++)
        {
            taps[fraction][tap] = static_cast<float>(kSrcMultiples[fraction][tap]) / 32768.0f;
        }
    }

    return taps;
}();

// The low 32 bits of a * b >> 24, with the product on 64 bits (mullw and mulhw, or mulhwu with corrections).
int32_t MultiplyShift24(int32_t a, int32_t b)
{
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 24);
}

// AXFXChorusInit's and AXFXChorusSettings' conversion of AXFX_CHORUS's parameters to AXFX_CHORUS_EXP's.
ChorusExp::Params ExpParams(const Chorus::Params& params)
{
    const float base_delay = static_cast<float>(params.base_delay);
    ChorusExp::Params result;
    result.delay_time = base_delay;
    result.depth = static_cast<float>(params.variation) / base_delay;
    result.rate = 1000.0f / static_cast<float>(params.period);
    result.feedback = 0.0f;
    result.out_gain = 1.0f;
    result.send_gain = 0.0f;

    return result;
}

} // namespace

uint32_t ChorusExp::GetMemSize()
{
    return kChannelCount * kLineSize * 4;
}

bool ChorusExp::Init(const Params& params)
{
    params_ = params;
    active_ |= kStopped;
    size_ = kLineSize;
    for (std::vector<float>& line : line_)
    {
        line.assign(size_, 0.0f);
    }

    // The output starts the delay time behind the input, in 16.16 fixed point.
    in_pos_ = 0;
    const uint32_t delay = ToUnsigned(32.0f * params_.delay_time);
    out_pos_ = (size_ - delay) << 16;
    last_pos_ = out_pos_;
    size_fp_ = size_ << 16;
    if (!InitParams())
    {
        FreeLines();
        return false;
    }

    active_ &= ~kStopped;

    return true;
}

bool ChorusExp::Settings(const Params& params)
{
    FreeLines();
    if (!Init(params))
    {
        return false;
    }

    active_ = (active_ | kSkipFrame) & ~kStopped;

    return true;
}

void ChorusExp::Shutdown()
{
    FreeLines();
}

void ChorusExp::Process(AuxFrame& frame)
{
    Process(frame, nullptr, nullptr);
}

void ChorusExp::Process(AuxFrame& frame, const AuxFrame* bus_in, AuxFrame* bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkipFrame;
        return;
    }

    const std::array<int32_t, kSamplesPerFrame> offsets = CalcLfo();
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        // The read position: the output position moved by the LFO, kept within the lines. The history takes the
        // samples that the read position has passed since the last sample.
        uint32_t position = out_pos_ + static_cast<uint32_t>(offsets[i]);
        if (static_cast<int32_t>(position) >= static_cast<int32_t>(size_fp_))
        {
            position -= size_fp_;
        }
        else if (static_cast<int32_t>(position) < 0)
        {
            position += size_fp_;
        }

        uint32_t distance = position - last_pos_;
        if (static_cast<int32_t>(distance) < 0)
        {
            distance += size_fp_;
        }

        uint32_t read = last_pos_ >> 16;
        for (uint32_t steps = distance >> 16; steps > 0; steps--)
        {
            for (int channel = 0; channel < kChannelCount; channel++)
            {
                history_[channel][hist_index_] = line_[channel][read];
            }

            hist_index_ = (hist_index_ + 1) & 3;
            Step(read, size_);
        }

        last_pos_ = position & 0xffff0000;

        // Each channel's output: the history, oldest first, through the taps for the fraction of the distance.
        const std::array<float, 4>& taps = kSrcTaps[(distance & 0xffff) >> 9];
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            const std::array<float, 4>& history = history_[channel];
            float output = 0.0f;
            output = output + taps[0] * history[hist_index_];
            output = output + taps[1] * history[(hist_index_ + 1) & 3];
            output = output + taps[2] * history[(hist_index_ + 2) & 3];
            output = output + taps[3] * history[(hist_index_ + 3) & 3];

            int32_t& sample = frame.channels[channel][i];
            uint32_t input = static_cast<uint32_t>(sample);
            if (bus_in != nullptr)
            {
                input += static_cast<uint32_t>(bus_in->channels[channel][i]);
            }

            line_[channel][in_pos_] = static_cast<float>(static_cast<int32_t>(input)) + output * params_.feedback;
            sample = ntdlib::PowerPcToInt32(output * params_.out_gain);
            if (bus_out != nullptr)
            {
                bus_out->channels[channel][i] = ntdlib::PowerPcToInt32(output * params_.send_gain);
            }
        }

        Step(in_pos_, size_);
        out_pos_ += 0x10000;
        if (out_pos_ >= size_fp_)
        {
            out_pos_ = 0;
        }
    }
}

bool ChorusExp::InitParams()
{
    // The game's checks pass a NaN delay time, depth or rate. The read position then leaves the lines. The model
    // refuses a NaN there.
    const Params& p = params_;
    if (!(p.delay_time >= 0.1f && p.delay_time <= 50.0f) || !(p.depth >= 0.0f && p.depth <= 1.0f) ||
        !(p.rate >= 0.1f && p.rate <= 2.0f) || p.feedback < 0.0f || p.feedback >= 1.0f || p.out_gain < 0.0f ||
        p.out_gain > 1.0f || p.send_gain < 0.0f || p.send_gain > 1.0f)
    {
        return false;
    }

    // The LFO's swing is `depth` times the delay, less a sample at full depth. One step of the table takes `step`
    // samples.
    const float delay_samples = 32.0f * p.delay_time;
    float depth_samples = delay_samples * p.depth;
    if (depth_samples >= delay_samples)
    {
        depth_samples = depth_samples - 1.0f;
        if (depth_samples < 0.0f)
        {
            depth_samples = 0.0f;
        }
    }

    const float step = kSampleRate / p.rate * 0.00390625f;
    lfo_ = Lfo{};
    lfo_.phase_add = ntdlib::PowerPcToInt32(65536.0f * (256.0f * p.rate / kSampleRate));
    lfo_.depth_samp = ntdlib::PowerPcToInt32(65536.0f * depth_samples);
    lfo_.grad_factor = ntdlib::PowerPcToInt32(65536.0f * (depth_samples / step));
    lfo_.last_num = 0xffffffff;
    history_ = {};
    hist_index_ = 0;

    return true;
}

void ChorusExp::FreeLines()
{
    active_ |= kStopped;
    for (std::vector<float>& line : line_)
    {
        line = std::vector<float>();
    }
}

std::array<int32_t, kSamplesPerFrame> ChorusExp::CalcLfo()
{
    std::array<int32_t, kSamplesPerFrame> offsets{};
    for (int32_t& offset : offsets)
    {
        // At each step of the table, the LFO starts from the step's value and moves toward the next step's value in
        // equal parts.
        const uint32_t number = lfo_.phase & 0xffff0000;
        int32_t value = 0;
        if (number != lfo_.last_num)
        {
            lfo_.last_num = number;
            const uint32_t index = number >> 16;
            const int32_t start = kLfoSine[index];
            const int32_t end = kLfoSine[(index + 1) & 127];
            value = MultiplyShift24(start, lfo_.depth_samp);
            lfo_.grad = MultiplyShift24(end - start, lfo_.grad_factor);
        }
        else
        {
            value = static_cast<int32_t>(static_cast<uint32_t>(lfo_.last_value) + static_cast<uint32_t>(lfo_.grad));
        }

        // The table holds half a period. The other half is the same, negated.
        lfo_.last_value = value;
        if (lfo_.sign >= 1)
        {
            value = static_cast<int32_t>(0u - static_cast<uint32_t>(value));
        }

        lfo_.phase += static_cast<uint32_t>(lfo_.phase_add);
        if ((lfo_.phase & 0xff800000) != 0)
        {
            lfo_.phase &= 0x7fffff;
            lfo_.sign ^= 1;
        }

        offset = value;
    }

    return offsets;
}

uint32_t Chorus::GetMemSize()
{
    return ChorusExp::GetMemSize();
}

bool Chorus::Init(const Params& params)
{
    return exp_.Init(ExpParams(params));
}

bool Chorus::Settings(const Params& params)
{
    return exp_.Settings(ExpParams(params));
}

void Chorus::Shutdown()
{
    exp_.Shutdown();
}

void Chorus::Process(AuxFrame& frame)
{
    exp_.Process(frame);
}

} // namespace ntdlib::rvl::axfx

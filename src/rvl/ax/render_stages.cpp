// SPDX-License-Identifier: MIT

// The stages of the microcode's model (see render.h): a voice's samples through the accelerator, the sample rate
// converters, the envelope and the filters, then the mixes into the buses and the Wii Remotes, the aux buses, the
// compressor and the output.

#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/ax/envelope.h"
#include "ntdlib/dsp/accelerator.h"
#include "ntdlib/dsp/coef_rom.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/render.h"
#include "rvl/ax/render_internal.h"

namespace ntdlib::rvl::ax
{

bool Renderer::Convert(uint16_t src, uint16_t filter, uint16_t to, int samples)
{
    if (src > kSrcNone || (src == kSrcFourTap && filter > 2))
    {
        return false;
    }

    LoadAccelerator();
    if (src == kSrcFourTap)
    {
        FourTap(filter, to, samples);
    }
    else if (src == kSrcLinear)
    {
        Linear(to, samples);
    }
    else
    {
        Copy(to, samples);
    }
    StoreAccelerator();

    return true;
}

void Renderer::ClearDepop()
{
    // The mixes write the depop values afresh.
    for (int i = 0; i < 12; i++)
    {
        dram_[Field(kPbDpop) + i] = 0;
    }
    for (int i = 0; i < 8; i++)
    {
        dram_[Field(kPbRmtDpop) + i] = 0;
    }
}

void Renderer::LoadAccelerator()
{
    // 0x07e6: the format, the addresses, the coefficients, the gain and the decoder's state go into the accelerator's
    // registers. Each register keeps its bits. The build of October 2006 loads them for each part (0x08dc).
    const auto join = [this](uint16_t word)
    {
        return uint32_t{dram_[Field(word)]} << 16 | dram_[Field(word) + 1u];
    };
    accelerator_.format = dram_[Field(kPbFormat)];
    accelerator_.start = join(kPbLoopAddress) & ntdlib::dsp::kAcceleratorAddressMask;
    accelerator_.end = join(kPbEndAddress) & ntdlib::dsp::kAcceleratorAddressMask;
    accelerator_.current = join(kPbCurrentAddress) & ntdlib::dsp::kAcceleratorCurrentMask;
    for (std::size_t i = 0; i < coefs_.size(); i++)
    {
        coefs_[i] = dram_[Field(kPbCoefs) + i];
    }
    accelerator_.gain = dram_[Field(kPbGain)];
    accelerator_.pred_scale = dram_[Field(kPbPredScale)] & ntdlib::dsp::kAcceleratorPredScaleMask;
    accelerator_.yn1 = dram_[Field(kPbYn1)];
    accelerator_.yn2 = dram_[Field(kPbYn2)];
    accelerator_.reads_stopped = false;
    reads_zero_ = false;
}

int16_t Renderer::ReadSample()
{
    if (reads_zero_)
    {
        return static_cast<int16_t>(dram_[kZero]);
    }

    // On the Wii, the accelerator reads MEM1 for an address with bit 28 clear, and MEM2 otherwise.
    const auto byte = [this](uint32_t address) -> uint8_t
    {
        const uint8_t* p =
            (address & 0x10000000) == 0 ? memory_->At(address & 0x01ffffff) : memory_->Aram(address & 0x03ffffff);
        return p != nullptr ? *p : 0;
    };

    return static_cast<int16_t>(ntdlib::dsp::AcceleratorNext(accelerator_, coefs_, byte, [this] { AtEnd(); }));
}

void Renderer::AtEnd()
{
    // At the end address the accelerator stops its reads and raises exception 5. The handler (0x0f61) stops a voice
    // that doesn't loop, and points the frame's later reads at a zero. The handler of the build of October 2006
    // (0x0f67) leaves the reads stopped, and they give 0 too. For a voice that loops, the handler loads the decoder's
    // state at the loop start, and the reads go on. A stream keeps the decoder's history.
    accelerator_.reads_stopped = true;
    if (dram_[Field(kPbLoopFlag)] != 1)
    {
        dram_[kPbState] = 0;
        reads_zero_ = true;
        return;
    }

    accelerator_.pred_scale = dram_[Field(kPbLoopPredScale)] & ntdlib::dsp::kAcceleratorPredScaleMask;
    if (dram_[kPbType] != 1)
    {
        accelerator_.yn1 = dram_[Field(kPbLoopYn1)];
        accelerator_.yn2 = dram_[Field(kPbLoopYn2)];
    }
    accelerator_.reads_stopped = false;
}

void Renderer::StoreAccelerator()
{
    // The converters end by storing the decoder's state and the current address back in the PB.
    dram_[Field(kPbYn2)] = accelerator_.yn2;
    dram_[Field(kPbYn1)] = accelerator_.yn1;
    dram_[Field(kPbPredScale)] = accelerator_.pred_scale;
    dram_[Field(kPbCurrentAddress) + 1u] = static_cast<uint16_t>(accelerator_.current);
    dram_[Field(kPbCurrentAddress)] = static_cast<uint16_t>(accelerator_.current >> 16);
}

void Renderer::FourTap(uint16_t filter, uint16_t to, int samples)
{
    // The 4-tap converter (0x071a, and 0x0806 in the build of October 2006). The ratio advances a 16.16 position. Each
    // whole sample is a read into a ring of the last four. The position's top 7 bits select four coefficients of the
    // filter. They multiply the four samples, oldest first. Each product is doubled.
    const uint16_t last = Field(kPbLast);
    const uint16_t ratio_at = Field(kPbRatio);
    const auto ratio = static_cast<int32_t>(uint32_t{dram_[ratio_at]} << 16 | dram_[ratio_at + 1u]);
    std::array<int16_t, 4> ring{};
    for (std::size_t i = 0; i < ring.size(); i++)
    {
        ring[i] = static_cast<int16_t>(dram_[last + i]);
    }
    std::size_t write = 0;
    const auto table = static_cast<uint16_t>(kCoefficientRom + filter * kFilterWords);
    int64_t position = int64_t{dram_[Field(kPbFrac)]} + ratio;
    for (int i = 0; i < samples; i++)
    {
        const auto reads = static_cast<uint16_t>(MiddleWord(position));
        const auto frac = static_cast<uint32_t>(position & 0xffff);
        for (uint16_t n = 0; n < reads; n++)
        {
            ring[write] = ReadSample();
            write = (write + 1) & 3;
        }
        position = Wrap40(position - int64_t{static_cast<int16_t>(reads)} * 0x10000);
        position = Wrap40(position + ratio);

        const std::size_t first = std::size_t{table} - kCoefficientRom + (frac >> 9) * 4;
        int64_t sum = 0;
        for (std::size_t k = 0; k < 4; k++)
        {
            const auto coefficient = static_cast<int16_t>(ntdlib::dsp::kCoefficientRom[first + k]);
            sum += 2 * int64_t{coefficient} * ring[(write + k) & 3];
        }
        dram_[to + i] = static_cast<uint16_t>(MiddleWord(Wrap40(sum)));
    }
    position = Wrap40(position - ratio);

    for (std::size_t i = 0; i < ring.size(); i++)
    {
        dram_[last + i] = static_cast<uint16_t>(ring[(write + i) & 3]);
    }
    dram_[Field(kPbFrac)] = static_cast<uint16_t>(position);
}

void Renderer::Linear(uint16_t to, int samples)
{
    // The linear converter (0x076b, and 0x085a in the build of October 2006): the same reads, then the oldest of the
    // four samples and the next one, weighted by the position. At a whole position, the oldest sample as it is.
    const uint16_t last = Field(kPbLast);
    const uint16_t ratio_at = Field(kPbRatio);
    const auto ratio = static_cast<int32_t>(uint32_t{dram_[ratio_at]} << 16 | dram_[ratio_at + 1u]);
    std::array<int16_t, 4> ring{};
    for (std::size_t i = 0; i < ring.size(); i++)
    {
        ring[i] = static_cast<int16_t>(dram_[last + i]);
    }
    std::size_t write = 0;
    int64_t position = int64_t{dram_[Field(kPbFrac)]} + ratio;
    for (int i = 0; i < samples; i++)
    {
        const auto reads = static_cast<uint16_t>(MiddleWord(position));
        for (uint16_t n = 0; n < reads; n++)
        {
            ring[write] = ReadSample();
            write = (write + 1) & 3;
        }
        position = Wrap40(position - int64_t{static_cast<int16_t>(reads)} * 0x10000);

        const int64_t frac = position;
        int16_t sample = ring[write];
        if (frac != 0)
        {
            const auto weight = static_cast<uint16_t>(-frac);
            sample = MiddleWord(int64_t{ring[write]} * weight + int64_t{ring[(write + 1) & 3]} * (frac & 0xffff));
        }
        dram_[to + i] = static_cast<uint16_t>(sample);
        position = Wrap40(position + ratio);
    }
    position = Wrap40(position - ratio);

    for (std::size_t i = 0; i < ring.size(); i++)
    {
        dram_[last + i] = static_cast<uint16_t>(ring[(write + i) & 3]);
    }
    dram_[Field(kPbFrac)] = static_cast<uint16_t>(position);
}

void Renderer::Copy(uint16_t to, int samples)
{
    // Without a converter (0x07c0, and 0x08b4 in the build of October 2006), the frame takes its samples as they come.
    // The PB keeps the last four.
    for (int i = 0; i < samples; i++)
    {
        dram_[to + i] = static_cast<uint16_t>(ReadSample());
    }
    for (int i = 0; i < 4; i++)
    {
        dram_[Field(kPbLast) + i] = dram_[to + samples - 4 + i];
    }
}

void Renderer::Envelope(uint16_t at, int samples)
{
    // The volume envelope (0x034c, and 0x036d in the build of October 2006): a gain that starts at the PB's volume and
    // moves by the delta each sample. Each pair of samples steps by twice the delta, a 16-bit signed value. The gain is
    // unsigned, the product doubled and saturated to 16 bits.
    const uint16_t volume = dram_[Field(kPbVolume)];
    const uint16_t delta = dram_[Field(kPbDelta)];
    for (int i = 0; i < samples; i++)
    {
        const uint16_t gain = ntdlib::ax::EnvelopeGain(volume, delta, i);
        const auto x = static_cast<int16_t>(dram_[at + i]);
        dram_[at + i] = static_cast<uint16_t>(MiddleWord(2 * int64_t{x} * gain));
    }
    dram_[Field(kPbVolume)] = ntdlib::ax::EnvelopeVolumeAfter(volume, delta, samples);
}

void Renderer::LowPass(uint16_t state, uint16_t from, uint16_t to, int samples)
{
    // The one-pole low-pass filter (0x06ab, and 0x039b in the build of October 2006): y = a0 x + b0 y1, with the
    // products doubled. a0 multiplies as unsigned (mulx), b0 as signed (maddx). Each output saturates to 16 bits and
    // feeds back that way. The state holds y1, a0 and b0.
    auto y = static_cast<int16_t>(dram_[state]);
    const uint16_t a0 = dram_[state + 1u];
    const auto b0 = static_cast<int16_t>(dram_[state + 2u]);
    for (int i = 0; i < samples; i++)
    {
        const auto x = static_cast<int16_t>(dram_[from + static_cast<uint16_t>(i)]);
        y = MiddleWord(2 * int64_t{x} * a0 + 2 * int64_t{y} * b0);
        dram_[to + static_cast<uint16_t>(i)] = static_cast<uint16_t>(y);
    }
    dram_[state] = static_cast<uint16_t>(y);
}

void Renderer::SecondFilter(uint16_t at)
{
    // The second filter of the build of October 2006 (0x03c1): y = a x + b s, with the products doubled. a multiplies
    // as unsigned (mulx), b as signed (maddx). For the part's first sample, s is the filter's state. After that, s is
    // the last output negated. Each output saturates to 16 bits, and so does each negated output. The state keeps the
    // part's last output negated.
    auto s = static_cast<int16_t>(dram_[kPbSecondFilter + 1u]);
    const uint16_t a = dram_[kPbSecondFilter + 2u];
    const auto b = static_cast<int16_t>(dram_[kPbSecondFilter + 3u]);
    for (int i = 0; i < kPartSamples; i++)
    {
        const auto x = static_cast<int16_t>(dram_[at + i]);
        const int16_t y = MiddleWord(2 * int64_t{x} * a + 2 * int64_t{s} * b);
        dram_[at + i] = static_cast<uint16_t>(y);
        s = static_cast<int16_t>(Saturated(-int64_t{y}));
    }
    dram_[kPbSecondFilter + 1u] = static_cast<uint16_t>(s);
}

void Renderer::Biquad(uint16_t state, uint16_t from, uint16_t to)
{
    // The biquad filter (0x06c3), with coefficients in 2.14. The sum is shifted left two places, rounded to a whole
    // sample (ties to even) and saturated to 16 bits. Each output feeds back that way. The state holds x1, x2, y1, y2,
    // b0, b1, b2, a1 and a2.
    auto x1 = static_cast<int16_t>(dram_[state]);
    auto x2 = static_cast<int16_t>(dram_[state + 1u]);
    auto y1 = static_cast<int16_t>(dram_[state + 2u]);
    auto y2 = static_cast<int16_t>(dram_[state + 3u]);
    const auto b0 = static_cast<int16_t>(dram_[state + 4u]);
    const auto b1 = static_cast<int16_t>(dram_[state + 5u]);
    const auto b2 = static_cast<int16_t>(dram_[state + 6u]);
    const auto a1 = static_cast<int16_t>(dram_[state + 7u]);
    const auto a2 = static_cast<int16_t>(dram_[state + 8u]);
    for (int i = 0; i < kSamples; i++)
    {
        const auto x = static_cast<int16_t>(dram_[from + static_cast<uint16_t>(i)]);
        const int64_t sum = int64_t{b0} * x + int64_t{b1} * x1 + int64_t{b2} * x2 + int64_t{a1} * y1 + int64_t{a2} * y2;
        const int16_t y = MiddleWord(Wrap40(Round(Wrap40(sum * 4))));
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        dram_[to + static_cast<uint16_t>(i)] = static_cast<uint16_t>(y);
    }
    dram_[state] = static_cast<uint16_t>(x1);
    dram_[state + 1u] = static_cast<uint16_t>(x2);
    dram_[state + 2u] = static_cast<uint16_t>(y1);
    dram_[state + 3u] = static_cast<uint16_t>(y2);
}

void Renderer::MixBus(int bus, uint32_t bits)
{
    // The handlers that the mixer control selects for each bus (the tables at 0x0d13, 0x0d33, 0x0d53 and 0x0d73). The
    // surround channel mixes with a volume that moves when its ramp bit is set, and otherwise when its bit is set. The
    // ramp bit of the left and right channels mixes both with volumes that move. Otherwise each mixes when its bit is
    // set. Bus C's sixth bit makes its left and right channels take the frame without the ITD.
    const auto b = static_cast<std::size_t>(bus);
    const bool unshifted = bus == 3 && (bits & 0x20) != 0;
    const uint16_t left = unshifted ? kFrame : dram_[kSourceL];
    const uint16_t right = unshifted ? kFrame : dram_[kSourceR];
    const auto volume_l = static_cast<uint16_t>(kPbMix + 4 * bus);
    const auto volume_s = static_cast<uint16_t>(kPbMix + 16 + 2 * bus);
    if ((bits & 0x18) != 0)
    {
        dram_[kPbDpop + 8 + b] = MixChannel(kFrame, volume_s, (bits & 0x10) != 0, kBuses[b][2], kSamples);
    }
    // The voice's meter adds up its mix into the main bus's left and right.
    const bool ramp = (bits & 4) != 0;
    MeterSums* sums = bus == 0 && meter_ >= 0 ? &meter_sums_[static_cast<std::size_t>(meter_)] : nullptr;
    if (ramp || (bits & 1) != 0)
    {
        dram_[kPbDpop + b] =
            MixChannel(left, volume_l, ramp, kBuses[b][0], kSamples, sums != nullptr ? (*sums)[0].data() : nullptr);
    }
    if (ramp || (bits & 2) != 0)
    {
        dram_[kPbDpop + 4 + b] = MixChannel(right, static_cast<uint16_t>(volume_l + 2), ramp, kBuses[b][1], kSamples,
                                            sums != nullptr ? (*sums)[1].data() : nullptr);
    }
}

uint16_t Renderer::MixChannel(uint16_t source, uint16_t volume, bool ramp, uint16_t bus, int samples, int64_t* meter)
{
    // Adds the samples into one bus channel (0x0bd1, 0x0beb, 0x0c21 and 0x0c51): each product, doubled, is added below
    // the bus sample's 16 fractional bits, in 40 bits. The volume is constant, or it starts at the PB's volume and
    // moves by the delta each sample. The PB keeps the volume after the frame. Returns the depop value: the middle word
    // of the last product. A meter adds up what each product adds to the bus sample. The build of October 2006 mixes
    // with the DSP's ROM (0x80e7, 0x81f9, 0x8282 and 0x845d) and, for the Wii Remotes, with 0x0d07. Their volumes
    // ramp as signed values and stop at the range's ends (SaturatedRamp).
    const uint16_t start = dram_[volume];
    const uint16_t delta = dram_[volume + 1u];
    const bool saturates = build_ == Build::kOctober2006;
    int64_t product = 0;
    for (int i = 0; i < samples; i++)
    {
        uint16_t gain = start;
        if (ramp)
        {
            gain = saturates ? SaturatedRamp(start, delta, i)
                             : static_cast<uint16_t>(start + static_cast<uint32_t>(i) * delta);
        }
        const auto x = static_cast<int16_t>(dram_[static_cast<uint16_t>(source + i) & 0x0fffu]);
        product = 2 * int64_t{x} * gain;
        const auto at = static_cast<uint16_t>(bus + 2 * i);
        Store32(dram_, at, Wrap40(Wrap40(Load32(dram_, at) * 0x10000) + product) >> 16);
        if (meter != nullptr)
        {
            meter[i] += product >> 16;
        }
    }
    if (ramp)
    {
        dram_[volume] = saturates ? SaturatedRamp(start, delta, samples)
                                  : static_cast<uint16_t>(start + static_cast<uint32_t>(samples) * delta);
    }

    return static_cast<uint16_t>(MiddleWord(product));
}

void Renderer::MixBusOctober2006(int bus, uint32_t bits, int part, uint16_t source, uint16_t left, uint16_t right)
{
    // The handlers that the mixer control selects for each bus in the build of October 2006 (the tables at 0x0d4e,
    // 0x0d6e, 0x0d8e and 0x0dae, and at 0x0dce for bus C's sixth bit). They take the channels that MixBus takes, 32
    // samples of each. Buses B and C keep no depop value for the surround channel when it alone mixes, with a volume
    // that moves (0x0b3b and 0x0bfb).
    const auto b = static_cast<std::size_t>(bus);
    const bool unshifted = bus == 3 && (bits & 0x20) != 0;
    const auto volume_l = static_cast<uint16_t>(kPbMix + 4 * bus);
    const auto volume_s = static_cast<uint16_t>(kPbMix + 16 + 2 * bus);
    const uint16_t dpop = Field(kPbDpop);
    const auto offset = static_cast<uint16_t>(2 * kPartSamples * part);
    if ((bits & 0x18) != 0)
    {
        const uint16_t last = MixChannel(source, volume_s, (bits & 0x10) != 0,
                                         static_cast<uint16_t>(kBuses[b][2] + offset), kPartSamples);
        if (bus < 2 || (bits & 0x10) == 0 || (bits & 7) != 0)
        {
            dram_[dpop + 8 + b] = last;
        }
    }
    const bool ramp = (bits & 4) != 0;
    int64_t* sums_l = nullptr;
    int64_t* sums_r = nullptr;
    if (bus == 0 && meter_ >= 0)
    {
        MeterSums& sums = meter_sums_[static_cast<std::size_t>(meter_)];
        sums_l = sums[0].data() + kPartSamples * part;
        sums_r = sums[1].data() + kPartSamples * part;
    }
    if (ramp || (bits & 1) != 0)
    {
        dram_[dpop + b] = MixChannel(unshifted ? source : left, volume_l, ramp,
                                     static_cast<uint16_t>(kBuses[b][0] + offset), kPartSamples, sums_l);
    }
    if (ramp || (bits & 2) != 0)
    {
        dram_[dpop + 4 + b] = MixChannel(unshifted ? source : right, static_cast<uint16_t>(volume_l + 2), ramp,
                                         static_cast<uint16_t>(kBuses[b][1] + offset), kPartSamples, sums_r);
    }
}

void Renderer::ConvertRemote(int samples, uint16_t frac_at, uint16_t last)
{
    // The 4-tap converter of the Wii Remotes' path, from 32 kHz to 6 kHz on the 8 kHz filter: `samples` samples into
    // kRemoteFrame from the samples at kScratch, from the position at `frac_at`. The last four samples that it reads go
    // back to `last`, and the position to `frac_at`.
    int64_t position = dram_[frac_at];
    uint16_t index = 0;
    for (int j = 0; j < samples; j++)
    {
        position = Wrap40(position + kRemoteRatio);
        index = static_cast<uint16_t>(MiddleWord(position));
        const auto frac = static_cast<uint32_t>(position & 0xffff);
        const std::size_t first = (frac >> 9) * 4;
        int64_t sum = 0;
        for (std::size_t k = 0; k < 4; k++)
        {
            const auto coefficient = static_cast<int16_t>(ntdlib::dsp::kCoefficientRom[first + k]);
            const auto sample = static_cast<int16_t>(dram_[(kScratch + index + k) & 0x0fffu]);
            sum += 2 * int64_t{coefficient} * sample;
        }
        dram_[kRemoteFrame + j] = static_cast<uint16_t>(MiddleWord(Wrap40(sum)));
    }
    for (int i = 0; i < 4; i++)
    {
        dram_[last + i] = dram_[(kScratch + index + i) & 0x0fff];
    }
    dram_[frac_at] = static_cast<uint16_t>(position);
}

void Renderer::Remote()
{
    // The Wii Remotes' path (0x03a2): the remote filter, a 4-tap converter from 32 kHz to 6 kHz on the 8 kHz filter,
    // then the eight mixes. Each mix takes two bits of the remote mixer control: none, a constant volume, or a volume
    // that moves.
    const uint16_t iir = dram_[kPbRmtIir];
    if (iir == 0)
    {
        for (int i = 0; i < kSamples; i++)
        {
            dram_[kRemoteInput + i] = dram_[kFrame + i];
        }
    }
    else if (iir == 2)
    {
        Biquad(kPbRmtIir + 1, kFrame, kRemoteInput);
    }
    else
    {
        LowPass(kPbRmtIir + 1, kFrame, kRemoteInput);
    }

    for (int i = 0; i < 4; i++)
    {
        dram_[kScratch + i] = dram_[kPbRmtLast + i];
    }
    ConvertRemote(kRemoteSamples, kPbRmtFrac, kPbRmtLast);

    const uint16_t control = dram_[kPbRmtMixerCtrl];
    for (std::size_t k = 0; k < kRemoteBuses.size(); k++)
    {
        const unsigned bits = control >> (2 * k) & 3u;
        uint16_t dpop = 0;
        if (bits != 0)
        {
            dpop = MixChannel(kRemoteFrame, static_cast<uint16_t>(kPbRmtMix + 2 * k), bits >= 2, kRemoteBuses[k],
                              kRemoteSamples);
        }
        dram_[kRemoteDpop[k]] = dpop;
    }
}

void Renderer::RemoteOctober2006(int part, uint16_t source)
{
    // The Wii Remotes' path of the build of October 2006 (0x0406): a 4-tap converter from 32 kHz to 6 kHz on the 8 kHz
    // filter turns the part's 32 samples into 6. It takes the PB's last four samples before them. This build has no
    // remote filter. Each of the eight mixes then takes a volume that moves (0x0d07), whatever the remote mixer control
    // selects.
    const uint16_t last = Field(kPbRmtLast);
    const uint16_t frac_at = Field(kPbRmtFrac);
    for (int i = 0; i < 4; i++)
    {
        dram_[kScratch + i] = dram_[last + i];
    }
    for (int i = 0; i < kPartSamples; i++)
    {
        dram_[kScratch + 4 + i] = dram_[static_cast<uint16_t>(source + i) & 0x0fffu];
    }
    ConvertRemote(kPartRemoteSamples, frac_at, last);

    const auto offset = static_cast<uint16_t>(2 * kPartRemoteSamples * part);
    for (std::size_t k = 0; k < kRemoteBuses.size(); k++)
    {
        dram_[Field(kRemoteDpop[k])] = MixChannel(kRemoteFrame, static_cast<uint16_t>(Field(kPbRmtMix) + 2 * k), true,
                                                  static_cast<uint16_t>(kRemoteBuses[k] + offset), kPartRemoteSamples);
    }
}

void Renderer::MixAux(int bus, uint16_t volume, uint32_t write, uint32_t read)
{
    // MIX_AUX_A, B and C (0x049b, 0x04b4 and 0x04cd): the bus's three channels go out to the frame that the callback
    // processes later, and the frame that it has processed comes back in their place. That frame goes into the main
    // bus at the return volume. The volume moves from the last frame's. The build of October 2006 doesn't double the
    // return's products. The bus's meter takes the left and right, and keeps the surround channel for the next frame's
    // ADD_TO_LR.
    const auto b = static_cast<std::size_t>(bus);
    const uint16_t base = kBuses[b + 1][0];
    const uint16_t old = dram_[kAuxVolumes + b];
    dram_[kAuxVolumes + b] = volume;
    ToMain(base, write, 0x480);
    const std::array<uint16_t, kSamples> gains = Ramp(old, volume);
    FromMain(read, base, 0x480);
    for (std::size_t channel = 0; channel < 3; channel++)
    {
        for (int i = 0; i < kSamples; i++)
        {
            const auto main_at = static_cast<uint16_t>(kBuses[0][channel] + 2 * i);
            const auto aux_at = static_cast<uint16_t>(kBuses[b + 1][channel] + 2 * i);
            const int64_t sample = Load32(dram_, aux_at);
            const uint16_t gain = gains[static_cast<std::size_t>(i)];
            const int64_t added = build_ == Build::kOctober2006 ? ScaledOnce(sample, gain) : Scaled(sample, gain);
            Store32(dram_, main_at, Wrap40(Load32(dram_, main_at) + added));
            if (meters_ != nullptr && channel < 2)
            {
                meter_sums_[meters_->peaks.size() + b][channel][static_cast<std::size_t>(i)] += added;
            }
            else if (meters_ != nullptr)
            {
                meters_->aux_surround[b][static_cast<std::size_t>(i)] = added;
            }
        }
    }
}

void Renderer::Compressor(uint16_t threshold, uint16_t release, uint32_t table)
{
    // COMPRESSOR (0x057b). While any sample of the left and right channels reaches the threshold, the frame takes a row
    // of the table's gains: the attack's row for the frames counted down so far, and the release count starts again.
    // In a quiet frame, the count goes down, and the frame takes the release's row for the count. Once the count is 0,
    // a quiet frame stays as it is.
    bool loud = false;
    for (int i = 0; i < 2 * kSamples && !loud; i++)
    {
        const int64_t sample = Load32(dram_, static_cast<uint16_t>(2 * i));
        loud = (sample < 0 ? -sample : sample) >= threshold;
    }

    uint32_t row = 0;
    if (loud)
    {
        row = table + uint32_t{dram_[kCompressorCount]} * 0xc0;
        dram_[kCompressorCount] = release;
    }
    else
    {
        if (dram_[kCompressorCount] == 0)
        {
            return;
        }
        dram_[kCompressorCount]--;
        row = table + uint32_t{dram_[kCompressorCount]} * 0xc0 + 0x840;
    }

    FromMain(row, kScratch, 0xc0);
    for (const uint16_t channel : {kBuses[0][0], kBuses[0][1]})
    {
        for (int i = 0; i < kSamples; i++)
        {
            const auto at = static_cast<uint16_t>(channel + 2 * i);
            const int64_t sample = Load32(dram_, at);
            const uint16_t gain = dram_[kScratch + i];
            const int64_t high = Wrap40(2 * (sample >> 16) * gain * 0x10000);
            Store32(dram_, at, Wrap40(high + 2 * (sample & 0xffff) * gain) >> 16);
        }
    }
}

void Renderer::WmOutput(const std::array<uint32_t, 4>& remotes)
{
    // WM_OUTPUT (0x067c): each Wii Remote's main bus, saturated to 16 bits, goes to its buffer.
    for (std::size_t remote = 0; remote < remotes.size(); remote++)
    {
        const auto staging = static_cast<uint16_t>(kRemoteBuses[1] + kRemoteSamples * remote);
        for (int i = 0; i < kRemoteSamples; i++)
        {
            const int64_t sample = Load32(dram_, static_cast<uint16_t>(kRemoteBuses[2 * remote] + 2 * i));
            dram_[staging + static_cast<uint16_t>(i)] = static_cast<uint16_t>(MiddleWord(Wrap40(sample * 0x10000)));
        }
        ToMain(staging, remotes[remote], kRemoteSamples * 2);
    }
}

void Renderer::Output(uint16_t volume, uint32_t surround, uint32_t output)
{
    // OUTPUT (0x060b): the surround channel goes out for the next frame's ADD_TO_LR. The left and right channels go out
    // at the master volume, saturated to 16 bits: right, then left, for each sample. The volume moves from the last
    // frame's.
    ToMain(kBuses[0][2], surround, 0x180);
    const std::array<uint16_t, kSamples> gains = Ramp(dram_[kMasterVolume], volume);
    dram_[kMasterVolume] = volume;
    const uint16_t staging = kBuses[1][0];
    for (int i = 0; i < kSamples; i++)
    {
        const uint16_t gain = gains[static_cast<std::size_t>(i)];
        for (int side = 0; side < 2; side++)
        {
            const uint16_t channel = side == 0 ? kBuses[0][1] : kBuses[0][0];
            const int64_t sample = Load32(dram_, static_cast<uint16_t>(channel + 2 * i));
            const int64_t scaled = Wrap40(Scaled(sample, gain) * 0x10000);
            dram_[staging + static_cast<uint16_t>(2 * i + side)] = static_cast<uint16_t>(MiddleWord(scaled));
        }
    }
    ToMain(staging, output, 0x180);
}

} // namespace ntdlib::rvl::ax

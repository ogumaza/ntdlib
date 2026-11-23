// SPDX-License-Identifier: MIT

// The helpers that the parts of System share (system.cpp and system_frame.cpp): the clamp of a sample, the addresses of
// a voice's samples, and the sync of the game's changes to a voice into the renderer's copy. The two files build as one
// translation unit. The helpers therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/ax/ax.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/render.h"
#include "ntdlib/cafe/ax/voice.h"

namespace ntdlib::cafe::ax
{
namespace
{

int16_t ClampToInt16(int32_t x)
{
    return static_cast<int16_t>(std::clamp(x, -32768, 32767));
}

// The renderer's addresses keep 29 bits of the byte address: 29 for PCM8's bytes, 28 for PCM16's words and 30 for
// ADPCM's nibbles.
uint32_t AddressMask(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return 0x1FFFFFFF;
    case SampleFormat::kPcm16:
        return 0x0FFFFFFF;
    default:
        return 0x3FFFFFFF;
    }
}

// The address in the format's units of the sample `offset` units past the sample base `base`.
uint32_t AddressOf(SampleFormat format, uint32_t base, uint32_t offset)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return base + offset;
    case SampleFormat::kPcm16:
        return base / 2 + offset;
    default:
        return 2 * base + offset;
    }
}

// The renderer's progress into the game's copy of a voice, for each thing that the game hasn't changed since.
void TakeRendererProgress(Voice& voice)
{
    Pb& game = voice.game;
    const Pb& renderer = voice.renderer;
    const uint32_t bits = voice.sync;
    if (!(bits & sync::kState))
    {
        game.state = renderer.state;
        voice.state = renderer.state;
    }
    if (!(bits & (sync::kAddresses | sync::kCurrentAddress)))
    {
        game.current_address = renderer.current_address;
    }
    if (!(bits & sync::kVe))
    {
        game.ve.volume = renderer.ve.volume;
    }
    if (!(bits & sync::kAdpcm))
    {
        game.adpcm = renderer.adpcm;
    }
    if (!(bits & sync::kSrc))
    {
        game.src.frac = renderer.src.frac;
        game.src.last = renderer.src.last;
    }
}

// The game's changes of the addresses into the renderer's copy. The bit of the whole addresses wins over each
// address's bit.
void ApplyAddressChanges(const Pb& game, Pb& renderer, uint32_t bits)
{
    if (bits & sync::kAddresses)
    {
        renderer.loop_flag = game.loop_flag;
        renderer.format = game.format;
        renderer.extension = game.extension;
        renderer.loop_address = game.loop_address;
        renderer.end_address = game.end_address;
        renderer.current_address = game.current_address;
        return;
    }

    if (bits & sync::kLoopFlag)
    {
        renderer.loop_flag = game.loop_flag;
    }
    if (bits & sync::kLoopAddress)
    {
        renderer.loop_address = game.loop_address;
    }
    if (bits & sync::kEndAddress)
    {
        renderer.end_address = game.end_address;
    }
    if (bits & sync::kCurrentAddress)
    {
        renderer.current_address = game.current_address;
    }
}

// The game's changes of the low-pass and the biquad into the renderer's copy. For each filter, the coefficients' bit
// wins over the whole filter's.
void ApplyFilterChanges(const Pb& game, Pb& renderer, uint32_t bits)
{
    if (bits & sync::kLpfCoefs)
    {
        renderer.lpf.a0 = game.lpf.a0;
        renderer.lpf.b0 = game.lpf.b0;
    }
    else if (bits & sync::kLpf)
    {
        renderer.lpf = game.lpf;
    }
    if (bits & sync::kBiquadCoefs)
    {
        renderer.biquad.b0 = game.biquad.b0;
        renderer.biquad.b1 = game.biquad.b1;
        renderer.biquad.b2 = game.biquad.b2;
        renderer.biquad.a1 = game.biquad.a1;
        renderer.biquad.a2 = game.biquad.a2;
    }
    else if (bits & sync::kBiquad)
    {
        renderer.biquad = game.biquad;
    }
}

// The game's changes into the renderer's copy of a voice. The whole SRC's bit wins over the ratio's. With
// `stale_history`, a new ADPCM block keeps the renderer's history.
void ApplyGameChanges(const Pb& game, Pb& renderer, uint32_t bits, bool stale_history)
{
    if (bits & sync::kSrcType)
    {
        renderer.src_mode = game.src_mode;
        renderer.four_tap_filter = game.four_tap_filter;
    }
    if (bits & sync::kMixMasks)
    {
        renderer.tv_mask = game.tv_mask;
        renderer.drc_mask = game.drc_mask;
    }
    if (bits & sync::kState)
    {
        renderer.state = game.state;
    }
    if (bits & sync::kType)
    {
        renderer.type = game.type;
    }
    if (bits & sync::kMix)
    {
        renderer.tv_mix = game.tv_mix;
        renderer.drc_mix = game.drc_mix;
    }
    if (bits & sync::kVe)
    {
        renderer.ve = game.ve;
    }
    ApplyAddressChanges(game, renderer, bits);
    if (bits & sync::kAdpcm)
    {
        const int16_t yn1 = renderer.adpcm.yn1;
        const int16_t yn2 = renderer.adpcm.yn2;
        renderer.adpcm = game.adpcm;
        if (stale_history)
        {
            renderer.adpcm.yn1 = yn1;
            renderer.adpcm.yn2 = yn2;
        }
    }
    if (bits & sync::kSrc)
    {
        renderer.src = game.src;
    }
    else if (bits & sync::kSrcRatio)
    {
        renderer.src.ratio = game.src.ratio;
    }
    if (bits & sync::kAdpcmLoop)
    {
        renderer.adpcm_loop = game.adpcm_loop;
    }
    ApplyFilterChanges(game, renderer, bits);
}

// The past values that an enhanced voice keeps, from a new SRC, low-pass or biquad.
void ResetEnhancedPast(EnhancedVoice& enhanced, const Pb& renderer, uint32_t bits)
{
    if (bits & sync::kSrc)
    {
        ResetHistory(enhanced, renderer.src.last);
    }
    if ((bits & sync::kLpf) && !(bits & sync::kLpfCoefs))
    {
        enhanced.lpf = static_cast<float>(renderer.lpf.yn1);
    }
    if ((bits & sync::kBiquad) && !(bits & sync::kBiquadCoefs))
    {
        const Biquad& biquad = renderer.biquad;
        enhanced.biquad = {static_cast<float>(biquad.xn1), static_cast<float>(biquad.xn2),
                           static_cast<float>(biquad.yn1), static_cast<float>(biquad.yn2)};
    }
}

} // namespace
} // namespace ntdlib::cafe::ax

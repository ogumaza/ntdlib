// SPDX-License-Identifier: MIT

// The AX parameter block (AXPB). The CPU keeps one 0x140-byte PB for each voice in main memory. The AX microcode reads
// each PB, updates it and writes it back every frame. In main memory every field is a big-endian 16-bit word. A 32-bit
// value takes two words, high word first. The layout follows AX in the RVL SDK as linked into Wii Sports (December
// 2006). __AXSetPBDefault (main.dol 0x8010707c) confirms it.
//
// WarioWare: Smooth Moves links the build of October 2006. That build keeps a 0x120-byte PB. The build's microcode
// reads that layout. Pb stands for that layout too (see LoadPbOctober2006).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/ax/ax.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rvl::ax
{

// The build of AX's CPU side. AX's microcode identifies it (ntdlib/sound_build.h).
using Build = ntdlib::AxBuild;

// One AX frame is 3 ms long and holds 96 stereo samples at 32 kHz. The Wii Remote speakers get 18 samples per frame at
// 6 kHz.
constexpr int kSamplesPerFrame = 96;
constexpr int kRemoteSamplesPerFrame = 18;
constexpr int kSampleRate = 32000;
constexpr int kRemoteCount = 4;

// The aux buses: A, B and C.
constexpr int kAuxBusCount = 3;

// AX has 96 voices (AX_VOICE_MAX) and 32 priorities, as the Wii U's AX has (ntdlib::ax). Priority 0 marks a free voice.
// Priority 31 is the highest. The build of October 2006 has 64 voices.
using ntdlib::ax::kPriorityFree;
using ntdlib::ax::kPriorityMax;
using ntdlib::ax::kVoiceCount;
constexpr int kVoiceCountOctober2006 = 64;

// The volume value for a gain of 1.0 in the mixer and the command list (AX_MAX_VOLUME).
constexpr uint16_t kMaxVolume = 0x8000;

// The sample formats in PbAddr::format.
using ntdlib::ax::SampleFormat;

// For ADPCM, the addresses in PbAddr count nibbles. Each 8-byte ADPCM frame holds a header byte and 14 samples.
constexpr uint32_t kAdpcmSamplesPerFrame = 14;
constexpr uint32_t kAdpcmNibblesPerFrame = 16;

// PCM decodes with no prediction. The ADPCM block's gain scales its samples to 16 bits.
constexpr uint16_t kPcm16Gain = 0x800;
constexpr uint16_t kPcm8Gain = 0x100;

using ntdlib::ax::VoiceState;

// The values of Pb::src_select and Pb::type.
using ntdlib::ax::SrcMode;
using ntdlib::ax::VoiceType;

// Values of the `on` word in PbLpf, PbBiquad and the remote filter. The remote filter can be either type.
constexpr uint16_t kLpfOn = 1;
constexpr uint16_t kBiquadOn = 2;

// Bits of Pb::MixerCtrl(). They select which of the voice's mixes are on and which of them ramp (DELTA). The C bits
// also serve as the rear left and right channels of Dolby Pro Logic II.
namespace mixer
{

constexpr uint32_t kL = 1u << 0;
constexpr uint32_t kR = 1u << 1;
constexpr uint32_t kDelta = 1u << 2;
constexpr uint32_t kS = 1u << 3;
constexpr uint32_t kDeltaS = 1u << 4;
constexpr uint32_t kAuxAL = 1u << 16;
constexpr uint32_t kAuxAR = 1u << 17;
constexpr uint32_t kDeltaA = 1u << 18;
constexpr uint32_t kAuxAS = 1u << 19;
constexpr uint32_t kDeltaAS = 1u << 20;
constexpr uint32_t kAuxBL = 1u << 21;
constexpr uint32_t kAuxBR = 1u << 22;
constexpr uint32_t kDeltaB = 1u << 23;
constexpr uint32_t kAuxBS = 1u << 24;
constexpr uint32_t kDeltaBS = 1u << 25;
constexpr uint32_t kAuxCL = 1u << 26;
constexpr uint32_t kAuxCR = 1u << 27;
constexpr uint32_t kDeltaC = 1u << 28;
constexpr uint32_t kAuxCS = 1u << 29;
constexpr uint32_t kDeltaCS = 1u << 30;

} // namespace mixer

// The parts of a voice's PB that the CPU has changed since the last frame (AXVPB::sync, AX_PBSYNC_*). __AXSyncPBs
// copies only these parts into the PB in main memory. It leaves the rest as the microcode wrote them.
namespace sync = ntdlib::ax::sync;

// The mix: a volume and a per-sample ramp for each bus and channel.
struct PbMix
{
    uint16_t l = 0;
    uint16_t delta_l = 0;
    uint16_t r = 0;
    uint16_t delta_r = 0;
    uint16_t aux_a_l = 0;
    uint16_t delta_aux_a_l = 0;
    uint16_t aux_a_r = 0;
    uint16_t delta_aux_a_r = 0;
    uint16_t aux_b_l = 0;
    uint16_t delta_aux_b_l = 0;
    uint16_t aux_b_r = 0;
    uint16_t delta_aux_b_r = 0;
    uint16_t aux_c_l = 0;
    uint16_t delta_aux_c_l = 0;
    uint16_t aux_c_r = 0;
    uint16_t delta_aux_c_r = 0;
    uint16_t s = 0;
    uint16_t delta_s = 0;
    uint16_t aux_a_s = 0;
    uint16_t delta_aux_a_s = 0;
    uint16_t aux_b_s = 0;
    uint16_t delta_aux_b_s = 0;
    uint16_t aux_c_s = 0;
    uint16_t delta_aux_c_s = 0;
};

// The interaural time difference (ITD). It delays one side through a buffer in main memory.
struct PbItd
{
    uint16_t flag = 0;
    uint16_t buffer_hi = 0;
    uint16_t buffer_lo = 0;
    uint16_t shift_l = 0;
    uint16_t shift_r = 0;
    uint16_t target_shift_l = 0;
    uint16_t target_shift_r = 0;
};

// The last sample that the voice added to each mix. The microcode fades these out when the voice stops (depop).
struct PbDpop
{
    uint16_t l = 0;
    uint16_t aux_a_l = 0;
    uint16_t aux_b_l = 0;
    uint16_t aux_c_l = 0;
    uint16_t r = 0;
    uint16_t aux_a_r = 0;
    uint16_t aux_b_r = 0;
    uint16_t aux_c_r = 0;
    uint16_t s = 0;
    uint16_t aux_a_s = 0;
    uint16_t aux_b_s = 0;
    uint16_t aux_c_s = 0;
};

// The volume envelope: the current volume and the change per sample.
struct PbVe
{
    uint16_t current_volume = 0;
    uint16_t current_delta = 0; // signed
};

// The sample addresses, in the accelerator's units: nibbles for ADPCM, bytes for PCM8 and 16-bit words for PCM16.
struct PbAddr
{
    uint16_t loop_flag = 0;
    uint16_t format = 0;
    uint16_t loop_address_hi = 0;
    uint16_t loop_address_lo = 0;
    uint16_t end_address_hi = 0;
    uint16_t end_address_lo = 0;
    uint16_t current_address_hi = 0;
    uint16_t current_address_lo = 0;
};

// The ADPCM decoder: the coefficient pairs, the gain and the decoder state. PCM uses the gain as its scale.
struct PbAdpcm
{
    std::array<uint16_t, 16> a{};
    uint16_t gain = 0;
    uint16_t pred_scale = 0;
    uint16_t yn1 = 0;
    uint16_t yn2 = 0;
};

// The sample rate converter: the ratio as 16.16 fixed point, the position between samples and the last four samples.
struct PbSrc
{
    uint16_t ratio_hi = 0;
    uint16_t ratio_lo = 0;
    uint16_t current_address_frac = 0;
    std::array<uint16_t, 4> last_samples{};
};

// The ADPCM decoder state at the loop start.
struct PbAdpcmLoop
{
    uint16_t loop_pred_scale = 0;
    uint16_t loop_yn1 = 0;
    uint16_t loop_yn2 = 0;
};

// A one-pole low-pass filter.
struct PbLpf
{
    uint16_t on = 0;
    uint16_t yn1 = 0;
    uint16_t a0 = 0;
    uint16_t b0 = 0;
};

struct PbBiquad
{
    uint16_t on = 0;
    uint16_t xn1 = 0;
    uint16_t xn2 = 0;
    uint16_t yn1 = 0;
    uint16_t yn2 = 0;
    uint16_t b0 = 0;
    uint16_t b1 = 0;
    uint16_t b2 = 0;
    uint16_t a1 = 0;
    uint16_t a2 = 0;
};

// The mixes to the four Wii Remote speakers.
struct PbRmtMix
{
    uint16_t main0 = 0;
    uint16_t delta_main0 = 0;
    uint16_t aux0 = 0;
    uint16_t delta_aux0 = 0;
    uint16_t main1 = 0;
    uint16_t delta_main1 = 0;
    uint16_t aux1 = 0;
    uint16_t delta_aux1 = 0;
    uint16_t main2 = 0;
    uint16_t delta_main2 = 0;
    uint16_t aux2 = 0;
    uint16_t delta_aux2 = 0;
    uint16_t main3 = 0;
    uint16_t delta_main3 = 0;
    uint16_t aux3 = 0;
    uint16_t delta_aux3 = 0;
};

struct PbRmtDpop
{
    uint16_t main0 = 0;
    uint16_t main1 = 0;
    uint16_t main2 = 0;
    uint16_t main3 = 0;
    uint16_t aux0 = 0;
    uint16_t aux1 = 0;
    uint16_t aux2 = 0;
    uint16_t aux3 = 0;
};

struct PbRmtSrc
{
    uint16_t current_address_frac = 0;
    std::array<uint16_t, 4> last_samples{};
};

struct Pb
{
    uint32_t MixerCtrl() const
    {
        return uint32_t{mixer_ctrl_hi} << 16 | mixer_ctrl_lo;
    }

    void SetMixerCtrl(uint32_t ctrl)
    {
        mixer_ctrl_hi = static_cast<uint16_t>(ctrl >> 16);
        mixer_ctrl_lo = static_cast<uint16_t>(ctrl);
    }

    uint16_t next_hi = 0; // + 0x0: the next PB in the microcode's list, or 0 for the last
    uint16_t next_lo = 0;
    uint16_t curr_hi = 0; // + 0x4: this PB's address
    uint16_t curr_lo = 0;
    uint16_t src_select = 0;    // + 0x8: the SrcMode
    uint16_t coef_select = 0;   // + 0xa: the polyphase coefficients: 0 8 kHz, 1 12 kHz, 2 16 kHz
    uint16_t mixer_ctrl_hi = 0; // + 0xc
    uint16_t mixer_ctrl_lo = 0;
    uint16_t state = 0;          // + 0x10
    uint16_t type = 0;           // + 0x12: the VoiceType
    PbMix mix;                   // + 0x14
    PbItd itd;                   // + 0x44
    PbDpop dpop;                 // + 0x52
    PbVe ve;                     // + 0x6a
    PbAddr addr;                 // + 0x6e
    PbAdpcm adpcm;               // + 0x7e
    PbSrc src;                   // + 0xa6
    PbAdpcmLoop adpcm_loop;      // + 0xb4
    PbLpf lpf;                   // + 0xba
    PbBiquad biquad;             // + 0xc2
    uint16_t remote = 0;         // + 0xd6: whether the voice plays on the Wii Remotes
    uint16_t rmt_mixer_ctrl = 0; // + 0xd8
    PbRmtMix rmt_mix;            // + 0xda
    PbRmtDpop rmt_dpop;          // + 0xfa
    PbRmtSrc rmt_src;            // + 0x10a
    PbBiquad rmt_iir;            // + 0x114: as a low-pass filter (on = 1), the words are on, yn1, a0 and b0
    std::array<uint16_t, 12> padding{};
};

constexpr std::size_t kPbSize = 0x140;

static_assert(sizeof(Pb) == kPbSize);
static_assert(offsetof(Pb, mix) == 0x14 && offsetof(Pb, itd) == 0x44 && offsetof(Pb, dpop) == 0x52);
static_assert(offsetof(Pb, ve) == 0x6a && offsetof(Pb, addr) == 0x6e && offsetof(Pb, adpcm) == 0x7e);
static_assert(offsetof(Pb, src) == 0xa6 && offsetof(Pb, adpcm_loop) == 0xb4 && offsetof(Pb, lpf) == 0xba);
static_assert(offsetof(Pb, biquad) == 0xc2 && offsetof(Pb, remote) == 0xd6 && offsetof(Pb, rmt_mix) == 0xda);
static_assert(offsetof(Pb, rmt_dpop) == 0xfa && offsetof(Pb, rmt_src) == 0x10a && offsetof(Pb, rmt_iir) == 0x114);

// Joins the two words of a 32-bit address.
constexpr uint32_t Join(uint16_t hi, uint16_t lo)
{
    return uint32_t{hi} << 16 | lo;
}

// Reads a PB from main memory and writes a PB back.
Pb LoadPb(const uint8_t* bytes);
void StorePb(const Pb& pb, uint8_t* bytes);

// The PB of the build of October 2006 (__AXServiceVPB, 0x80030534 in Smooth Moves' main.dol). Its fields up to the ITD
// are at the same offsets. An update block follows at 0x52: three counters and the address of a buffer of changes that
// the microcode makes within a frame. The model never queues such changes. Then come the fields from the depop values
// to the low-pass filter, at 0x5c. A second filter of four words follows at 0xcc. The model leaves it off. The Wii
// Remote's fields up to its sample rate converter start at 0xd4. The rest is padding. This PB has no biquad filter and
// no filter for the Wii Remotes.
constexpr std::size_t kPbSizeOctober2006 = 0x120;
constexpr std::size_t kUpdateBlockOctober2006 = 0x52;

// Reads the fields that both layouts have. The rest of the Pb is zero.
Pb LoadPbOctober2006(const uint8_t* bytes);

// Writes the fields that both layouts have. Leaves the update block, the second filter and the padding as they are.
void StorePbOctober2006(const Pb& pb, uint8_t* bytes);

} // namespace ntdlib::rvl::ax

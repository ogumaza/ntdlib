// SPDX-License-Identifier: MIT

// Checks the mappings of sar/encodings.h between the wave encodings of the DS (SWAV's format byte: 0 for PCM8, 1 for
// PCM16 and 2 for IMA-ADPCM), the Wii (RWAV's: 0 for PCM8, 1 for PCM16 and 2 for DSP-ADPCM) and the later consoles
// (CWAV's and FWAV's: those of the Wii, and 3 for the 3DS's IMA-ADPCM). The readers take each enum's value from the
// byte. The DS has no DSP-ADPCM and the Wii no IMA-ADPCM. rsar/convert.h converts the Wii's waves and streams with
// these mappings.

#include <array>
#include <optional>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/testing/check.h"

namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;
using xsflib::testing::Check;

void TestDs()
{
    static_assert(sar::EncodingOf(sdat::WaveEncoding::kImaAdpcm) == sar::WaveEncoding::kImaAdpcm);

    Check(sar::EncodingOf(sdat::WaveEncoding::kPcm8) == sar::WaveEncoding::kPcm8 &&
              sar::EncodingOf(sdat::WaveEncoding::kPcm16) == sar::WaveEncoding::kPcm16,
          "the DS's PCM is the later consoles' PCM");
    Check(sar::EncodingOf(static_cast<sdat::WaveEncoding>(3)) == sar::WaveEncoding::kImaAdpcm,
          "a DS encoding outside the enum gives IMA-ADPCM");
    Check(sdat::EncodingOf(sar::WaveEncoding::kPcm8) == sdat::WaveEncoding::kPcm8 &&
              sdat::EncodingOf(sar::WaveEncoding::kPcm16) == sdat::WaveEncoding::kPcm16 &&
              sdat::EncodingOf(sar::WaveEncoding::kImaAdpcm) == sdat::WaveEncoding::kImaAdpcm,
          "the later consoles' PCM and IMA-ADPCM are the DS's");
    Check(!sdat::EncodingOf(sar::WaveEncoding::kDspAdpcm) && !sdat::EncodingOf(static_cast<sar::WaveEncoding>(4)),
          "DSP-ADPCM, and a value outside the enum, have no DS encoding");
}

void TestWii()
{
    static_assert(rsar::EncodingOf(sar::WaveEncoding::kDspAdpcm) == rsar::WaveEncoding::kDspAdpcm);

    Check(sar::EncodingOf(rsar::WaveEncoding::kPcm8) == sar::WaveEncoding::kPcm8 &&
              sar::EncodingOf(rsar::WaveEncoding::kPcm16) == sar::WaveEncoding::kPcm16 &&
              sar::EncodingOf(rsar::WaveEncoding::kDspAdpcm) == sar::WaveEncoding::kDspAdpcm,
          "each of the Wii's encodings is the later consoles' encoding");
    Check(sar::EncodingOf(static_cast<rsar::WaveEncoding>(3)) == sar::WaveEncoding::kPcm16,
          "a Wii encoding outside the enum gives PCM16");
    Check(rsar::EncodingOf(sar::WaveEncoding::kPcm8) == rsar::WaveEncoding::kPcm8 &&
              rsar::EncodingOf(sar::WaveEncoding::kPcm16) == rsar::WaveEncoding::kPcm16,
          "the later consoles' PCM is the Wii's");
    Check(!rsar::EncodingOf(sar::WaveEncoding::kImaAdpcm) && !rsar::EncodingOf(static_cast<sar::WaveEncoding>(4)),
          "IMA-ADPCM, and a value outside the enum, have no Wii encoding");
}

// Every encoding that the other console plays comes back as itself.
void TestRoundTrips()
{
    constexpr std::array kDs = {sdat::WaveEncoding::kPcm8, sdat::WaveEncoding::kPcm16, sdat::WaveEncoding::kImaAdpcm};
    constexpr std::array kWii = {rsar::WaveEncoding::kPcm8, rsar::WaveEncoding::kPcm16, rsar::WaveEncoding::kDspAdpcm};

    for (const sdat::WaveEncoding encoding : kDs)
    {
        Check(sdat::EncodingOf(sar::EncodingOf(encoding)) == encoding, "a DS encoding comes back");
    }
    for (const rsar::WaveEncoding encoding : kWii)
    {
        Check(rsar::EncodingOf(sar::EncodingOf(encoding)) == encoding, "a Wii encoding comes back");
    }
}

} // namespace

int main()
{
    TestDs();
    TestWii();
    TestRoundTrips();

    return xsflib::testing::Summary("sar_encodings");
}

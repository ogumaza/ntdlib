// SPDX-License-Identifier: MIT

// Checks that AX's microcode is found in a main.dol and identifies the build of AX, and that NW4R SND's banner dates
// its build. Both rules for damaged files are checked as well.

#include "ntdlib/sound_build.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/testing/dol_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

using ntdlib::testing::MicrocodeDol;
using xsflib::testing::Check;
using xsflib::testing::failures;

namespace
{

// `text` between runs of zeros.
std::vector<uint8_t> Padded(const std::string& text)
{
    std::vector<uint8_t> bytes(64, 0);
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.resize(bytes.size() + 64, 0);

    return bytes;
}

void TestMicrocodeIdentifiesAx()
{
    // AX of October 2006 reads 0x120-byte PBs, and every later AX 0x140 bytes. A file without AX's microcode has none.
    const std::vector<uint8_t> old_dol = MicrocodeDol(0x120, "");
    const std::vector<uint8_t> new_dol = MicrocodeDol(0x140, "");
    const std::vector<uint8_t> zeros(0x3000, 0);

    const std::optional<ntdlib::Microcode> old_ax = ntdlib::FindMicrocode(old_dol);
    const std::optional<ntdlib::Microcode> new_ax = ntdlib::FindMicrocode(new_dol);
    const std::optional<ntdlib::Microcode> none = ntdlib::FindMicrocode(zeros);
    const std::optional<ntdlib::Microcode> empty = ntdlib::FindMicrocode({});

    XSFLIB_CHECK(old_ax && old_ax->address == 0x80004000 && old_ax->words.size() == ntdlib::kMicrocodeWords &&
                 old_ax->words[2] == 0x029f && old_ax->words[0x81] == 0x0120);
    XSFLIB_CHECK(old_ax && ntdlib::IdentifyBuild(*old_ax) == ntdlib::AxBuild::kOctober2006);
    XSFLIB_CHECK(new_ax && ntdlib::IdentifyBuild(*new_ax) == ntdlib::AxBuild::kDecember2006);
    XSFLIB_CHECK(!none && !empty);
}

void TestDamagedSectionIsLeftOutOnRequest()
{
    // A damaged header places the first text section past the file's end. By default, FindMicrocode finds no microcode
    // in such a file. With skip_damaged_sections, it leaves the section out and finds the microcode in the data
    // section.
    std::vector<uint8_t> dol = MicrocodeDol(0x140, "");
    xsflib::StoreBe32(&dol[0], 0x100000);
    xsflib::StoreBe32(&dol[0x90], 0x100);

    const std::optional<ntdlib::Microcode> by_default = ntdlib::FindMicrocode(dol);
    const std::optional<ntdlib::Microcode> skipping = ntdlib::FindMicrocode(dol, {.skip_damaged_sections = true});

    XSFLIB_CHECK(!by_default);
    XSFLIB_CHECK(skipping && skipping->address == 0x80004000);
}

void TestBannerDatesTheBuild()
{
    // NW4R SND's banner gives the build's date. Another library's banner gives none.
    const uint32_t final_build =
        ntdlib::FindSndBuild(Padded("<< NW4R    - SND \tfinal   build: Mar  8 2008 20:59:14 (0x4201_127) >>")).date;
    const uint32_t release_build =
        ntdlib::FindSndBuild(Padded("<< NW4R    - SND \trelease build: Nov 20 2008 03:40:59 (0x4199_60831) >>")).date;
    const uint32_t other_library =
        ntdlib::FindSndBuild(Padded("<< NW4R    - G3D \tfinal   build: Jun  8 2007 11:17:15 (0x4199_60831) >>")).date;
    const uint32_t no_banner = ntdlib::FindSndBuild(Padded("no banner")).date;

    Check(final_build == 20080308, "a build's date");
    Check(release_build == 20081120, "a release build's date");
    Check(other_library == 0, "another library's banner");
    Check(no_banner == 0, "no banner");
}

void TestBuildsFollowTheirDates()
{
    // The banner follows AX's microcode in a main.dol. It says nothing of AX's build.
    const std::vector<uint8_t> wii_music =
        MicrocodeDol(0x140, "<< NW4R    - SND \tfinal   build: Jun 20 2008 11:01:02 (0x4302_158) >>");
    const std::vector<uint8_t> mario_kart =
        MicrocodeDol(0x140, "<< NW4R    - SND \tfinal   build: Mar  8 2008 10:59:21 (0x4302_145) >>");

    const ntdlib::SndBuild june = ntdlib::FindSndBuild(wii_music);
    const ntdlib::SndBuild march = ntdlib::FindSndBuild(mario_kart);

    Check(june.date == 20080620 && june.SinceJune2008() && june.Since2008() && !june.October2006(),
          "Wii Music's build");
    Check(march.Since2008() && !march.SinceJune2008(), "Mario Kart Wii's build");
}

void TestDamagedDayIsCheckedOnRequest()
{
    // With check_day, a damaged day gives no date. Without it, the day is read anyway. Both read a real day.
    const std::vector<uint8_t> damaged =
        Padded("<< NW4R    - SND \tfinal   build: Jun 2x 2007 11:17:15 (0x4199_60831) >>");
    const std::vector<uint8_t> real =
        Padded("<< NW4R    - SND \tfinal   build: Jun  8 2007 11:17:15 (0x4199_60831) >>");

    const uint32_t checked = ntdlib::FindSndBuild(damaged, {.check_day = true}).date;
    const uint32_t unchecked = ntdlib::FindSndBuild(damaged).date;
    const uint32_t real_checked = ntdlib::FindSndBuild(real, {.check_day = true}).date;

    Check(checked == 0, "a damaged day with the check");
    Check(unchecked != 0, "a damaged day without the check");
    Check(real_checked == 20070608, "a day before the 10th with the check");
}

} // namespace

int main()
{
    TestMicrocodeIdentifiesAx();
    TestDamagedSectionIsLeftOutOnRequest();
    TestBannerDatesTheBuild();
    TestBuildsFollowTheirDates();
    TestDamagedDayIsCheckedOnRequest();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}

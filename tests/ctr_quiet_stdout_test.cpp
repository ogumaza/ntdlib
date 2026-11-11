// SPDX-License-Identifier: MIT

// Checks SilenceStdout with stdout closed. No game data needed.

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <filesystem>

#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "ntdlib/ctr/dsp/quiet_stdout.h"

using namespace xsflib::testing;

namespace
{

// With stdout closed, SilenceStdout keeps descriptor 1 on the null device. The next file that the tool opens doesn't
// take descriptor 1, and the text on stdout doesn't reach the file. The test closes stdout and restores it at the end.
// Windows reopens the stream on NUL instead.
void TestStdoutClosed()
{
#if !defined(_WIN32)
    const TempFolder folder("ntdlib_ctr_quiet_stdout_test");
    const std::filesystem::path path = folder.Path() / "rip.bin";
    struct stat null_device = {};
    XSFLIB_CHECK(stat("/dev/null", &null_device) == 0);
    std::fflush(stdout);
    const int saved = dup(1);
    close(1);

    ntdlib::ctr::SilenceStdout();
    const int next = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::printf("text on stdout\n");
    std::fflush(stdout);
    struct stat out = {};
    const bool out_known = fstat(1, &out) == 0;
    close(next);
    dup2(saved, 1);
    close(saved);

    XSFLIB_CHECK(next >= 0 && next != 1);
    XSFLIB_CHECK(out_known && S_ISCHR(out.st_mode) && out.st_rdev == null_device.st_rdev);
    XSFLIB_CHECK(std::filesystem::file_size(path) == 0);
#endif
}

} // namespace

int main()
{
    TestStdoutClosed();

    return Summary("ctr quiet stdout");
}

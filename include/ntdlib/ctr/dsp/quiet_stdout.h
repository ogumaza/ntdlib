// SPDX-License-Identifier: MIT

// Teakra prints unmodelled MMIO accesses to stdout. Tools that want a clean stdout call SilenceStdout() and write their
// own output to stderr (or files).

#pragma once

#include <cstdio>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ntdlib::ctr
{

inline void SilenceStdout()
{
    std::fflush(stdout);
#if defined(_WIN32)
    std::freopen("NUL", "w", stdout);
#else
    // A tool started with stdout closed gets descriptor 1 for the null device itself. That descriptor stays open. If it
    // were closed, the next file that the tool opens would take descriptor 1 and receive Teakra's messages.
    const int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0 && fd != 1)
    {
        dup2(fd, 1);
        close(fd);
    }
#endif
}

} // namespace ntdlib::ctr

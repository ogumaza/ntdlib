// SPDX-License-Identifier: MIT

// A check that a call throws an exception whose message contains some words.

#pragma once

#include <string>

#include "xsflib/testing/check.h"

namespace ntdlib::testing
{

// Whether `call` throws an `Error` whose message contains `part`. An exception of another type goes on to the caller.
template <class Error, class Call>
bool ThrowsAbout(Call call, const std::string& part)
{
    const std::string message = xsflib::testing::ErrorOf<Error>(call);

    return !message.empty() && message.find(part) != std::string::npos;
}

} // namespace ntdlib::testing

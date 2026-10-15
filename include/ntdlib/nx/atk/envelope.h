// SPDX-License-Identifier: MIT

// nn::atk's envelope (CurveAdshr) and LFO (CurveLfo): ntdlib's model of them, with the Switch's policy. nw::snd has the
// same envelope and LFO for the Wii U, apart from their rounding and the LFO's sine.

#pragma once

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nx/atk/util.h"

namespace ntdlib::nx::atk
{

using CurveAdshr = ntdlib::nwsnd::CurveAdshr<ntdlib::nwsnd::NxPolicy>;
using CurveLfo = ntdlib::nwsnd::CurveLfo<ntdlib::nwsnd::NxPolicy>;

} // namespace ntdlib::nx::atk

// SPDX-License-Identifier: MIT

#pragma once

#include <cfenv>

namespace ntdlib::n64
{

// Some games run their sound threads with the FPU rounding toward zero. Pokémon Stadium and Pokémon Stadium 2 do. A
// model's arithmetic follows that while the guard lives. Each file that uses the guard is compiled to respect the
// rounding mode.
class RoundTowardZero
{
public:
    explicit RoundTowardZero(bool active) : active_(active), saved_(active ? std::fegetround() : 0)
    {
        if (active_)
        {
            std::fesetround(FE_TOWARDZERO);
        }
    }

    ~RoundTowardZero()
    {
        if (active_)
        {
            std::fesetround(saved_);
        }
    }

    RoundTowardZero(const RoundTowardZero&) = delete;
    RoundTowardZero& operator=(const RoundTowardZero&) = delete;

private:
    bool active_;
    int saved_;
};

} // namespace ntdlib::n64

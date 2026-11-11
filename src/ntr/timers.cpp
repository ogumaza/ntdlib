// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/timers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ntdlib::ntr
{

namespace
{

// TMxCNT_H's bits.
constexpr uint16_t kCascade = 1u << 2;
constexpr uint16_t kIrq = 1u << 6;
constexpr uint16_t kStart = 1u << 7;
constexpr uint16_t kWritable = 0x00C7;

// The prescalers, by TMxCNT_H's bits 0 and 1.
constexpr std::array<uint64_t, 4> kPrescales = {1, 64, 256, 1024};

} // namespace

uint16_t Timers::Counter(int timer, uint64_t clock) const
{
    const Timer& t = timers_[static_cast<std::size_t>(timer)];
    if (!Counting(timer))
    {
        return t.counter;
    }

    return static_cast<uint16_t>(t.counter + (clock - t.base) / Prescale(timer));
}

uint16_t Timers::Reload(int timer) const
{
    return timers_[static_cast<std::size_t>(timer)].reload;
}

void Timers::SetReload(int timer, uint16_t reload)
{
    timers_[static_cast<std::size_t>(timer)].reload = reload;
}

uint16_t Timers::Control(int timer) const
{
    return timers_[static_cast<std::size_t>(timer)].control;
}

void Timers::SetControl(int timer, uint16_t control, uint64_t clock)
{
    Rebase(timer, clock);
    Timer& t = timers_[static_cast<std::size_t>(timer)];
    const uint16_t old = t.control;
    t.control = static_cast<uint16_t>(control & kWritable);
    if ((old & kStart) == 0 && (control & kStart) != 0)
    {
        t.counter = t.reload;
        t.base = clock;
    }
    else if ((old & 3) != (control & 3))
    {
        t.base = clock;
    }
}

uint64_t Timers::NextOverflow(uint64_t next) const
{
    for (int timer = 0; timer < 4; timer++)
    {
        if (Counting(timer))
        {
            next = std::min(next, OverflowClock(timer));
        }
    }

    return next;
}

uint32_t Timers::Overflow(uint64_t at)
{
    uint32_t irqs = 0;
    for (int timer = 0; timer < 4; timer++)
    {
        if (Counting(timer) && OverflowClock(timer) == at)
        {
            irqs |= OverflowFrom(timer, at);
        }
    }

    return irqs;
}

bool Timers::Counting(int timer) const
{
    const uint16_t control = timers_[static_cast<std::size_t>(timer)].control;
    return (control & kStart) != 0 && (timer == 0 || (control & kCascade) == 0);
}

uint64_t Timers::Prescale(int timer) const
{
    return kPrescales[timers_[static_cast<std::size_t>(timer)].control & 3];
}

uint64_t Timers::OverflowClock(int timer) const
{
    const Timer& t = timers_[static_cast<std::size_t>(timer)];
    return t.base + (0x10000 - static_cast<uint64_t>(t.counter)) * Prescale(timer);
}

void Timers::Rebase(int timer, uint64_t clock)
{
    if (!Counting(timer))
    {
        return;
    }

    Timer& t = timers_[static_cast<std::size_t>(timer)];
    const uint64_t counts = (clock - t.base) / Prescale(timer);
    t.counter = static_cast<uint16_t>(t.counter + counts);
    t.base += counts * Prescale(timer);
}

uint32_t Timers::OverflowFrom(int timer, uint64_t at)
{
    Timer& t = timers_[static_cast<std::size_t>(timer)];
    t.counter = t.reload;
    t.base = at;
    uint32_t irqs = (t.control & kIrq) != 0 ? 1u << timer : 0;

    if (timer < 3)
    {
        Timer& next = timers_[static_cast<std::size_t>(timer) + 1];
        if ((next.control & (kStart | kCascade)) == (kStart | kCascade))
        {
            if (next.counter == 0xFFFF)
            {
                irqs |= OverflowFrom(timer + 1, at);
            }
            else
            {
                next.counter++;
            }
        }
    }

    return irqs;
}

} // namespace ntdlib::ntr

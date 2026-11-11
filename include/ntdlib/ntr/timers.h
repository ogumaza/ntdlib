// SPDX-License-Identifier: MIT

// The four timers of one of the DS's CPUs (TM0CNT to TM3CNT). Both CPUs have the same timers. A timer counts the
// system's clock at the rate of its prescaler, or, with cascade, the overflows of the timer before it. The counters
// count only when they are read or overflow: each timer keeps its counter at a base clock.

#pragma once

#include <array>
#include <cstdint>

namespace ntdlib::ntr
{

class Timers
{
public:
    // TMxCNT_L as a read returns it: the counter of `timer` at `clock`.
    uint16_t Counter(int timer, uint64_t clock) const;

    // TMxCNT_L as a write sets it: the value that the counter starts from and goes back to after an overflow.
    uint16_t Reload(int timer) const;
    void SetReload(int timer, uint16_t reload);

    // TMxCNT_H. Starting the timer at `clock` loads its counter with the reload value. A new prescaler counts from
    // `clock`.
    uint16_t Control(int timer) const;
    void SetControl(int timer, uint16_t control, uint64_t clock);

    // The earlier of `next` and the clock of the next overflow of a timer that counts the clock.
    uint64_t NextOverflow(uint64_t next) const;

    // Overflows the timers that overflow at `at`, and the cascade timers that count those overflows. Returns the
    // timers that request an IRQ: bit 0 for timer 0 to bit 3 for timer 3.
    uint32_t Overflow(uint64_t at);

private:
    struct Timer
    {
        uint16_t reload = 0;
        uint16_t control = 0;
        uint16_t counter = 0; // the counter at `base`
        uint64_t base = 0;    // the clock at which the counter held `counter`
    };

    // Whether `timer` counts the clock: it runs and doesn't cascade. Timer 0 has no timer before it, and counts the
    // clock with cascade too.
    bool Counting(int timer) const;

    // The system's cycles of each count of `timer`.
    uint64_t Prescale(int timer) const;

    // The clock at which `timer` overflows, while it counts the clock.
    uint64_t OverflowClock(int timer) const;

    // Moves the base of `timer` to the last count at or before `clock`.
    void Rebase(int timer, uint64_t clock);

    // Overflows `timer` at `at`, and the cascade timers after it that the overflow carries into. Returns the timers
    // that request an IRQ in the same form as Overflow.
    uint32_t OverflowFrom(int timer, uint64_t at);

    std::array<Timer, 4> timers_ = {};
};

} // namespace ntdlib::ntr

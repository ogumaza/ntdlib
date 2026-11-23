// SPDX-License-Identifier: MIT

// The runs and the snapshot checks that the tests of JAudio2's and JAudio1's engines share. Both engines give
// RunSubframe, Output, Save, Restore and ActiveChannels.

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

namespace ntdlib::testing::jaudio_engine_checks
{

// The output of `count` sub-frames.
template <typename Engine>
std::vector<int16_t> Run(Engine& engine, int count)
{
    std::vector<int16_t> output;
    for (int i = 0; i < count; i++)
    {
        engine.RunSubframe();
        const std::span<const int16_t> subframe = engine.Output();
        output.insert(output.end(), subframe.begin(), subframe.end());
    }

    return output;
}

// The output of a run without snapshots: `before` sub-frames, then `after` more.
struct StraightRun
{
    std::vector<int16_t> head;
    std::vector<int16_t> tail;
};

// Checks the snapshots of an engine on `game`, with notes sounding after `before` sub-frames. `start` starts the sound
// in an engine and returns what the engine's Start returns, empty on success. A snapshot taken after `before`
// sub-frames and restored gives the output of the straight run, twice. A restore refuses a snapshot of another engine
// and one taken before the engine's current start. Returns the straight run.
template <typename Engine, typename Files, typename Start>
StraightRun CheckSnapshots(const Files& game, const Start& start, int before, int after)
{
    Engine straight(game, {});
    XSFLIB_CHECK(!start(straight));
    StraightRun run;
    run.head = Run(straight, before);
    run.tail = Run(straight, after);
    XSFLIB_CHECK(std::any_of(run.tail.begin(), run.tail.end(), [](int16_t sample) { return sample != 0; }));

    Engine engine(game, {});
    XSFLIB_CHECK(engine.Save().model == nullptr);
    XSFLIB_CHECK(!start(engine));
    XSFLIB_CHECK(Run(engine, before) == run.head);
    const typename Engine::Snapshot snapshot = engine.Save();
    XSFLIB_CHECK(engine.ActiveChannels() > 0);
    XSFLIB_CHECK(Run(engine, after) == run.tail);
    engine.Restore(snapshot);
    XSFLIB_CHECK(Run(engine, after) == run.tail);

    // A second restore of the same snapshot gives the same output again.
    engine.Restore(snapshot);
    XSFLIB_CHECK(Run(engine, after) == run.tail);

    const auto restore = [&snapshot](Engine& e)
    {
        return xsflib::testing::ErrorOf<std::logic_error>([&] { e.Restore(snapshot); });
    };
    XSFLIB_CHECK(!restore(straight).empty());
    XSFLIB_CHECK(!start(engine));
    XSFLIB_CHECK(!restore(engine).empty());

    return run;
}

} // namespace ntdlib::testing::jaudio_engine_checks

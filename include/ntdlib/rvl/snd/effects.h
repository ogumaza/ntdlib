// SPDX-License-Identifier: MIT

// NW4R SND's effects on AX's aux buses. Each effect class of NW4R SND wraps an effect of the
// SDK's AXFX library. AxManager keeps a list of effects for each bus. While a bus has effects, AxManager's callback for
// the bus runs them in each AX frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/axfx/chorus.h"
#include "ntdlib/rvl/axfx/delay.h"
#include "ntdlib/rvl/axfx/reverb_hi.h"
#include "ntdlib/rvl/axfx/reverb_std.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/util.h"

namespace ntdlib::rvl::snd
{

// The build of the AXFX library that a game links, by its build of NW4R SND.
axfx::Build AxfxBuild(const SndBuild& build);

// An object of one of NW4R SND's effect classes (FxBase), on the model of the AXFX effect that it wraps. A copy holds
// the whole state of the effect.
//
// The game gives each object a work buffer that holds the AXFX effect's memory. NW4R SND's StartUp and SetParam fail
// if the buffer is too small. The model leaves that out: the games give each effect a buffer large enough.
class Effect
{
public:
    Effect(EffectType type, axfx::Build build);

    // The class's SetParam: converts `params` into the AXFX effect's parameters. While the effect runs, the AXFX
    // effect's Settings then applies them. Returns false if Settings fails.
    bool SetParam(const EffectParams& params);

    // StartUp and Shutdown: the AXFX effect's Init and Shutdown.
    bool StartUp();
    void Shutdown();

    // UpdateBuffer: the AXFX effect's callback on a frame of the bus.
    void UpdateBuffer(axfx::AuxFrame& frame);

private:
    template <typename Fx>
    struct Slot
    {
        Fx fx;
        typename Fx::Params params{};
    };

    using Slots = std::variant<Slot<axfx::ReverbHi>, Slot<axfx::ReverbHiExp>, Slot<axfx::Chorus>, Slot<axfx::Delay>,
                               Slot<axfx::ReverbStd>, Slot<axfx::ReverbStdExp>>;

    static Slots MakeSlot(EffectType type, axfx::Build build);

    Slots slot_;
    bool active_ = false;
};

// The effects on the aux buses: the game's effect objects, AxManager's lists of effects, and what the game does with
// them for the sound that plays. A copy holds the whole state of the effects. The callbacks
// that it registers with AX point at the original.
class Effects
{
public:
    // Sets up an effect object for each of `setups`. The game does the same for the sound. The first AX frame after the
    // sound has started appends them (see OnAxFrame). With `follow_variables`, the sound's variables pick the presets
    // of an effect with presets. Otherwise its bus stays without it.
    void Set(const std::vector<EffectSetup>& setups, axfx::Build build, bool follow_variables);

    // Whether any effect is set up.
    bool Any() const
    {
        return !setups_.empty();
    }

    // The effects' part of AX's frame callback, after NW4R SND's. The game-mode driver does the same in its frame
    // callback (driver/driver.c). The first frame appends each effect without presets, in the order of the setups.
    // Then, in each frame, the sound's local variables 0 and 1 pick a preset, if the sound follows them (see
    // PickPreset). `variables` is nullptr while the sound has no player.
    void OnAxFrame(ax::System& ax, const std::array<int16_t, 16>* variables);

private:
    // AxManager::AppendEffect (0x80031d5c): starts up the object and adds it to the bus's list. A bus whose list was
    // empty registers AxManager's callback with AX. That callback then gives silence for two frames.
    bool Append(ax::System& ax, int bus, std::size_t object);

    // AxManager::ShutdownEffect (0x80032028): shuts down the bus's effects, empties its list and takes its callback
    // from AX. ClearEffect (0x80031f60) runs it when it doesn't fade.
    void Shutdown(ax::System& ax, int bus);

    // AxManager::AuxCallbackFunc (0x80032110): runs the bus's effects in turn on the frame.
    void AuxCallback(int bus, axfx::AuxFrame& frame);

    // WarioWare: Smooth Moves, 0x80100e40: `value` from a local variable of the music. A value from 1 to the number of
    // presets that differs from the last one sets that preset and appends the effect again. 0 takes the effects of the
    // bus away. Other values change nothing.
    void PickPreset(ax::System& ax, int16_t value);

    std::vector<EffectSetup> setups_;
    std::vector<Effect> objects_; // one for each setup
    bool follow_variables_ = false;
    bool pending_ = false; // whether the next AX frame appends the effects

    // The preset that the variables picked last, 0 for none, and whether its bus has the effect (WarioWare: Smooth
    // Moves keeps these at 0xa00 and 0xa06 of its sound object).
    int16_t preset_ = 0;
    bool preset_appended_ = false;

    // AxManager's lists: indices into objects_.
    std::array<std::vector<std::size_t>, ax::kAuxBusCount> lists_;
    std::array<int, ax::kAuxBusCount> wait_counters_{};
};

} // namespace ntdlib::rvl::snd

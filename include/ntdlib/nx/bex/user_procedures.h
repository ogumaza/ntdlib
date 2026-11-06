// SPDX-License-Identifier: MIT

// The user procedures of two games' sound layers on the Switch: the functions that a sequence calls with the command
// 0xF0E0 and a number (nn::atk's SequenceUserProcCallback). The procedures draw a number for a sound to pick one of its
// variants, count a sound's calls, or read a table. A sequence sets the slot and the count in two variables, calls the
// procedure, and then reads the result from the first variable, such as to transpose its note onto a variant's key.
//
// Super Mario Party Jamboree's handler is libbexsound's fn_103b80 in its main, with its state from fn_1039c0. Mario
// Party Superstars' handler is fn_479c60, called from fn_454110, with its state from fn_479c04. Each function's comment
// names the routine it follows. Procedures 2 to 4 of Jamboree's handler read the music sync (music_sync.h). This model
// leaves them to the caller.
//
// The games reseed every generator with one value when they start some of their scenes (Jamboree's fn_104290, and
// Superstars' fn_479d34 from bex::SetSyncRandSeed). A rip can't know that value. The model starts from the state that
// the handlers' constructors give. That is the state before any reseed.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ntdlib::nx::bex
{

// The handler of a game's user procedures. The numbers stay the same from release to release. A file can therefore
// hold them.
enum class ProcedureHandler : uint32_t
{
    kNone = 0,
    kJamboree = 1,   // Super Mario Party Jamboree: procedures 0, 1, 5 and 6 (fn_103b80)
    kSuperstars = 2, // Mario Party Superstars: procedure 0 (fn_479c60)
};

// The variables that a procedure sees: the sound's 16, the 16 global ones and the 16 of the track that calls it.
struct ProcedureVariables
{
    std::array<int16_t, 16> local{};
    std::array<int16_t, 16> global{};
    std::array<int16_t, 16> track{};
};

// The state of one sound system's handler. A copy plays on apart from the original. A model's snapshot can hold one.
class UserProcedures
{
public:
    static constexpr std::size_t kTableCount = 16;
    static constexpr std::size_t kTableSize = 128;

    // The constructor's state (fn_1039c0, fn_479c04): every generator at 1, no last result, and every count at 0.
    // Jamboree's handler has no table until SetTable loads one.
    explicit UserProcedures(ProcedureHandler handler = ProcedureHandler::kNone);

    ProcedureHandler Handler() const
    {
        return handler_;
    }

    // Loads table `index` of Jamboree's procedure 5 (fn_104318): the values of audio/userproc/conv/<index>.msgpack.
    // Indices past kTableCount change nothing.
    void SetTable(std::size_t index, std::span<const int16_t, kTableSize> values);

    // Runs procedure `procedure` on `variables`. Returns whether the handler runs that procedure. A procedure that it
    // doesn't run changes nothing.
    bool Run(uint16_t procedure, ProcedureVariables& variables);

    // Whether procedure `procedure` changes the handler's state: a draw or a count. Its next call can then give another
    // result for the same variables.
    bool KeepsState(uint16_t procedure) const;

    friend bool operator==(const UserProcedures&, const UserProcedures&) = default;

private:
    static constexpr std::size_t kJamboreeSlots = 512;
    static constexpr std::size_t kCounterSlots = 64;
    static constexpr std::size_t kSuperstarsSlots = 32;

    // Jamboree's procedures 0 and 1, each with its bank of generators (fn_103b80 at 0x103eac and 0x103f7c).
    void DrawJamboree(int bank, ProcedureVariables& variables);

    // Jamboree's procedure 5 (0x104158) and procedure 6 (0x1041c0).
    void ReadTable(ProcedureVariables& variables);
    void Count(ProcedureVariables& variables);

    // Superstars' procedure 0 (fn_479c60).
    void DrawSuperstars(ProcedureVariables& variables);

    ProcedureHandler handler_;
    std::array<std::array<uint32_t, kJamboreeSlots>, 2> states_{}; // each bank's generators (Superstars: bank 0)
    std::array<std::array<uint32_t, kJamboreeSlots>, 2> last_{};   // each generator's last result
    std::array<uint16_t, kCounterSlots> counters_{};
    std::array<bool, kTableCount> loaded_{};
    std::array<std::array<int16_t, kTableSize>, kTableCount> tables_{};
};

// Reads a table of Jamboree's procedure 5 from its file, audio/userproc/conv/<number>.msgpack (fn_fa440): a MessagePack
// map whose key "Value" holds an array of 128 integers. The game keeps each value's low 16 bits. Throws
// xsflib::FormatError for a file of another form.
std::array<int16_t, UserProcedures::kTableSize> ReadProcedureTable(std::span<const uint8_t> file);

} // namespace ntdlib::nx::bex

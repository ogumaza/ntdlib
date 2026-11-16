// SPDX-License-Identifier: MIT

// The static walk of a JAudio sequence's bytecode (jaudio/sequence_walk.h), on paths and sequences built for the test:
// the steps of the track's stack, the notes, the work list and its limit, the reads past a sequence's end, and each
// rule of the walks of JAudio2's and JAudio1's commands (jaudio1/sequence_walk.h).

#include "ntdlib/jaudio/sequence_walk.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio1/engine.h"
#include "ntdlib/jaudio1/sequence_walk.h"
#include "ntdlib/jaudio2/engine.h"
#include "ntdlib/testing/jaudio_files.h"

using namespace xsflib::testing;

namespace
{

namespace nj = ntdlib::jaudio;

using Path = nj::SequenceWalk::Path;
constexpr uint16_t kAny = nj::InstrumentUse::kAny;

// The paths that a walk gives, in turn, until none is left.
std::vector<Path> Drain(nj::SequenceWalk& walk)
{
    std::vector<Path> paths;
    Path path;
    while (walk.Next(path))
    {
        paths.push_back(path);
    }

    return paths;
}

// A path at `cursor` with `depth` entries on its stack: 100, 101 and so on.
Path At(uint32_t cursor, uint32_t depth = 0)
{
    Path path{.cursor = cursor, .bank = 0, .program = 0xf0, .depth = depth};
    for (uint32_t i = 0; i < depth; i++)
    {
        path.stack[i] = 100 + i;
    }

    return path;
}

// A call pushes the cursor after the command and moves to the target. At depth 8 it does nothing, and the path goes
// on after the command. A loop's start pushes the cursor too, and does nothing at depth 8.
void TestCallsAndLoopStarts()
{
    const std::vector<uint8_t> data(16, 0xfe);
    nj::SequenceWalk walk(data, At(0));
    Path path;
    XSFLIB_REQUIRE(walk.Next(path));
    path.cursor = 4;
    walk.Call(path, 10);
    std::vector<Path> paths = Drain(walk);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0].cursor == 10 && paths[0].depth == 1 && paths[0].stack[0] == 4);

    nj::SequenceWalk full(data, At(0, 8));
    XSFLIB_REQUIRE(full.Next(path));
    path.cursor = 4;
    full.Call(path, 10);
    paths = Drain(full);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0].cursor == 4 && paths[0].depth == 8 && paths[0].stack[7] == 107);

    nj::SequenceWalk loop(data, At(0, 1));
    XSFLIB_REQUIRE(loop.Next(path));
    path.cursor = 3;
    loop.LoopStart(path);
    paths = Drain(loop);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0].cursor == 3 && paths[0].depth == 2 && paths[0].stack[1] == 3);

    nj::SequenceWalk full_loop(data, At(0, 8));
    XSFLIB_REQUIRE(full_loop.Next(path));
    path.cursor = 3;
    full_loop.LoopStart(path);
    paths = Drain(full_loop);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0].cursor == 3 && paths[0].depth == 8);
}

// A return pops the top entry and goes there, and the entry reads 0 again. At depth 0 the path goes on where the
// engine does nothing, and ends where the track ends.
void TestReturns()
{
    const std::vector<uint8_t> data(16, 0xfe);
    Path path;
    nj::SequenceWalk walk(data, At(0, 2));
    XSFLIB_REQUIRE(walk.Next(path));
    path.cursor = 5;
    walk.Return(path, true);
    std::vector<Path> paths = Drain(walk);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0] == At(101, 1));

    nj::SequenceWalk goes_on(data, At(0));
    XSFLIB_REQUIRE(goes_on.Next(path));
    path.cursor = 5;
    goes_on.Return(path, true);
    paths = Drain(goes_on);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0] == At(5));

    nj::SequenceWalk ends(data, At(0));
    XSFLIB_REQUIRE(ends.Next(path));
    path.cursor = 5;
    ends.Return(path, false);
    XSFLIB_CHECK(Drain(ends).empty());
}

// A loop's end at depth 0 goes on. Otherwise the walk takes both ways: back to the top entry with the stack as it is,
// and on with the entry popped. The engine jumps to the top entry whether a call or a loop pushed it.
void TestLoopEnds()
{
    const std::vector<uint8_t> data(16, 0xfe);
    Path path;
    nj::SequenceWalk at_0(data, At(0));
    XSFLIB_REQUIRE(at_0.Next(path));
    path.cursor = 6;
    at_0.LoopEnd(path);
    std::vector<Path> paths = Drain(at_0);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK(paths[0] == At(6));

    // The entry is a call's return, 4.
    nj::SequenceWalk over_call(data, At(0));
    XSFLIB_REQUIRE(over_call.Next(path));
    path.cursor = 4;
    over_call.Call(path, 10);
    XSFLIB_REQUIRE(over_call.Next(path));
    path.cursor = 11;
    over_call.LoopEnd(path);
    paths = Drain(over_call);
    XSFLIB_REQUIRE(paths.size() == 2);
    Path back = At(4);
    back.depth = 1;
    back.stack[0] = 4;
    XSFLIB_CHECK((paths[0] == At(11) && paths[1] == back) || (paths[0] == back && paths[1] == At(11)));
}

// A note adds the path's bank and program, unless the program plays the DSP's oscillator. A number from a register
// counts as one. The commands' offsets come out ascending, each once.
void TestNotesAndCommands()
{
    const std::vector<uint8_t> data(16, 0xfe);
    nj::SequenceWalk walk(data, At(8));
    Path path;
    XSFLIB_REQUIRE(walk.Next(path));
    walk.Note(path); // program 0xf0
    path.program = 0xff;
    walk.Note(path);
    path.bank = 3;
    path.program = 7;
    walk.Note(path);
    path.program = kAny;
    walk.Note(path);
    path.bank = kAny;
    path.program = 0x20;
    walk.Note(path);
    walk.Add(At(2));
    walk.Add(At(5));
    walk.Add(At(2));
    Drain(walk);
    const nj::SequenceInstruments result = walk.Finish();

    XSFLIB_CHECK(result.complete && result.stopped_by.empty());
    XSFLIB_CHECK((result.uses == std::set<nj::InstrumentUse>{{3, 7}, {3, kAny}, {kAny, 0x20}}));
    XSFLIB_CHECK((result.commands == std::vector<uint32_t>{2, 5, 8}));
}

// A path is walked once, whichever step gives it. Past the limit the walk is incomplete and gives no more paths.
void TestWorkListAndLimit()
{
    const std::vector<uint8_t> data(16, 0xfe);
    nj::SequenceWalk walk(data, At(0));
    Path path;
    XSFLIB_REQUIRE(walk.Next(path));
    walk.Add(At(0));
    walk.Child(0, 0, 0xf0);
    XSFLIB_CHECK(!walk.Next(path));

    nj::SequenceWalk limited(data, At(0), 3);
    XSFLIB_REQUIRE(limited.Next(path));
    limited.Add(At(1));
    limited.Add(At(2));
    XSFLIB_CHECK(limited.Finish().complete);

    nj::SequenceWalk past(data, At(0), 3);
    XSFLIB_REQUIRE(past.Next(path));
    past.Add(At(1));
    past.Add(At(2));
    past.Add(At(3));
    XSFLIB_CHECK(!past.Next(path));
    const nj::SequenceInstruments result = past.Finish();
    XSFLIB_CHECK(!result.complete && result.stopped_by == "more than 3 paths");

    // A stop keeps its first reason.
    nj::SequenceWalk stopped(data, At(0));
    stopped.Stop("first");
    stopped.Stop("second");
    XSFLIB_CHECK(!stopped.Next(path));
    XSFLIB_CHECK(stopped.Finish().stopped_by == "first");
}

// A child starts with an empty stack, at its start, with the bank and program it's given.
void TestChild()
{
    const std::vector<uint8_t> data(16, 0xfe);
    nj::SequenceWalk walk(data, At(0, 3));
    Path path;
    XSFLIB_REQUIRE(walk.Next(path));
    walk.Child(9, 2, 5);
    const std::vector<Path> paths = Drain(walk);
    XSFLIB_REQUIRE(paths.size() == 1);
    XSFLIB_CHECK((paths[0] == Path{.cursor = 9, .bank = 2, .program = 5}));
}

// A read past the end gives 0xff, a sequence's end, and the wider reads are big-endian.
void TestReads()
{
    const std::vector<uint8_t> data = {0x12, 0x34, 0x56};
    const nj::SequenceWalk walk(data, At(0));
    XSFLIB_CHECK(walk.U8(2) == 0x56 && walk.U8(3) == 0xff && walk.U8(0xffffffff) == 0xff);
    XSFLIB_CHECK(walk.U16(0) == 0x1234 && walk.U16(2) == 0x56ff);
    XSFLIB_CHECK(walk.U24(0) == 0x123456 && walk.U24(1) == 0x3456ff);
}

// The walk of a JAudio2 sequence of `bytes` from 0.
nj::SequenceInstruments Walk2(std::initializer_list<uint8_t> bytes)
{
    const std::vector<uint8_t> data(bytes);

    return nj::WalkSequence(data, 0);
}

using Uses = std::set<nj::InstrumentUse>;

// Notes: a byte below 0x80 with its flags and velocity, and for a gate note (id 0) a time of up to 4 bytes; 0xb1 and
// 0xb3. A track starts with bank 0 and program 0xf0, the DSP's oscillator.
void TestJaudio2Notes()
{
    XSFLIB_CHECK(Walk2({0x3c, 0x01, 0x64, 0xff}).uses.empty());

    nj::SequenceInstruments walk = Walk2({0xe2, 0x03, 0xe3, 0x05, 0x3c, 0x00, 0x64, 0x81, 0x80, 0x00, 0xff});
    XSFLIB_CHECK(walk.complete && walk.uses == (Uses{{3, 5}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 2, 4, 10}));

    // A time whose fourth byte has its top bit set ends there too.
    walk = Walk2({0x3c, 0x00, 0x64, 0x81, 0x82, 0x83, 0x84, 0xff});
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 7}));

    walk = Walk2({0xe1, 0x02, 0x07, 0xb1, 0x3c, 0x01, 0x64, 0xe3, 0x08, 0xb3, 0x00, 0x3c, 0x64, 0x00, 0x30, 0xff});
    XSFLIB_CHECK(walk.uses == (Uses{{2, 7}, {2, 8}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 3, 7, 9, 15}));

    // Note offs (0x80 to 0x8f) and 0xb0 with its byte read nothing else.
    walk = Walk2({0x81, 0x88, 0xb0, 0x01, 0xff});
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 1, 2, 4}));
}

// 0xc1 opens a child at its second argument with the path's bank and program. A start from a register stops the walk.
void TestJaudio2Children()
{
    nj::SequenceInstruments walk = Walk2(
        {0xe2, 0x04, 0xe3, 0x06, 0xc1, 0x00, 0x00, 0x00, 0x0c, 0xe3, 0x07, 0xff, 0xe3, 0x08, 0x3c, 0x01, 0x64, 0xff});
    XSFLIB_CHECK(walk.complete && walk.uses == (Uses{{4, 8}}));
    walk = Walk2({0xe2, 0x04, 0xc1, 0x00, 0x00, 0x00, 0x0a, 0xe2, 0x05, 0xff, 0x3c, 0x01, 0x64, 0xff});
    XSFLIB_CHECK(walk.uses.empty());
    walk = Walk2({0xe2, 0x04, 0xe3, 0x01, 0xc1, 0x00, 0x00, 0x00, 0x0c, 0xe2, 0x05, 0xff, 0x3c, 0x01, 0x64, 0xff});
    XSFLIB_CHECK(walk.uses == (Uses{{4, 1}}));

    walk = Walk2({0x91, 0x40, 0xc1, 0x00, 0x03, 0xff});
    XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("child") != std::string::npos);
}

// Calls, returns and jumps. A condition of 0 always holds: the walk then takes only the call or the jump. Any other
// condition, or one from a register, takes both ways. A return at depth 0 does nothing, and the path goes on.
void TestJaudio2Flow()
{
    // A call of a subroutine that sets program 1, then a note after the return.
    nj::SequenceInstruments walk =
        Walk2({0xe2, 0x02, 0xc3, 0x00, 0x00, 0x0a, 0x3c, 0x01, 0x64, 0xff, 0xe3, 0x01, 0xc5});
    XSFLIB_CHECK(walk.complete && walk.uses == (Uses{{2, 1}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 2, 6, 9, 10, 12}));

    // 0xc4 with condition 0 calls. With condition 1 it also goes on without the call.
    const auto call_if = [](uint8_t condition)
    {
        return Walk2({0xe2, 0x02, 0xe3, 0x03, 0xc4, condition, 0x00, 0x00, 0x0e, 0x3c, 0x01, 0x64, 0xff, 0xff, 0xe3,
                      0x01, 0xc5});
    };
    XSFLIB_CHECK(call_if(0).uses == (Uses{{2, 1}}));
    XSFLIB_CHECK(call_if(1).uses == (Uses{{2, 1}, {2, 3}}));
    walk = Walk2({0xe2, 0x02, 0xe3, 0x03, 0x90, 0x80, 0xc4, 0x00, 0x00, 0x00, 0x10, 0x3c, 0x01, 0x64, 0xff, 0xff, 0xe3,
                  0x01, 0xc5});
    XSFLIB_CHECK(walk.uses == (Uses{{2, 1}, {2, 3}}));

    // A return at depth 0 goes on. 0xc6 with condition 0 returns. With condition 1 it also goes on.
    walk = Walk2({0xe2, 0x02, 0xe3, 0x03, 0xc5, 0x3c, 0x01, 0x64, 0xff});
    XSFLIB_CHECK(walk.uses == (Uses{{2, 3}}));
    const auto return_if = [](uint8_t condition)
    {
        return Walk2({0xe2, 0x02, 0xc3, 0x00, 0x00, 0x0a, 0x3c, 0x01, 0x64, 0xff, 0xe3, 0x01, 0xc6, condition, 0xe3,
                      0x05, 0xc5});
    };
    XSFLIB_CHECK(return_if(0).uses == (Uses{{2, 1}}));
    XSFLIB_CHECK(return_if(1).uses == (Uses{{2, 1}, {2, 5}}));

    // 0xc7 jumps with no way on. 0xc8 with condition 0 jumps, and with condition 2 also goes on.
    walk = Walk2({0xe2, 0x02, 0xc7, 0x00, 0x00, 0x0a, 0xe3, 0x01, 0x3c, 0x01, 0xe3, 0x02, 0x3c, 0x01, 0x64, 0xff});
    XSFLIB_CHECK(walk.uses == (Uses{{2, 2}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 2, 10, 12, 15}));
    const auto jump_if = [](uint8_t condition)
    {
        return Walk2({0xe2, 0x02, 0xc8, condition, 0x00, 0x00, 0x0d, 0xe3, 0x01, 0x3c, 0x01, 0x64, 0xff, 0xe3, 0x02,
                      0x3c, 0x01, 0x64, 0xff});
    };
    XSFLIB_CHECK(jump_if(0).uses == (Uses{{2, 2}}));
    XSFLIB_CHECK(jump_if(2).uses == (Uses{{2, 1}, {2, 2}}));

    // A target from a register, and a jump or a call through a table, stop the walk.
    for (const uint8_t command : {0xc3, 0xc7})
    {
        walk = Walk2({0x90, 0x80, command, 0x03, 0xff});
        XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("register") != std::string::npos);
    }
    for (const uint8_t command : {0xc4, 0xc8})
    {
        walk = Walk2({0x91, 0x40, command, 0x00, 0x03, 0xff});
        XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("register") != std::string::npos);
    }
    for (const uint8_t command : {0xc9, 0xca})
    {
        walk = Walk2({command, 0x03, 0x00, 0x00, 0x10, 0xff});
        XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("table") != std::string::npos);
    }
}

// A loop's start (0xcb, a 16-bit count) and its end (0xcc). The walk leaves the loop and goes round it.
void TestJaudio2Loops()
{
    const nj::SequenceInstruments walk =
        Walk2({0xe2, 0x02, 0xe3, 0x01, 0xcb, 0x00, 0x04, 0x3c, 0x01, 0x64, 0xe3, 0x02, 0xcc, 0xe3, 0x03, 0xff});
    XSFLIB_CHECK(walk.complete && walk.uses == (Uses{{2, 1}, {2, 2}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 2, 4, 7, 10, 12, 13, 15}));
}

// The bank and the program. 0xe1 sets both from its 16-bit argument, 0xe2 the bank and 0xe3 the program. A value
// from a register is any number.
void TestJaudio2BankAndProgram()
{
    XSFLIB_CHECK(Walk2({0xe1, 0x12, 0x34, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{0x12, 0x34}}));
    XSFLIB_CHECK(Walk2({0x90, 0x80, 0xe1, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, kAny}}));
    XSFLIB_CHECK(Walk2({0xe3, 0x01, 0x90, 0x80, 0xe2, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(Walk2({0xe2, 0x01, 0x90, 0x80, 0xe3, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{1, kAny}}));
}

// The registers of the bank (0x67) and the program (0x68).
void TestJaudio2Registers()
{
    // 0xd8 loads a register. A value from a register is any number, and a register's number from a register can be
    // either.
    XSFLIB_CHECK(Walk2({0xd8, 0x67, 0x01, 0x05, 0xd8, 0x68, 0x00, 0x07, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{5, 7}}));
    XSFLIB_CHECK(Walk2({0xe3, 0x01, 0x91, 0x40, 0xd8, 0x67, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0x90, 0x80, 0xd8, 0x03, 0x00, 0x05, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{kAny, kAny}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xd8, 0x20, 0x00, 0x05, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{1, 1}}));

    // 0xda's operation 0 loads its 16-bit value, and 0xd9's comes from a register. Operations 3 and 4 write other
    // registers. Any other operation, or one from a register, makes the register any number.
    XSFLIB_CHECK(Walk2({0xe3, 0x01, 0xda, 0x00, 0x67, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{3, 1}}));
    XSFLIB_CHECK(Walk2({0xe3, 0x01, 0xd9, 0x00, 0x67, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, 1}}));
    for (const uint8_t operation : {3, 4})
    {
        XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xda, operation, 0x67, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                     (Uses{{1, 1}}));
    }
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xda, 0x01, 0x68, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{1, kAny}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0x90, 0x80, 0xda, 0x03, 0x68, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{1, kAny}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0x91, 0x40, 0xda, 0x00, 0x03, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{kAny, kAny}}));

    // The reads of ports (0xd0, 0xd6, 0xd7) and of a table (0xdc) write a value that the walk doesn't know.
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xd0, 0x00, 0x67, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xd6, 0x00, 0x68, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{1, kAny}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xd7, 0x00, 0x67, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xdc, 0x0c, 0x68, 0x00, 0x00, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{1, kAny}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0x91, 0x40, 0xd0, 0x00, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{kAny, kAny}}));

    // 0xdb negates the register for 0x0b, or for an operation from a register, and does nothing otherwise.
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xdb, 0x0b, 0x67, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0xdb, 0x01, 0x67, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{1, 1}}));
    XSFLIB_CHECK(Walk2({0xe1, 0x01, 0x01, 0x90, 0x80, 0xdb, 0x01, 0x67, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{kAny, 1}}));
}

// Interrupts stop the walk. The other commands that read more than their arguments: 0xf0's time and 0xfd's format.
void TestJaudio2OtherCommands()
{
    nj::SequenceInstruments walk = Walk2({0xf4, 0x00, 0x01, 0xff});
    XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("interrupts") != std::string::npos);

    // The format "a%d\n" with its byte for %d.
    walk = Walk2({0xf0, 0x81, 0x00, 0xfd, 0x61, 0x25, 0x64, 0x5c, 0x6e, 0x00, 0x05, 0xff});
    XSFLIB_CHECK(walk.complete && (walk.commands == std::vector<uint32_t>{0, 3, 11}));

    // A command that the table doesn't have reads no argument, and 0xff ends the path.
    walk = Walk2({0xa5, 0xff, 0x3c, 0x01, 0x64});
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 1}));
}

// The walk of a JAudio1 sequence of `bytes` from 0.
nj::SequenceInstruments Walk1(std::initializer_list<uint8_t> bytes)
{
    const std::vector<uint8_t> data(bytes);

    return ntdlib::jaudio1::WalkSequence(data);
}

// JAudio1's notes: a key below 0x80, its flags and its velocity. A note id other than 0 reads its skip's byte when bit
// 5 is set. Note 0 reads its gate's percentage and as many bytes of its length as bits 3 and 4 say. A track starts
// with bank 0 and program 0xf0. Waits (0x80 a byte, 0x88 16 bits) and note offs (a release byte with bit 3).
void TestJaudio1Notes()
{
    XSFLIB_CHECK(Walk1({0x3c, 0x01, 0x64, 0xff}).uses.empty());

    nj::SequenceInstruments walk = Walk1(
        {0xac, 0x06, 0x03, 0x05, 0x3c, 0x01, 0x64, 0x3c, 0x21, 0x64, 0x00, 0x3c, 0x10, 0x64, 0x64, 0x00, 0x30, 0xff});
    XSFLIB_CHECK(walk.complete && walk.uses == (Uses{{3, 5}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 4, 7, 11, 17}));

    walk = Walk1({0x80, 0x10, 0x88, 0x00, 0x10, 0x81, 0x89, 0x40, 0xf9, 0x81, 0x40, 0xff});
    XSFLIB_CHECK(walk.complete && (walk.commands == std::vector<uint32_t>{0, 2, 5, 6, 8, 11}));

    // 0xf9 without bit 7 of its byte takes the release byte's presence from a register.
    walk = Walk1({0xf9, 0x01, 0xff});
    XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("register") != std::string::npos);

    // 0x90 to 0x9f: a level, its value and its time, in the widths that the command's low bits give.
    walk = Walk1({0x90, 0x00, 0x03, 0x9a, 0x00, 0x40, 0x60, 0x9f, 0x00, 0x40, 0x00, 0x00, 0x60, 0xff});
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 3, 7, 13}));
}

// The writes of the registers 6 (the bank and the program), 0x20 (the bank) and 0x21 (the program), 0xa0 to 0xaf.
// Operation 0 with an operand that isn't a register stores a known value. Operations 2 and 3 store nothing. Any other
// operation stores any number.
void TestJaudio1Registers()
{
    const auto uses = [](std::initializer_list<uint8_t> before)
    {
        std::vector<uint8_t> bytes = {0xac, 0x06, 0x01, 0x01};
        bytes.insert(bytes.end(), before);
        bytes.insert(bytes.end(), {0x3c, 0x01, 0x64, 0xff});

        return ntdlib::jaudio1::WalkSequence(bytes).uses;
    };

    XSFLIB_CHECK(uses({0xac, 0x06, 0x12, 0x34}) == (Uses{{0x12, 0x34}}));
    XSFLIB_CHECK(uses({0xa4, 0x20, 0x03}) == (Uses{{3, 1}}));
    XSFLIB_CHECK(uses({0xa8, 0x21, 0x05}) == (Uses{{1, 0x0a}}));
    XSFLIB_CHECK(uses({0xa9, 0x04, 0x21, 0x07}) == (Uses{{1, 7}}));
    XSFLIB_CHECK(uses({0xa9, 0x08, 0x20}) == (Uses{{0xff, 1}}));
    XSFLIB_CHECK(uses({0xa9, 0x0c, 0x21, 0x00, 0x09}) == (Uses{{1, 9}}));
    XSFLIB_CHECK(uses({0xa0, 0x06, 0x03}) == (Uses{{kAny, kAny}}));
    XSFLIB_CHECK(uses({0xa5, 0x21, 0x01}) == (Uses{{1, kAny}}));
    XSFLIB_CHECK(uses({0xa6, 0x21, 0x01}) == (Uses{{1, 1}}));
    XSFLIB_CHECK(uses({0xa7, 0x20, 0x01}) == (Uses{{1, 1}}));
    XSFLIB_CHECK(uses({0xa9, 0x94, 0x20, 0x07}) == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(uses({0xaa, 0x04, 0x21, 0x03, 0x05}) == (Uses{{1, kAny}}));
    XSFLIB_CHECK(uses({0xab, 0x20, 0x03}) == (Uses{{kAny, 1}}));
    XSFLIB_CHECK(uses({0xa4, 0x07, 0x03}) == (Uses{{1, 1}}));

    // readPort (0xcb) to register 6, or to a register from a register. WriteRegDirect ignores 0x20 and 0x21.
    XSFLIB_CHECK(uses({0xcb, 0x00, 0x06}) == (Uses{{kAny, kAny}}));
    XSFLIB_CHECK(uses({0xcb, 0x00, 0x20}) == (Uses{{1, 1}}));
    XSFLIB_CHECK(uses({0xb1, 0xcb, 0x40, 0x00, 0x03}) == (Uses{{kAny, kAny}}));
}

// openTrack (0xc1) gives the child the path's bank and program, or bank 0 and program 0xf0 where the flags' bit 7
// keeps it from inheriting, and both from a register. openTrackBros (0xc2) gives any number. A start from a register
// stops the walk.
void TestJaudio1Children()
{
    const auto child = [](std::initializer_list<uint8_t> open)
    {
        std::vector<uint8_t> bytes = {0xac, 0x06, 0x02, 0x05};
        bytes.insert(bytes.end(), open);
        bytes.resize(0x10, 0xff);
        bytes.insert(bytes.end(), {0x3c, 0x01, 0x64, 0xa4, 0x21, 0x07, 0x3c, 0x01, 0x64, 0xff});

        return ntdlib::jaudio1::WalkSequence(bytes);
    };

    XSFLIB_CHECK(child({0xc1, 0x00, 0x00, 0x00, 0x10}).uses == (Uses{{2, 5}, {2, 7}}));
    XSFLIB_CHECK(child({0xc1, 0x80, 0x00, 0x00, 0x10}).uses == (Uses{{0, 7}}));
    XSFLIB_CHECK(child({0xc1, 0x40, 0x00, 0x00, 0x10}).uses == (Uses{{2, 5}, {2, 7}}));
    XSFLIB_CHECK(child({0xb1, 0xc1, 0x80, 0x03, 0x00, 0x00, 0x10}).uses == (Uses{{2, 5}, {2, 7}, {0, 7}}));
    XSFLIB_CHECK(child({0xc2, 0x00, 0x00, 0x00, 0x10}).uses == (Uses{{kAny, kAny}, {kAny, 7}}));

    for (const uint8_t command : {0xc1, 0xc2})
    {
        const nj::SequenceInstruments walk = Walk1({0xb1, command, 0x40, 0x00, 0x03, 0xff});
        XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("child") != std::string::npos);
    }
}

// call (0xc4) and jmp (0xc8): a flag, then a 24-bit target. A condition in the flag's low 4 bits other than 0 also
// goes on. A flag with bit 7 set takes the target from a register or a table, and stops the walk. ret (0xc6) ends the
// track at depth 0. Loops: loopS (0xc9) and loopE (0xca).
void TestJaudio1Flow()
{
    const auto call = [](uint8_t flag)
    {
        return Walk1({0xac, 0x06, 0x02, 0x03, 0xc4, flag, 0x00, 0x00, 0x0e, 0x3c, 0x01, 0x64, 0xff, 0xff, 0xa4, 0x21,
                      0x01, 0xc6, 0x00});
    };
    XSFLIB_CHECK(call(0x00).uses == (Uses{{2, 1}}));
    XSFLIB_CHECK(call(0x01).uses == (Uses{{2, 1}, {2, 3}}));

    const auto jump = [](uint8_t flag)
    {
        return Walk1({0xac, 0x06, 0x02, 0x03, 0xc8, flag, 0x00, 0x00, 0x0d, 0x3c,
                      0x01, 0x64, 0xff, 0xa4, 0x21, 0x01, 0x3c, 0x01, 0x64, 0xff});
    };
    XSFLIB_CHECK(jump(0x00).uses == (Uses{{2, 1}}));
    XSFLIB_CHECK(jump(0x05).uses == (Uses{{2, 1}, {2, 3}}));
    for (const uint8_t command : {0xc4, 0xc8})
    {
        const nj::SequenceInstruments walk = Walk1({command, 0x80, 0x03, 0xff});
        XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("register") != std::string::npos);
    }

    // ret at depth 0 ends the track. With a condition other than 0, or one from a register, it also goes on.
    XSFLIB_CHECK(Walk1({0xac, 0x06, 0x02, 0x03, 0xc6, 0x00, 0x3c, 0x01, 0x64, 0xff}).uses.empty());
    XSFLIB_CHECK(Walk1({0xac, 0x06, 0x02, 0x03, 0xc6, 0x02, 0x3c, 0x01, 0x64, 0xff}).uses == (Uses{{2, 3}}));
    XSFLIB_CHECK(Walk1({0xac, 0x06, 0x02, 0x03, 0xb0, 0xc6, 0x80, 0x03, 0x3c, 0x01, 0x64, 0xff}).uses ==
                 (Uses{{2, 3}}));

    const nj::SequenceInstruments walk = Walk1(
        {0xac, 0x06, 0x02, 0x01, 0xc9, 0x00, 0x04, 0x3c, 0x01, 0x64, 0xa4, 0x21, 0x02, 0xca, 0xa4, 0x21, 0x03, 0xff});
    XSFLIB_CHECK(walk.complete && walk.uses == (Uses{{2, 1}, {2, 2}}));
    XSFLIB_CHECK((walk.commands == std::vector<uint32_t>{0, 4, 7, 10, 13, 14, 17}));
}

// The other commands: interrupts (setInterrupt, 0xdf) and commands from a register (0xb8 to 0xbf) stop the walk.
// 0xb0 to 0xb7 read the command and the mask's byte. A command below 0xc0 does nothing there. A command without a
// handler reads its arguments. printf (0xfb) reads its format and a byte for each conversion.
void TestJaudio1OtherCommands()
{
    nj::SequenceInstruments walk = Walk1({0xdf, 0x00, 0x00, 0x00, 0x10, 0xff});
    XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("interrupts") != std::string::npos);
    walk = Walk1({0xb8, 0x03, 0xff});
    XSFLIB_CHECK(!walk.complete && walk.stopped_by.find("register") != std::string::npos);

    // The format "a%d\n" with its byte for %d.
    walk = Walk1({0xb0, 0xa5, 0x00, 0xc7, 0x00, 0x00, 0x20, 0xc3, 0x00, 0x00,
                  0x20, 0xfb, 0x61, 0x25, 0x64, 0x5c, 0x6e, 0x00, 0x05, 0xff});
    XSFLIB_CHECK(walk.complete && (walk.commands == std::vector<uint32_t>{0, 3, 7, 11, 19}));
}

// The decoding against the models: the walk and each engine's track must read a sequence's commands alike. Each check
// runs the model's tracks sub-frame by sub-frame. Between sub-frames, while the sequence plays, the cursor of every
// track of the tree must be the start of a command that the walk passed, and the cursors must reach the offsets that a
// reading of the bytes gives. JAudio2's root stays in the tree after its end, with its cursor past the 0xff.

namespace files = ntdlib::testing::jaudio_files;
namespace j1 = ntdlib::jaudio1;
namespace j2 = ntdlib::jaudio2;

constexpr int kModelFrames = 40;

// The cursors of the tracks of the tree, between the sub-frames of a run of `engine`, while the sequence plays.
template <typename Engine>
std::set<uint32_t> ModelCursors(Engine& engine)
{
    std::set<uint32_t> cursors;
    for (int i = 0; i < kModelFrames * Engine::kSubframesPerFrame; i++)
    {
        engine.RunSubframe();
        if (!engine.Playing())
        {
            break;
        }
        for (const uint32_t track : engine.TreeOrder())
        {
            cursors.insert(engine.Cursor(track));
        }
    }

    return cursors;
}

// JAudio2's model, on a sequence started as the only sound of a game.
std::set<uint32_t> ModelCursors2(const std::vector<uint8_t>& sequence)
{
    const files::SoundTableBlocks sounds = files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<uint8_t> startup = files::StartUpArchive(files::BankV1File(0, 10, 20000), {}, sounds);
    const std::vector<uint8_t> sequences = files::RarcFile({{7, "field.bms", sequence}});
    const std::vector<uint8_t> aw = files::AwFile();
    const j2::SoundFiles game(startup, sequences,
                              [&aw](const std::string& name) { return name == "test.aw" ? &aw : nullptr; });
    j2::Engine engine(game, {});
    if (engine.Start(game.Archive().Sounds().at(0).Id()))
    {
        return {};
    }

    return ModelCursors(engine);
}

// JAudio1's model, on a sequence started as a game's only piece.
std::set<uint32_t> ModelCursors1(const std::vector<uint8_t>& sequence)
{
    const std::vector<uint8_t> aaf =
        files::AafFile({{.category = 16, .file = 7}}, {"test.aw"}, 5, {}, false, files::BankV0File(0, 60, false));
    const std::vector<uint8_t> sequences =
        files::RarcFile({{7, "field.bms", sequence}}, false, ntdlib::jaudio::kWideRarcEntrySize);
    const std::vector<uint8_t> aw = files::AwFile();
    const j1::SoundFiles game(aaf, sequences,
                              [&aw](const std::string& name) { return name == "test.aw" ? &aw : nullptr; });
    j1::Engine engine(game, {});
    if (engine.Start(7, {}))
    {
        return {};
    }

    return ModelCursors(engine);
}

bool Passed(const nj::SequenceInstruments& walk, uint32_t offset)
{
    return std::binary_search(walk.commands.begin(), walk.commands.end(), offset);
}

// One command, `body`, with arguments of 0, then a wait and the sequence's end. The walk tries 0 to 12 bytes of
// arguments, and takes the first count after which it passes the wait and the end. The model must then stop only at
// commands that the walk passed, and reach the end. Returns what failed, or nothing.
std::string CheckCommand(const std::vector<uint8_t>& body, std::initializer_list<uint8_t> wait, bool jaudio1)
{
    std::string hex;
    for (const uint8_t byte : body)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 15];
    }

    for (uint32_t count = 0; count <= 12; count++)
    {
        std::vector<uint8_t> sequence = body;
        sequence.resize(body.size() + count, 0);
        const auto wait_at = static_cast<uint32_t>(sequence.size());
        sequence.insert(sequence.end(), wait);
        const auto end = static_cast<uint32_t>(sequence.size());
        sequence.push_back(0xff);

        const nj::SequenceInstruments walk = jaudio1 ? j1::WalkSequence(sequence) : nj::WalkSequence(sequence, 0);
        if (!walk.complete)
        {
            return hex + ": the walk stops (" + walk.stopped_by + ")";
        }
        if (!Passed(walk, wait_at) || !Passed(walk, end))
        {
            continue;
        }

        const std::set<uint32_t> cursors = jaudio1 ? ModelCursors1(sequence) : ModelCursors2(sequence);
        for (const uint32_t cursor : cursors)
        {
            if (!Passed(walk, cursor))
            {
                return hex + ": the model stops at " + std::to_string(cursor) + ", inside a command of the walk";
            }
        }
        if (!cursors.contains(end))
        {
            return hex + ": the model doesn't reach the end at " + std::to_string(end);
        }

        return {};
    }

    return hex + ": no count of arguments reaches the wait";
}

// Prints each failure, for the test's log, and checks that there is none.
void CheckNoFailures(const std::vector<std::string>& failures)
{
    for (const std::string& failure : failures)
    {
        std::fprintf(stderr, "%s\n", failure.c_str());
    }
    XSFLIB_CHECK(failures.empty());
}

// JAudio2: every first byte, and the prefixes 0x90 to 0x97 with each argument from a register, before every command
// from 0xa0 on. A note's arguments aren't all 0: a gate note of time 0 waits for its note to end. Each key plays as
// note 1, and as a gate note of 1 tick with a time of one byte and of two. 0xb3, a gate note with its arguments, gets a
// time of 1 tick: its 16-bit time, after flags, key and velocity from registers where a prefix has them. The sweep
// leaves out the commands that move the cursor, open a track or end one, and those that stop the walk: 0xc1, 0xc3,
// 0xc4, 0xc7, 0xc8, 0xc9, 0xca, 0xf4 and 0xff. TestJaudio2Model and TestJaudio2Flow check them.
void TestJaudio2Sweep()
{
    const auto left_out = [](uint32_t command)
    {
        static constexpr std::array<uint8_t, 9> kLeftOut = {0xc1, 0xc3, 0xc4, 0xc7, 0xc8, 0xc9, 0xca, 0xf4, 0xff};
        return std::find(kLeftOut.begin(), kLeftOut.end(), command) != kLeftOut.end();
    };

    std::vector<std::string> failures;
    const auto check = [&failures](const std::vector<uint8_t>& body)
    {
        if (std::string failure = CheckCommand(body, {0xf1, 0x08}, false); !failure.empty())
        {
            failures.push_back(std::move(failure));
        }
    };
    for (uint32_t command = 0; command < 0x100; command++)
    {
        const auto byte = static_cast<uint8_t>(command);
        if (command < 0x80)
        {
            check({byte, 0x01, 0x00});
            check({byte, 0x00, 0x00, 0x01});
            check({byte, 0x00, 0x00, 0x80, 0x01});
        }
        else if (command == 0xb3)
        {
            check({byte, 0x00, 0x3c, 0x40, 0x00, 0x01});
        }
        else if (!left_out(command))
        {
            check({byte});
        }
    }
    for (uint32_t prefix = 0x90; prefix < 0x98; prefix++)
    {
        const auto byte = static_cast<uint8_t>(prefix);
        for (uint32_t command = 0xa0; command < 0x100; command++)
        {
            if (command == 0xb3)
            {
                check({byte, 0xe0, 0xb3, 0x00, 0x00, 0x00, 0x00, 0x01});
            }
            else if (!left_out(command))
            {
                check({byte, 0xff, static_cast<uint8_t>(command)});
            }
        }
    }

    CheckNoFailures(failures);
}

// JAudio1: every first byte, and the prefixes 0xb0 to 0xb7 with each argument from a register, before every command
// from 0xc0 on. A note's arguments aren't all 0: note 0 without a length waits for its note to end. Each key plays as
// note 1, as note 1 with its skip's byte, and as note 0 of 1 tick with a length of one byte and of two. The sweep
// leaves out the commands that move the cursor, open a track or end one, and those that stop the walk: 0xc1, 0xc2,
// 0xc4, 0xc6, 0xc8, 0xdf, 0xb8 to 0xbf, 0xff, and 0xf9 before a byte without bit 7. TestJaudio1Model, TestJaudio1Flow,
// TestJaudio1Children, TestJaudio1OtherCommands and TestJaudio1Notes check them.
void TestJaudio1Sweep()
{
    const auto left_out = [](uint32_t command)
    {
        static constexpr std::array<uint8_t, 8> kLeftOut = {0xc1, 0xc2, 0xc4, 0xc6, 0xc8, 0xdf, 0xf9, 0xff};
        return (command >= 0xb8 && command < 0xc0) ||
               std::find(kLeftOut.begin(), kLeftOut.end(), command) != kLeftOut.end();
    };

    std::vector<std::string> failures;
    const auto check = [&failures](const std::vector<uint8_t>& body)
    {
        if (std::string failure = CheckCommand(body, {0x80, 0x08}, true); !failure.empty())
        {
            failures.push_back(std::move(failure));
        }
    };
    for (uint32_t command = 0; command < 0x100; command++)
    {
        const auto byte = static_cast<uint8_t>(command);
        if (command < 0x80)
        {
            check({byte, 0x01, 0x40});
            check({byte, 0x21, 0x40, 0x00});
            check({byte, 0x08, 0x40, 0x64, 0x01});
            check({byte, 0x10, 0x40, 0x64, 0x00, 0x01});
        }
        else if (!left_out(command))
        {
            check({byte});
        }
    }
    check({0xf9, 0x80});
    for (uint32_t prefix = 0xb0; prefix < 0xb8; prefix++)
    {
        for (uint32_t command = 0xc0; command < 0x100; command++)
        {
            if (!left_out(command))
            {
                check({static_cast<uint8_t>(prefix), static_cast<uint8_t>(command), 0xff});
            }
        }
    }

    CheckNoFailures(failures);
}

// Checks one sequence against a model: every cursor between sub-frames is a command that the walk passed, and the
// cursors include `expected`.
void CheckModel(const nj::SequenceInstruments& walk, const std::set<uint32_t>& cursors,
                const std::set<uint32_t>& expected)
{
    XSFLIB_CHECK(walk.complete);
    std::vector<std::string> failures;
    for (const uint32_t cursor : cursors)
    {
        if (!Passed(walk, cursor))
        {
            failures.push_back("the model stops at " + std::to_string(cursor) + ", inside a command of the walk");
        }
    }
    for (const uint32_t cursor : expected)
    {
        if (!cursors.contains(cursor))
        {
            failures.push_back("the model doesn't stop at " + std::to_string(cursor));
        }
    }
    CheckNoFailures(failures);
}

// JAudio2's commands that the sweep leaves out, against the model. The root opens a track (0xc1), calls (0xc3), calls
// on condition 0 (0xc4), jumps on condition 0 (0xc8), jumps (0xc7) and ends (0xff). The subroutines return with 0xc5
// and with 0xc6 on condition 0. Each waits 8 ticks between them. The cursors after the waits: 0x07, 0x0d, 0x14 and 0x22
// in the root, 0x2a and 0x2e in the subroutines, 0x32 in the child.
void TestJaudio2Model()
{
    std::vector<uint8_t> sequence = {0xc1, 0x00, 0x00, 0x00, 0x30, 0xf1, 0x08, 0xc3, 0x00, 0x00, 0x28, 0xf1,
                                     0x08, 0xc4, 0x00, 0x00, 0x00, 0x2c, 0xf1, 0x08, 0xc8, 0x00, 0x00, 0x00,
                                     0x1a, 0xff, 0xc7, 0x00, 0x00, 0x20, 0xff, 0xff, 0xf1, 0x08, 0xff};
    sequence.resize(0x28, 0xff);
    sequence.insert(sequence.end(), {0xf1, 0x08, 0xc5, 0xff, 0xf1, 0x08, 0xc6, 0x00, 0xf1, 0x08, 0xff});

    CheckModel(nj::WalkSequence(sequence, 0), ModelCursors2(sequence), {0x07, 0x0d, 0x14, 0x22, 0x2a, 0x2e, 0x32});
}

// JAudio1's commands that the sweep leaves out, against the model. The root opens a track (0xc1), calls (0xc4), jumps
// (0xc8), loops twice (0xc9 and 0xca) and ends with ret at depth 0 (0xc6). The subroutine returns with ret. The child
// opens a brother (0xc2). Each waits 8 ticks between them. The cursors after the waits: 0x07, 0x0e and 0x19 in the
// root, 0x2a in the subroutine, 0x37 in the child and 0x3a in its brother.
void TestJaudio1Model()
{
    std::vector<uint8_t> sequence = {0xc1, 0x00, 0x00, 0x00, 0x30, 0x80, 0x08, 0xc4, 0x00, 0x00,
                                     0x00, 0x28, 0x80, 0x08, 0xc8, 0x00, 0x00, 0x00, 0x14, 0xff,
                                     0xc9, 0x00, 0x02, 0x80, 0x08, 0xca, 0xc6, 0x00};
    sequence.resize(0x28, 0xff);
    sequence.insert(sequence.end(), {0x80, 0x08, 0xc6, 0x00, 0xff, 0xff, 0xff, 0xff});
    sequence.insert(sequence.end(), {0xc2, 0x01, 0x00, 0x00, 0x38, 0x80, 0x08, 0xff, 0x80, 0x08, 0xff});

    CheckModel(j1::WalkSequence(sequence), ModelCursors1(sequence), {0x07, 0x0e, 0x19, 0x2a, 0x37, 0x3a});
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestCallsAndLoopStarts();
        TestReturns();
        TestLoopEnds();
        TestNotesAndCommands();
        TestWorkListAndLimit();
        TestChild();
        TestReads();
        TestJaudio2Notes();
        TestJaudio2Children();
        TestJaudio2Flow();
        TestJaudio2Loops();
        TestJaudio2BankAndProgram();
        TestJaudio2Registers();
        TestJaudio2OtherCommands();
        TestJaudio1Notes();
        TestJaudio1Registers();
        TestJaudio1Children();
        TestJaudio1Flow();
        TestJaudio1OtherCommands();
        TestJaudio2Model();
        TestJaudio1Model();
        TestJaudio2Sweep();
        TestJaudio1Sweep();
    };

    return RunTest("sequence_walk", tests);
}

// SPDX-License-Identifier: MIT

// The arguments of JAudio2's sequence commands, 0xA0 to 0xFF: Four Swords Adventures' command table at 0x80491e54. The
// entry for 0xE7 comes from Twilight Princess's table (0x803c5e80 in the game's main.dol). Commands below 0xA0 are
// notes, notes' ends and register prefixes, which the table leaves out.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ntdlib::jaudio
{

// A command's entry: whether the table has the command, the count of its arguments, and their types, two bits each,
// the first argument lowest. Type 0 is a byte, 1 is 16 bits, 2 is 24 bits, and 3 is a register's value (the
// register's number as a byte).
struct SequenceCommand
{
    bool known = false;
    uint8_t count = 0;
    uint16_t types = 0;
};

// The first command of the table.
constexpr uint8_t kFirstSequenceCommand = 0xa0;

namespace detail
{

constexpr std::array<SequenceCommand, 96> MakeSequenceCommands()
{
    std::array<SequenceCommand, 96> c{};
    const auto set = [&c](int command, uint8_t count, uint16_t types)
    {
        c[static_cast<std::size_t>(command - kFirstSequenceCommand)] = {true, count, types};
    };

    set(0xb1, 3, 0x0000); // note on: key, note ID, velocity
    set(0xb2, 1, 0x0000); // note off
    set(0xb3, 4, 0x0040); // note: flags, key, velocity, length
    set(0xb4, 1, 0x0000); // set the latest key
    set(0xb8, 2, 0x0000); // parameter, a byte value
    set(0xb9, 2, 0x0004); // parameter, a 16-bit value
    set(0xba, 3, 0x0010); // parameter, a byte value, a time
    set(0xbb, 3, 0x0014); // parameter, a 16-bit value, a time
    set(0xc1, 2, 0x0008); // open a track
    set(0xc2, 1, 0x0000); // close a track
    set(0xc3, 1, 0x0002); // call
    set(0xc4, 2, 0x0008); // call if
    set(0xc5, 0, 0x0000); // return
    set(0xc6, 1, 0x0000); // return if
    set(0xc7, 1, 0x0002); // jump
    set(0xc8, 2, 0x0008); // jump if
    set(0xc9, 2, 0x000b); // jump through a table
    set(0xca, 2, 0x000b); // call through a table
    set(0xcb, 1, 0x0001); // loop start
    set(0xcc, 0, 0x0000); // loop end
    set(0xd0, 2, 0x0000); // read a port
    set(0xd1, 2, 0x000c); // write a port
    set(0xd2, 1, 0x0000); // check a port's import
    set(0xd3, 1, 0x0000); // check a port's export
    set(0xd4, 2, 0x000c); // write the parent's port
    set(0xd5, 2, 0x000c); // write a child's port
    set(0xd6, 2, 0x0000); // read the parent's port
    set(0xd7, 2, 0x0000); // read a child's port
    set(0xd8, 2, 0x0004); // load a register
    set(0xd9, 3, 0x0030); // register arithmetic with a register
    set(0xda, 3, 0x0010); // register arithmetic with a value
    set(0xdb, 2, 0x0000); // negate a register
    set(0xdc, 4, 0x00e0); // load a register from a table
    set(0xe0, 1, 0x0001); // tempo
    set(0xe1, 1, 0x0001); // bank and program
    set(0xe2, 1, 0x0000); // bank
    set(0xe3, 1, 0x0000); // program
    set(0xe7, 2, 0x0004); // an oscillator's scale, in 16383ths
    set(0xe8, 2, 0x0008); // an oscillator's table
    set(0xe9, 5, 0x0155); // a simple ADSR
    set(0xea, 2, 0x0004); // bus connection
    set(0xeb, 1, 0x0000); // IIR cutoff
    set(0xec, 4, 0x0055); // IIR coefficients
    set(0xed, 1, 0x0002); // FIR coefficients
    set(0xf0, 0, 0x0000); // wait, a variable-length time
    set(0xf1, 1, 0x0000); // wait, a byte
    set(0xf3, 1, 0x0002); // interrupt table
    set(0xf4, 1, 0x0001); // enable interrupts
    set(0xf5, 1, 0x0001); // disable interrupts
    set(0xf6, 0, 0x0000); // return from an interrupt
    set(0xf7, 0, 0x0000); // clear the interrupt
    set(0xf8, 2, 0x0004); // interrupt timer
    set(0xf9, 1, 0x0001); // synchronise with the game
    set(0xfd, 0, 0x0000); // print
    set(0xfe, 0, 0x0000); // nothing
    set(0xff, 0, 0x0000); // finish

    return c;
}

} // namespace detail

// The table, indexed by the command less kFirstSequenceCommand.
inline constexpr std::array<SequenceCommand, 96> kSequenceCommands = detail::MakeSequenceCommands();

} // namespace ntdlib::jaudio

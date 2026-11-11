// SPDX-License-Identifier: MIT

#include "ntdlib/ubimilan/commands.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ubimilan/song.h"

namespace ntdlib::ubimilan
{
namespace
{

using commands::Command;
using commands::FromUnit;
using commands::Kind;
using commands::Scale;
using commands::ToUnit;

constexpr uint8_t kNoteOffStatus = 0x80;
constexpr uint8_t kNoteStatus = 0x90;
constexpr uint8_t kProgramStatus = 0xC0;
constexpr uint8_t kNothingStatus = 0xE0;
constexpr int kChannels = 16;

Command WaitItem(uint16_t countdown)
{
    Command item;
    item.kind = Kind::kWait;
    item.value.value = ToUnit(countdown, Scale{});
    return item;
}

// The whole of `data` as one item. The driver reads `read` bytes of it.
Decoded KeptAsData(std::span<const uint8_t> data, const std::string& reason, size_t read)
{
    Decoded decoded;
    Command item;
    item.kind = Kind::kData;
    item.bytes.assign(data.begin(), data.end());
    decoded.sequence.items.push_back(std::move(item));
    decoded.size = uint32_t(data.size());
    decoded.read = uint32_t(read);
    decoded.opaque = true;
    decoded.reason = reason;

    return decoded;
}

// The value of `item`'s field in the driver's units. Throws when the bytes can't hold it.
uint32_t DriverValue(commands::Fixed unit, uint32_t most, const char* what)
{
    const int64_t value = FromUnit(unit, Scale{});
    if (value < 0 || value > int64_t(most))
    {
        throw std::invalid_argument(std::string("the driver's bytes can't hold this ") + what);
    }

    return uint32_t(value);
}

} // namespace

Decoded DecodeSequence(std::span<const uint8_t> data)
{
    if (data.size() < kSequenceHeader + 2 || data[0] != kSequence)
    {
        return KeptAsData(data, "the bytes don't start as a sequence", data.size());
    }

    Decoded decoded;
    Command header;
    header.kind = Kind::kData;
    header.bytes.assign(data.begin(), data.begin() + kSequenceHeader);
    decoded.sequence.items.push_back(std::move(header));
    size_t at = kSequenceHeader;
    const auto take_wait = [&data, &at, &decoded]()
    {
        if (at + 2 > data.size())
        {
            return false;
        }

        decoded.sequence.items.push_back(WaitItem(xsflib::LoadLe16(&data[at])));
        at += 2;
        return true;
    };

    if (!take_wait())
    {
        return KeptAsData(data, "a countdown runs past the bytes", data.size());
    }

    while (true)
    {
        if (at >= data.size())
        {
            return KeptAsData(data, "the bytes hold no end", data.size());
        }

        const uint8_t status = data[at];
        if (status == kEndCommand)
        {
            Command end;
            end.kind = Kind::kEnd;
            decoded.sequence.items.push_back(std::move(end));
            decoded.size = uint32_t(at + 1);
            decoded.read = decoded.size;
            return decoded;
        }

        Command item;
        item.index = uint8_t(status & 0xF);
        const size_t arguments = size_t(ArgumentBytes(status));
        if (at + 1 + arguments > data.size())
        {
            return KeptAsData(data, "a command runs past the bytes", data.size());
        }

        switch (status & 0xF0)
        {
        case kNoteStatus:
        case kNoteOffStatus:
            item.kind = (status & 0xF0) == kNoteStatus ? Kind::kNote : Kind::kNoteOff;
            item.key = data[at + 1];
            item.velocity = ToUnit(data[at + 2], Scale{});
            break;

        case kProgramStatus:
            item.kind = Kind::kProgram;
            item.value.value = ToUnit(data[at + 1], Scale{});
            break;

        case kNothingStatus:
            item.kind = Kind::kOther;
            item.encoding.opcode = status;
            break;

        default:
            char text[64];
            std::snprintf(text, sizeof text, "command %02X, on which the game resets", unsigned(status));
            return KeptAsData(data, text, at + 1);
        }

        decoded.sequence.items.push_back(std::move(item));
        at += 1 + arguments;
        if (!take_wait())
        {
            return KeptAsData(data, "a countdown runs past the bytes", data.size());
        }
    }
}

Decoded DecodeResource(const Rom& rom, const DriverInfo& info, int id)
{
    const uint32_t address = ResourceAddress(rom, info, id);
    if (!address)
    {
        throw std::invalid_argument("the resource names no byte of the ROM");
    }

    if (rom.U8(address) != kSequence)
    {
        throw std::invalid_argument("the resource isn't a sequence");
    }

    return DecodeSequence(std::span<const uint8_t>(rom.Ptr(address), size_t(rom.End() - address)));
}

Encoded EncodeSequence(const commands::Sequence& sequence)
{
    const std::vector<Command>& items = sequence.items;
    if (items.size() < 2 || items[0].kind != Kind::kData || items[0].bytes.size() != kSequenceHeader ||
        items[0].bytes[0] != kSequence || items[1].kind != Kind::kWait)
    {
        throw std::invalid_argument("a sequence starts with its 4 bytes and a wait");
    }

    Encoded encoded;
    encoded.bytes = items[0].bytes;
    encoded.offsets.push_back(0);
    bool wait_due = true;
    bool ended = false;
    for (size_t i = 1; i < items.size(); i++)
    {
        const Command& item = items[i];
        encoded.offsets.push_back(uint32_t(encoded.bytes.size()));
        if (ended)
        {
            if (item.kind != Kind::kData)
            {
                throw std::invalid_argument("a command lies after the sequence's end");
            }

            encoded.bytes.insert(encoded.bytes.end(), item.bytes.begin(), item.bytes.end());
            continue;
        }

        if (wait_due != (item.kind == Kind::kWait))
        {
            throw std::invalid_argument(wait_due ? "a command takes a wait after it"
                                                 : "a wait takes a command before it");
        }

        if (item.index >= kChannels)
        {
            throw std::invalid_argument("a sequence has 16 channels");
        }

        switch (item.kind)
        {
        case Kind::kWait:
            {
                const uint32_t countdown = DriverValue(item.value.value, 0xFFFF, "countdown");
                encoded.bytes.push_back(uint8_t(countdown));
                encoded.bytes.push_back(uint8_t(countdown >> 8));
                break;
            }

        case Kind::kNote:
        case Kind::kNoteOff:
            encoded.bytes.push_back(uint8_t((item.kind == Kind::kNote ? kNoteStatus : kNoteOffStatus) | item.index));
            encoded.bytes.push_back(item.key);
            encoded.bytes.push_back(uint8_t(DriverValue(item.velocity, 0xFF, "velocity")));
            break;

        case Kind::kProgram:
            encoded.bytes.push_back(uint8_t(kProgramStatus | item.index));
            encoded.bytes.push_back(uint8_t(DriverValue(item.value.value, 0xFF, "program")));
            break;

        case Kind::kOther:
            if ((item.encoding.opcode & 0xFFF0) != kNothingStatus)
            {
                throw std::invalid_argument("the driver has no command of this opcode");
            }

            encoded.bytes.push_back(uint8_t(item.encoding.opcode));
            break;

        case Kind::kEnd:
            encoded.bytes.push_back(uint8_t(kEndCommand));
            ended = true;
            break;

        default:
            throw std::invalid_argument("the driver has no command of this kind");
        }

        wait_due = item.kind != Kind::kWait && !ended;
    }

    if (!ended)
    {
        throw std::invalid_argument("a sequence takes an end");
    }

    encoded.offsets.push_back(uint32_t(encoded.bytes.size()));

    return encoded;
}

commands::Scale ScaleOf(commands::Kind)
{
    return {};
}

} // namespace ntdlib::ubimilan

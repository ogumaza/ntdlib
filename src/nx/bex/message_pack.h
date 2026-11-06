// SPDX-License-Identifier: MIT

// A reader of MessagePack, the form of Bezel Engine's settings files, such as the jump settings of the music sync and
// the tables of the user procedures.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::nx::bex::detail
{

// A value of MessagePack, as far as the engine's files use the format: nil, booleans, integers, strings, arrays and
// maps.
struct Value
{
    std::variant<std::monostate, bool, int64_t, std::string, std::vector<Value>, std::vector<std::pair<Value, Value>>>
        v;

    const std::string* String() const
    {
        return std::get_if<std::string>(&v);
    }

    const std::vector<std::pair<Value, Value>>* Map() const
    {
        return std::get_if<std::vector<std::pair<Value, Value>>>(&v);
    }

    const std::vector<Value>* Array() const
    {
        return std::get_if<std::vector<Value>>(&v);
    }

    // The value of key `key` in a map, or null.
    const Value* Get(const std::string& key) const
    {
        if (const auto* map = Map())
        {
            for (const auto& [k, value] : *map)
            {
                if (const std::string* s = k.String(); s && *s == key)
                {
                    return &value;
                }
            }
        }

        return nullptr;
    }
};

// Reads a MessagePack value from a file. `what` names the file in the messages, such as "the jump setting". Throws
// xsflib::FormatError for a file that ends early, nests more than 16 deep or holds another type.
class MessagePack
{
public:
    MessagePack(std::span<const uint8_t> data, std::string what) : data_(data), what_(std::move(what))
    {
    }

    Value Read(int depth = 0)
    {
        if (depth > 16)
        {
            throw xsflib::FormatError(what_ + " nests too deep");
        }

        const uint8_t c = Byte();
        if (c <= 0x7F)
        {
            return {static_cast<int64_t>(c)};
        }
        if (c >= 0xE0)
        {
            return {static_cast<int64_t>(static_cast<int8_t>(c))};
        }
        if ((c & 0xF0) == 0x80)
        {
            return ReadMap(c & 0x0F, depth);
        }
        if ((c & 0xF0) == 0x90)
        {
            return ReadArray(c & 0x0F, depth);
        }
        if ((c & 0xE0) == 0xA0)
        {
            return ReadString(c & 0x1F);
        }

        switch (c)
        {
        case 0xC0:
            return {};
        case 0xC2:
            return {false};
        case 0xC3:
            return {true};
        case 0xCC:
            return {static_cast<int64_t>(Byte())};
        case 0xCD:
            return {static_cast<int64_t>(Big(2))};
        case 0xCE:
            return {static_cast<int64_t>(Big(4))};
        case 0xD0:
            return {static_cast<int64_t>(static_cast<int8_t>(Byte()))};
        case 0xD1:
            return {static_cast<int64_t>(static_cast<int16_t>(Big(2)))};
        case 0xD2:
            return {static_cast<int64_t>(static_cast<int32_t>(Big(4)))};
        case 0xD9:
            return ReadString(Byte());
        case 0xDA:
            return ReadString(Big(2));
        case 0xDC:
            return ReadArray(Big(2), depth);
        case 0xDE:
            return ReadMap(Big(2), depth);
        default:
            throw xsflib::FormatError(what_ + " holds a MessagePack type it doesn't use");
        }
    }

private:
    uint8_t Byte()
    {
        if (at_ >= data_.size())
        {
            throw xsflib::FormatError(what_ + " ends early");
        }

        return data_[at_++];
    }

    uint32_t Big(int bytes)
    {
        uint32_t value = 0;
        for (int i = 0; i < bytes; ++i)
        {
            value = value << 8 | Byte();
        }

        return value;
    }

    Value ReadString(uint32_t size)
    {
        if (size > data_.size() - at_)
        {
            throw xsflib::FormatError(what_ + " ends inside a string");
        }

        std::string s(reinterpret_cast<const char*>(data_.data() + at_), size);
        at_ += size;

        return {std::move(s)};
    }

    Value ReadArray(uint32_t count, int depth)
    {
        std::vector<Value> items;
        for (uint32_t i = 0; i < count; ++i)
        {
            items.push_back(Read(depth + 1));
        }

        return {std::move(items)};
    }

    Value ReadMap(uint32_t count, int depth)
    {
        std::vector<std::pair<Value, Value>> items;
        for (uint32_t i = 0; i < count; ++i)
        {
            Value key = Read(depth + 1);
            Value value = Read(depth + 1);
            items.emplace_back(std::move(key), std::move(value));
        }

        return {std::move(items)};
    }

    std::span<const uint8_t> data_;
    std::string what_;
    std::size_t at_ = 0;
};

} // namespace ntdlib::nx::bex::detail

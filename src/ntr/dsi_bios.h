// SPDX-License-Identifier: MIT

// The DSi BIOS's SHA-1 calls, SWI 0x24 to 0x29. Both CPUs have them in DSi mode (GBATEK's "BIOS SHA1 Functions (DSi
// only)"). A call's context is the BIOS's structure of 0x64 bytes in memory: the 5 state words at 0x00, the length in
// bits (64 bits) at 0x14, the buffer of an incomplete block at 0x1C, the buffer's fill at 0x5C and the block callback
// at 0x60. Programs copy contexts. Each call therefore reads the structure from memory and writes it back. The model
// runs SHA-1 itself in place of the callback. Init stores kDefaultCallback where the field is 0. A call on a context
// with another callback fails: the BIOS would call it. `load(address, size)` and `store(address, size, value)` reach
// memory as the CPU does.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/text.h"

#include "ntdlib/arm/cpu.h"

namespace ntdlib::ntr::bios
{

namespace sha1
{

inline constexpr uint32_t kLength = 0x14;
inline constexpr uint32_t kBuffer = 0x1C;
inline constexpr uint32_t kFill = 0x5C;
inline constexpr uint32_t kCallback = 0x60;

// What Init stores as the callback when the field is 0. The BIOS stores the address of its default callback. No game
// calls that callback.
inline constexpr uint32_t kDefaultCallback = 0xFFFF0001;

// The context, in host memory.
struct Context
{
    std::array<uint32_t, 5> state = {};
    uint64_t bits = 0;
    std::array<uint8_t, 64> buffer = {};
    uint32_t fill = 0;
    uint32_t callback = 0;
};

inline uint32_t Rotate(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

inline void Init(Context& context)
{
    context.state = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    context.bits = 0;
    context.fill = 0;
    if (context.callback == 0)
    {
        context.callback = kDefaultCallback;
    }
}

// The default callback on one block of 64 bytes: GBATEK's pseudo code, the same as FIPS 180's compression function.
inline void Block(Context& context, const uint8_t* block)
{
    std::array<uint32_t, 80> w = {};
    for (std::size_t i = 0; i < 16; i++)
    {
        w[i] = uint32_t{block[4 * i]} << 24 | uint32_t{block[4 * i + 1]} << 16 | uint32_t{block[4 * i + 2]} << 8 |
               block[4 * i + 3];
    }
    for (std::size_t i = 16; i < 80; i++)
    {
        w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    auto [a, b, c, d, e] = context.state;
    for (std::size_t i = 0; i < 80; i++)
    {
        uint32_t f = 0;
        if (i < 20)
        {
            f = 0x5A827999 + (d ^ (b & (c ^ d)));
        }
        else if (i < 40)
        {
            f = 0x6ED9EBA1 + (b ^ c ^ d);
        }
        else if (i < 60)
        {
            f = 0x8F1BBCDC + ((b & c) | (d & (b | c)));
        }
        else
        {
            f = 0xCA62C1D6 + (b ^ c ^ d);
        }

        const uint32_t next = f + e + Rotate(a, 5) + w[i];
        e = d;
        d = c;
        c = Rotate(b, 30);
        b = a;
        a = next;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
}

inline void Update(Context& context, std::span<const uint8_t> data)
{
    context.bits += uint64_t{data.size()} * 8;
    for (const uint8_t byte : data)
    {
        context.buffer[context.fill++] = byte;
        if (context.fill == 64)
        {
            Block(context, context.buffer.data());
            context.fill = 0;
        }
    }
}

// Pads the message, gives the digest, and leaves the length in bits as it was before the padding.
inline std::array<uint8_t, 20> Finish(Context& context)
{
    const uint64_t bits = context.bits;
    const std::array<uint8_t, 1> kEnd = {0x80};
    Update(context, kEnd);
    const std::array<uint8_t, 1> kZero = {0};
    while (context.fill != 56)
    {
        Update(context, kZero);
    }

    std::array<uint8_t, 8> length = {};
    for (std::size_t i = 0; i < 8; i++)
    {
        length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    Update(context, length);
    context.bits = bits;

    std::array<uint8_t, 20> digest = {};
    for (std::size_t i = 0; i < 20; i++)
    {
        digest[i] = static_cast<uint8_t>(context.state[i / 4] >> (24 - 8 * (i % 4)));
    }

    return digest;
}

template <typename Load>
Context LoadContext(uint32_t address, const Load& load)
{
    Context context;
    for (std::size_t i = 0; i < 5; i++)
    {
        context.state[i] = load(address + static_cast<uint32_t>(4 * i), 4);
    }
    context.bits = uint64_t{load(address + kLength + 4, 4)} << 32 | load(address + kLength, 4);
    for (std::size_t i = 0; i < 64; i++)
    {
        context.buffer[i] = static_cast<uint8_t>(load(address + kBuffer + static_cast<uint32_t>(i), 1));
    }
    context.fill = load(address + kFill, 4) & 0x3F;
    context.callback = load(address + kCallback, 4);

    return context;
}

template <typename Store>
void StoreContext(uint32_t address, const Context& context, const Store& store)
{
    for (std::size_t i = 0; i < 5; i++)
    {
        store(address + static_cast<uint32_t>(4 * i), 4, context.state[i]);
    }
    store(address + kLength, 4, static_cast<uint32_t>(context.bits));
    store(address + kLength + 4, 4, static_cast<uint32_t>(context.bits >> 32));
    for (std::size_t i = 0; i < 64; i++)
    {
        store(address + kBuffer + static_cast<uint32_t>(i), 1, context.buffer[i]);
    }
    store(address + kFill, 4, context.fill);
    store(address + kCallback, 4, context.callback);
}

template <typename Load>
std::vector<uint8_t> LoadBytes(uint32_t address, uint32_t size, const Load& load)
{
    std::vector<uint8_t> bytes(size);
    for (uint32_t i = 0; i < size; i++)
    {
        bytes[i] = static_cast<uint8_t>(load(address + i, 1));
    }

    return bytes;
}

template <typename Store>
void StoreBytes(uint32_t address, std::span<const uint8_t> bytes, const Store& store)
{
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        store(address + static_cast<uint32_t>(i), 1, bytes[i]);
    }
}

} // namespace sha1

// The first and last SWIs of the SHA-1 calls.
inline constexpr uint32_t kSha1First = 0x24;
inline constexpr uint32_t kSha1Last = 0x29;

// Runs SHA-1 call `number`, from kSha1First to kSha1Last, with the arguments in the CPU's registers. Returns a message
// for a call that the model can't make, and an empty string otherwise.
template <typename Load, typename Store>
std::string Sha1Call(arm::Cpu& cpu, uint32_t number, const Load& load, const Store& store)
{
    const uint32_t r0 = cpu.Reg(0);
    const uint32_t r1 = cpu.Reg(1);
    const uint32_t r2 = cpu.Reg(2);
    const auto foreign_callback = [](const sha1::Context& context, uint32_t address) -> std::string
    {
        if (context.callback == sha1::kDefaultCallback)
        {
            return {};
        }

        return "a SHA-1 context at " + xsflib::Hex(address, 8, true) + " with the callback " +
               xsflib::Hex(context.callback, 8, true) + ", which the BIOS doesn't model";
    };

    switch (number)
    {
    case 0x24: // Init(context)
        {
            sha1::Context context = sha1::LoadContext(r0, load);
            sha1::Init(context);
            sha1::StoreContext(r0, context, store);
            return {};
        }

    case 0x25: // Update(context, source, size)
        {
            sha1::Context context = sha1::LoadContext(r0, load);
            if (std::string error = foreign_callback(context, r0); !error.empty())
            {
                return error;
            }

            sha1::Update(context, sha1::LoadBytes(r1, r2, load));
            sha1::StoreContext(r0, context, store);
            return {};
        }

    case 0x26: // Finish(digest, context)
        {
            sha1::Context context = sha1::LoadContext(r1, load);
            if (std::string error = foreign_callback(context, r1); !error.empty())
            {
                return error;
            }

            const std::array<uint8_t, 20> digest = sha1::Finish(context);
            sha1::StoreContext(r1, context, store);
            sha1::StoreBytes(r0, digest, store);
            return {};
        }

    case 0x27: // InitUpdateFinish(digest, source, size)
        {
            sha1::Context context;
            sha1::Init(context);
            sha1::Update(context, sha1::LoadBytes(r1, r2, load));
            sha1::StoreBytes(r0, sha1::Finish(context), store);
            cpu.SetReg(0, 1);
            return {};
        }

    case 0x28: // Compare20Bytes(first, second): 1 when they match, 0 otherwise or for a null address
        cpu.SetReg(0, r0 != 0 && r1 != 0 && sha1::LoadBytes(r0, 20, load) == sha1::LoadBytes(r1, 20, load) ? 1 : 0);
        return {};

    default: // 0x29, InitUpdateFinishMess(destination, destination's size, source, source's size)
        {
            const uint32_t r3 = cpu.Reg(3);
            if (r0 == 0 || (r2 == 0 && r3 != 0))
            {
                cpu.SetReg(0, r0 == 0 ? 1 : 0);
                return {};
            }

            sha1::Context context;
            sha1::Init(context);
            sha1::Update(context, sha1::LoadBytes(r2, r3, load));
            std::array<uint8_t, 20> first = sha1::Finish(context);
            uint32_t destination = r0;
            uint32_t left = r1;
            do
            {
                // GBATEK's increment of the big-endian digest. It carries into a byte only while the byte below ends
                // at 1.
                int i = 19;
                do
                {
                    first[static_cast<std::size_t>(i)]++;
                    i--;
                } while (i >= 0 && first[static_cast<std::size_t>(i + 1)] == 1);

                // The second digest goes on from the first's context, with no Init.
                sha1::Update(context, first);
                const std::array<uint8_t, 20> second = sha1::Finish(context);
                const uint32_t count = std::min<uint32_t>(20, left);
                sha1::StoreBytes(destination, std::span<const uint8_t>(second.data(), count), store);
                destination += count;
                left -= count;
            } while (left != 0);

            cpu.SetReg(0, 1);
            return {};
        }
    }
}

} // namespace ntdlib::ntr::bios

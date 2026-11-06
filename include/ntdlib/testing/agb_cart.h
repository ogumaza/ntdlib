// SPDX-License-Identifier: MIT

// Helpers for the GBA's tests: small synthetic cartridges built in memory, so no game ROM is needed, and a temporary
// folder for the files that a test writes.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"

namespace ntdlib::testing::agb_files
{
using ntdlib::agb::kRomBase;
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::ReadFile;
using ntdlib::agb::RegisterWrite;
using ntdlib::agb::Rom;
using ntdlib::agb::Utf8;

// Output folder for this test run.
inline std::filesystem::path g_temp;

// Returns the path of `name` in this run's temp folder.
inline std::filesystem::path TempPath(const std::string& name);

// Opens `name` in this run's temp folder for writing, in binary mode, or returns null if it can't be opened.
inline std::FILE* OpenTempFile(const std::string& name);

// Returns a file's bytes, or none if it can't be read.
inline std::vector<uint8_t> ReadAll(const std::string& path);

// Returns the number at `p`, in little-endian byte order (xsflib's bytes.h).
inline uint16_t Le16(const uint8_t* p)
{
    return xsflib::LoadLe16(p);
}

inline uint32_t Le32(const uint8_t* p)
{
    return xsflib::LoadLe32(p);
}

// Returns the value of the last write to `address` in `writes`, or -1 if there was none.
inline long long LastWrite(const std::vector<RegisterWrite>& writes, uint32_t address);

// A cartridge image that a test builds in memory. Its first byte is at kRomBase, where the GBA maps the ROM. Each
// driver's tests build their cartridges on it.
class CartImage
{
public:
    // Starts an image of `size` bytes, all 0.
    explicit CartImage(size_t size);

    // Put8(), Put16() and Put32() write `v` at address `a`, in little-endian byte order.
    void Put8(uint32_t a, uint8_t v);
    void Put16(uint32_t a, uint16_t v);
    void Put32(uint32_t a, uint32_t v);

    // Writes `bytes` from address `a` on.
    void PutBytes(uint32_t a, const std::vector<uint8_t>& bytes);

    // Writes `halfwords`, such as Thumb code, from address `a` on.
    void PutHalfwords(uint32_t a, const std::vector<uint16_t>& halfwords);

    // Writes the Thumb bl at address `a` that calls `target`.
    void PutBl(uint32_t a, uint32_t target);

    // Grows or shrinks the image to `size` bytes. New bytes are 0.
    void Resize(size_t size);

    // Get8() and Get16() return the byte or the halfword at address `a`.
    uint8_t Get8(uint32_t a) const;
    uint16_t Get16(uint32_t a) const;

    // Returns the image as a ROM.
    Rom ToRom() const;

private:
    std::vector<uint8_t> bytes_;
};

// Creates a unique directory in the system's temp folder so concurrent test runs don't share files. Returns false on
// failure.
inline bool MakeTempFolder()
{
    std::random_device random;
    for (int attempt = 0; attempt < 100; attempt++)
    {
        char name[48];
        std::snprintf(name, sizeof name, "ntdlib_agb_tests_%08x%08x", unsigned(random()),
                      unsigned(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        g_temp = std::filesystem::temp_directory_path(ec) / name;
        if (!ec && std::filesystem::create_directory(g_temp, ec))
        {
            return true;
        }
    }

    return false;
}

inline std::filesystem::path TempPath(const std::string& name)
{
    return g_temp / PathFromUtf8(name);
}

inline std::FILE* OpenTempFile(const std::string& name)
{
    // MSVC warns that std::fopen is unsafe, so it gets fopen_s, which does the same here.
    const std::string path = TempPath(name).string();
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "wb") != 0)
    {
        return nullptr;
    }
#else
    f = std::fopen(path.c_str(), "wb");
#endif

    return f;
}

inline CartImage::CartImage(size_t size) : bytes_(size, 0)
{
}

inline void CartImage::Put8(uint32_t a, uint8_t v)
{
    bytes_[a - kRomBase] = v;
}

inline void CartImage::Put16(uint32_t a, uint16_t v)
{
    Put8(a, uint8_t(v));
    Put8(a + 1, uint8_t(v >> 8));
}

inline void CartImage::Put32(uint32_t a, uint32_t v)
{
    Put16(a, uint16_t(v));
    Put16(a + 2, uint16_t(v >> 16));
}

inline void CartImage::PutBytes(uint32_t a, const std::vector<uint8_t>& bytes)
{
    std::copy(bytes.begin(), bytes.end(), bytes_.begin() + (a - kRomBase));
}

inline void CartImage::PutHalfwords(uint32_t a, const std::vector<uint16_t>& halfwords)
{
    for (size_t i = 0; i < halfwords.size(); i++)
    {
        Put16(a + 2 * uint32_t(i), halfwords[i]);
    }
}

inline void CartImage::PutBl(uint32_t a, uint32_t target)
{
    const uint32_t offset = target - (a + 4);
    Put16(a, uint16_t(0xF000 | ((offset >> 12) & 0x7FF)));
    Put16(a + 2, uint16_t(0xF800 | ((offset >> 1) & 0x7FF)));
}

inline void CartImage::Resize(size_t size)
{
    bytes_.resize(size, 0);
}

inline uint8_t CartImage::Get8(uint32_t a) const
{
    return bytes_[a - kRomBase];
}

inline uint16_t CartImage::Get16(uint32_t a) const
{
    return Le16(&bytes_[a - kRomBase]);
}

inline Rom CartImage::ToRom() const
{
    Rom r;
    r.Assign(bytes_);

    return r;
}

inline std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::vector<uint8_t> data;
    ReadFile(path, data);

    return data;
}

inline long long LastWrite(const std::vector<RegisterWrite>& writes, uint32_t address)
{
    long long value = -1;
    for (const RegisterWrite& w : writes)
    {
        if (w.address == address)
        {
            value = w.value;
        }
    }

    return value;
}

} // namespace ntdlib::testing::agb_files

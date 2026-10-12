// SPDX-License-Identifier: MIT

#include "ntdlib/disc/compression.h"

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/source.h"
#include "xsflib/bytes.h"

namespace ntdlib::disc
{
namespace
{

// Output grows in steps of this size up to the caller's limit.
constexpr size_t kGrowth = 1 << 20;

// Makes room for more output. Returns false when `out` is at the limit.
bool Grow(std::vector<uint8_t>& out, size_t used, size_t max_size)
{
    if (used >= max_size)
    {
        return false;
    }

    out.resize(std::min(max_size, used + kGrowth));

    return true;
}

std::vector<uint8_t> Bunzip(std::span<const uint8_t> in, size_t max_size)
{
    bz_stream stream = {};
    if (BZ2_bzDecompressInit(&stream, 0, 0) != BZ_OK)
    {
        throw Error("bzip2 couldn't start");
    }

    // bzip2 counts sizes in unsigned int. Feed the input in pieces that fit.
    std::vector<uint8_t> out;
    size_t used = 0;
    size_t consumed = 0;
    int result = BZ_OK;
    while (result != BZ_STREAM_END && Grow(out, used, max_size))
    {
        const size_t in_piece = std::min<size_t>(in.size() - consumed, std::numeric_limits<unsigned>::max());
        const size_t out_piece = std::min<size_t>(out.size() - used, std::numeric_limits<unsigned>::max());
        stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(in.data() + consumed));
        stream.avail_in = static_cast<unsigned>(in_piece);
        stream.next_out = reinterpret_cast<char*>(out.data() + used);
        stream.avail_out = static_cast<unsigned>(out_piece);
        result = BZ2_bzDecompress(&stream);
        consumed += in_piece - stream.avail_in;
        used += out_piece - stream.avail_out;
        if (result != BZ_OK && result != BZ_STREAM_END)
        {
            BZ2_bzDecompressEnd(&stream);
            throw Error("bzip2 data is damaged");
        }

        // Without the end of the stream, stop when the input is used up and no more output comes.
        if (result == BZ_OK && consumed == in.size() && out_piece - stream.avail_out == 0)
        {
            break;
        }
    }

    BZ2_bzDecompressEnd(&stream);
    out.resize(used);

    return out;
}

std::vector<uint8_t> Unlzma(Method method, std::span<const uint8_t> props, std::span<const uint8_t> in, size_t max_size)
{
    // The properties of the 7-Zip SDK. For LZMA they are a byte (pb * 5 + lp) * 9 + lc and a little-endian dictionary
    // size. For LZMA2 they are a byte that encodes the dictionary size.
    lzma_options_lzma options;
    lzma_lzma_preset(&options, 0);
    if (method == Method::kLzma)
    {
        if (props.size() < 5 || props[0] >= 9 * 5 * 5)
        {
            throw Error("the LZMA properties are invalid");
        }

        options.lc = props[0] % 9;
        options.lp = (props[0] / 9) % 5;
        options.pb = props[0] / 45;
        options.dict_size = xsflib::LoadLe32(props.data() + 1);
    }
    else
    {
        if (props.empty() || props[0] > 40)
        {
            throw Error("the LZMA2 properties are invalid");
        }

        options.dict_size = props[0] == 40 ? 0xFFFFFFFF : (2u | (props[0] & 1u)) << (props[0] / 2 + 11);
    }

    const lzma_filter filters[] = {
        {method == Method::kLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &options},
        {LZMA_VLI_UNKNOWN, nullptr},
    };
    lzma_stream stream = LZMA_STREAM_INIT;
    if (lzma_raw_decoder(&stream, filters) != LZMA_OK)
    {
        throw Error("the LZMA properties aren't supported");
    }

    // An LZMA stream in WIA has no end marker. It ends with the input.
    std::vector<uint8_t> out;
    size_t used = 0;
    stream.next_in = in.data();
    stream.avail_in = in.size();
    lzma_ret result = LZMA_OK;
    while (result != LZMA_STREAM_END && Grow(out, used, max_size))
    {
        stream.next_out = out.data() + used;
        stream.avail_out = out.size() - used;
        result = lzma_code(&stream, LZMA_RUN);
        const size_t produced = out.size() - used - stream.avail_out;
        used += produced;
        if (result == LZMA_BUF_ERROR || (result == LZMA_OK && stream.avail_in == 0 && produced == 0))
        {
            break;
        }
        if (result != LZMA_OK && result != LZMA_STREAM_END)
        {
            lzma_end(&stream);
            throw Error("LZMA data is damaged");
        }
    }

    lzma_end(&stream);
    out.resize(used);

    return out;
}

std::vector<uint8_t> Unzstd(std::span<const uint8_t> in, size_t max_size)
{
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (!context)
    {
        throw Error("Zstandard couldn't start");
    }

    std::vector<uint8_t> out;
    ZSTD_inBuffer input = {in.data(), in.size(), 0};
    size_t used = 0;
    size_t result = 1;
    while (result != 0 && Grow(out, used, max_size))
    {
        ZSTD_outBuffer output = {out.data(), out.size(), used};
        result = ZSTD_decompressStream(context, &output, &input);
        const size_t produced = output.pos - used;
        used = output.pos;
        if (ZSTD_isError(result))
        {
            ZSTD_freeDCtx(context);
            throw Error("Zstandard data is damaged");
        }
        if (input.pos == input.size && produced == 0)
        {
            break;
        }
    }

    ZSTD_freeDCtx(context);
    out.resize(used);

    return out;
}

} // namespace

size_t Inflate(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    // GCZ blocks are a few megabytes at most. That's well within zlib's 32-bit sizes.
    if (in.size() > std::numeric_limits<uInt>::max() || out.size() > std::numeric_limits<uInt>::max())
    {
        throw Error("a compressed block is too large");
    }

    z_stream stream = {};
    if (inflateInit(&stream) != Z_OK)
    {
        throw Error("zlib couldn't start");
    }

    stream.next_in = const_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&stream, Z_FINISH);
    const size_t written = out.size() - stream.avail_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END && !(result == Z_BUF_ERROR && stream.avail_out == 0))
    {
        throw Error("zlib data is damaged");
    }

    return written;
}

uint32_t Adler32(std::span<const uint8_t> data)
{
    uLong sum = adler32(0, nullptr, 0);
    while (!data.empty())
    {
        const size_t n = std::min<size_t>(data.size(), std::numeric_limits<uInt>::max());
        sum = adler32(sum, data.data(), static_cast<uInt>(n));
        data = data.subspan(n);
    }

    return static_cast<uint32_t>(sum);
}

std::vector<uint8_t> Decompress(Method method, std::span<const uint8_t> props, std::span<const uint8_t> in,
                                size_t max_size)
{
    switch (method)
    {
    case Method::kBzip2:
        return Bunzip(in, max_size);
    case Method::kLzma:
    case Method::kLzma2:
        return Unlzma(method, props, in, max_size);
    case Method::kZstd:
        return Unzstd(in, max_size);
    default:
        throw Error("compression method " + std::to_string(static_cast<uint32_t>(method)) + " isn't supported");
    }
}

} // namespace ntdlib::disc

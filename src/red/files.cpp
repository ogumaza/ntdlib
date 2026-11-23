// SPDX-License-Identifier: MIT

// The files of Square's Red driver on a GameCube disc (see files.h).

#include "ntdlib/red/files.h"

#include <algorithm>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"

#include "ntdlib/zero_padded.h"

namespace ntdlib::red
{
namespace
{

// The magic of a piece, and the place of its bank's number.
constexpr std::string_view kPieceMagic = "BGM ";
constexpr std::size_t kPieceBankOffset = 6;

} // namespace

std::map<uint32_t, std::string> Files(const disc::GameFiles& game, std::string_view folder, const FileName& name)
{
    constexpr std::size_t kMaxDigits = 8;
    std::map<uint32_t, std::string> found;
    for (const disc::GameFile& file : game.files)
    {
        const std::string path = xsflib::AsciiLower(file.path);
        if (!path.starts_with(folder) || !path.ends_with(name.extension))
        {
            continue;
        }

        const std::string_view rest = std::string_view(path).substr(folder.size());
        if (!rest.starts_with(name.prefix) || rest.size() < name.prefix.size() + name.extension.size())
        {
            continue;
        }

        const std::string_view digits =
            rest.substr(name.prefix.size(), rest.size() - name.prefix.size() - name.extension.size());
        const auto is_digit = [](char c)
        {
            return c >= '0' && c <= '9';
        };
        if (digits.empty() || digits.size() > kMaxDigits || !std::all_of(digits.begin(), digits.end(), is_digit))
        {
            continue;
        }

        const uint32_t number = static_cast<uint32_t>(std::stoul(std::string(digits)));
        if (ZeroPadded(number, name.digits) == digits)
        {
            found.emplace(number, file.path);
        }
    }

    return found;
}

std::string Label(const std::string& path, const FileName& name)
{
    std::string label = path.substr(path.rfind('/') + 1);
    label.resize(label.size() - name.extension.size());

    return label;
}

std::optional<uint32_t> PieceBank(std::span<const uint8_t> piece)
{
    if (piece.size() < kPieceBankOffset + 2 || !std::equal(kPieceMagic.begin(), kPieceMagic.end(), piece.begin()))
    {
        return std::nullopt;
    }

    return xsflib::LoadBe16(piece.data() + kPieceBankOffset);
}

} // namespace ntdlib::red

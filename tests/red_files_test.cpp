// SPDX-License-Identifier: MIT

// Tests of the files of Square's Red driver (ntdlib/red/files.h) on made-up lists of a disc's files. The forms of the
// names come from the format strings in Final Fantasy Crystal Chronicles' main.dol: "dvd/sound/music/music%03d.bgm",
// "dvd/sound/wave/wave%04d.wd", "dvd/sound/se/block/se%03d.seb" and "dvd/sound/se/sep/se%06d.sep". The disc names the
// folder of the pieces "Music".

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/disc/game.h"
#include "ntdlib/red/files.h"

namespace red = ntdlib::red;
using xsflib::testing::Check;

namespace
{

// A game with the files at `paths`, each of 16 bytes.
ntdlib::disc::GameFiles GameWith(const std::vector<std::string>& paths)
{
    ntdlib::disc::GameFiles game;
    for (const std::string& path : paths)
    {
        game.files.push_back({path, 16});
    }

    return game;
}

// The files of a form match in any case and keep the disc's spelling. A number takes the digits that sprintf writes:
// at least the form's count, with no further leading zero.
void TestFilesByNumber()
{
    const ntdlib::disc::GameFiles game = GameWith({
        "dvd/sound/Music/music101.bgm",
        "dvd/sound/music/MUSIC007.BGM",
        "dvd/sound/music/music1234.bgm",
        "dvd/sound/music/music0012.bgm",
        "dvd/sound/music/music12.bgm",
        "dvd/sound/music/musicx01.bgm",
        "dvd/sound/music/music101.txt",
        "dvd/sound/music/other101.bgm",
        "dvd/sound/wave/wave0003.wd",
        "dvd/sound/wave/wave003.wd",
        "dvd/sound/se/block/se012.seb",
        "dvd/sound/se/sep/se000123.sep",
    });

    const std::map<uint32_t, std::string> expected = {
        {7, "dvd/sound/music/MUSIC007.BGM"},
        {101, "dvd/sound/Music/music101.bgm"},
        {1234, "dvd/sound/music/music1234.bgm"},
    };

    const std::map<uint32_t, std::string> pieces = red::Files(game, "dvd/sound/music/", red::kPiece);
    const std::map<uint32_t, std::string> banks = red::Files(game, "dvd/sound/wave/", red::kBank);
    const std::map<uint32_t, std::string> blocks = red::Files(game, "dvd/sound/se/block/", red::kBlock);
    const std::map<uint32_t, std::string> effects = red::Files(game, "dvd/sound/se/sep/", red::kEffect);

    Check(pieces == expected, "the pieces are 7, 101 and 1234, under the disc's names");
    Check(banks.size() == 1 && banks.contains(3), "the one bank is wave0003.wd");
    Check(blocks.contains(12), "block 12 is se012.seb");
    Check(effects.contains(123), "effect 123 is se000123.sep");
}

void TestLabel()
{
    Check(red::Label("dvd/sound/Music/music101.bgm", red::kPiece) == "music101", "a piece's label is music101");
    Check(red::Label("dvd/sound/wave/wave0003.wd", red::kBank) == "wave0003", "a bank's label is wave0003");
}

// A piece's header starts with "BGM ", and its bytes 6 and 7 give the number of its bank, the high byte first.
void TestPieceBank()
{
    const std::vector<uint8_t> piece = {'B', 'G', 'M', ' ', 0, 0, 0x01, 0x02, 3};
    const std::vector<uint8_t> other = {'S', 'E', 'P', ' ', 0, 0, 0x01, 0x02};
    const std::vector<uint8_t> short_piece = {'B', 'G', 'M', ' ', 0, 0, 0x01};

    const std::optional<uint32_t> piece_bank = red::PieceBank(piece);
    const std::optional<uint32_t> other_bank = red::PieceBank(other);
    const std::optional<uint32_t> short_bank = red::PieceBank(short_piece);

    Check(piece_bank == 0x102u, "the piece plays bank 0x102");
    Check(!other_bank.has_value(), "a file without the magic names no bank");
    Check(!short_bank.has_value(), "a piece that ends inside the bank's number names no bank");
}

} // namespace

int main()
{
    TestFilesByNumber();
    TestLabel();
    TestPieceBank();

    return xsflib::testing::Summary("red_files");
}

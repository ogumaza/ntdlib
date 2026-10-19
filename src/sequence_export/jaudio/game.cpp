// SPDX-License-Identifier: MIT

// A game's sound files (see game.h).

#include "ntdlib/sequence_export/jaudio/game.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"
#include "xsflib/sha1.h"

#include "ntdlib/jaudio/aaf.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

namespace fs = std::filesystem;

using xsflib::AsciiLower;
using xsflib::Utf8;

// The subfolder of `folder` named `name` in any capitals, or nothing.
std::optional<fs::path> Subfolder(const fs::path& folder, const std::string& name)
{
    std::error_code error;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
    {
        if (e.is_directory(error) && AsciiLower(Utf8(e.path().filename())) == AsciiLower(name))
        {
            return e.path();
        }
    }

    return std::nullopt;
}

} // namespace

GameRules RulesOf(const std::string& archive_sha1)
{
    // Four Swords Adventures: the SHA-1s of the USA release's and the Japanese release's start-up archives.
    constexpr std::array<std::string_view, 2> kFourSwordsAdventures{"8c55097f2cc9f5800fd8ab96da4e3b09727e0a39",
                                                                    "aab07cea25ba957374524c4f29d36b72e6d36a67"};
    GameRules rules;
    if (std::ranges::find(kFourSwordsAdventures, archive_sha1) != kFourSwordsAdventures.end())
    {
        // The game plays its system sounds, category 0, at a volume of 0.7 in each of its sound modes. The other
        // categories keep a volume of 1. The function at 0x80458744 in the USA release's main.dol takes the value from
        // a table at 0x804b1e60 and passes it to JAISoundParamsMove::moveVolume (0x800262ac). The Japanese release has
        // the same table.
        rules.category_volumes[0] = 0.7f;

        // JASOscillator::release (0x8002d8d4) gives an oscillator with neither a release table nor a direct release a
        // direct release of 0x10. Twilight Princess's build stops such an oscillator at once.
        rules.default_release = 0x10;

        // At start-up, the function at 0x80459ae4 passes the FX lines in the table at 0x80491600 to JASDsp::setFXLine
        // (0x8002cf5c). A line delays an effect send by 64 sub-frames (5,120 samples). Its filter has one tap, of
        // 0x3fff. The filter's output feeds back into the line and goes to one side of the mix at 0x6000. A sound
        // therefore repeats every 64 sub-frames: the first time at 0.375 times its send, then at half the level each
        // time. A voice at a volume of 1 gets a mixer volume of 12000: JASDriver::getChannelLevel_dsp (0x8002c328)
        // returns the halfword at 0x80539bd0. The game mixes each voice at twice its samples times the mixer volume in
        // 32768ths.
        rules.echo = EchoLine{64, 0x3fff / 32768.0f, 0x6000 / 32768.0f, 2.0f * 12000.0f / 32768.0f};
    }

    return rules;
}

Game Game::FromFolder(const fs::path& baa, const std::string& program)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(baa, bytes))
    {
        throw FormatError("can't read it");
    }

    Game game;
    game.OpenArchive(std::move(bytes));

    // The files in `where` with the extension `extension`, in the order of their names. AppleDouble files don't count.
    // macOS writes one, named "._" and the file's name, beside a file that it copies to a FAT or exFAT drive.
    const auto files_in = [](const fs::path& where, const std::string& extension)
    {
        std::vector<fs::path> found;
        std::error_code error;
        for (const fs::directory_entry& e : fs::directory_iterator(where, error))
        {
            if (e.is_regular_file(error) && AsciiLower(Utf8(e.path().extension())) == extension &&
                !Utf8(e.path().filename()).starts_with("._"))
            {
                found.push_back(e.path());
            }
        }
        std::sort(found.begin(), found.end());

        return found;
    };

    // The sequence archive: the first .arc in the Seqs folder, or beside the start-up archive. A JAudio1 game without
    // one can keep its sequences in a table (.bdt).
    const fs::path folder = baa.parent_path().empty() ? fs::path(".") : baa.parent_path();
    std::vector<fs::path> archives;
    for (const std::string extension : {".arc", ".bdt"})
    {
        if (extension == ".bdt" && !game.jaudio1_)
        {
            break;
        }
        for (const fs::path& where : {Subfolder(folder, "Seqs").value_or(folder), folder})
        {
            archives = files_in(where, extension);
            if (!archives.empty())
            {
                break;
            }
        }
        if (!archives.empty())
        {
            game.sequence_table_ = extension == ".bdt";
            break;
        }
    }
    if (!archives.empty())
    {
        game.sequence_archive_name_ = Utf8(archives.front().filename());
        // A sequence archive that can't be read stays empty.
        xsflib::ReadFile(archives.front(), game.sequence_archive_);
        if (archives.size() > 1)
        {
            game.warnings_.push_back("several sequence archives: " + program + " reads " + game.sequence_archive_name_);
        }
    }

    if (const std::optional<fs::path> waves = Subfolder(folder, game.jaudio1_ ? "Banks" : "Waves"))
    {
        game.wave_folders_.push_back(*waves);
    }
    game.wave_folders_.push_back(folder);
    if (game.jaudio1_)
    {
        const std::vector<fs::path> name_files = files_in(folder, ".asn");
        std::vector<uint8_t> names;
        if (!name_files.empty() && xsflib::ReadFile(name_files.front(), names))
        {
            game.ReadNames(names, Utf8(name_files.front().filename()));
        }
    }
    game.FindSequences();

    return game;
}

std::optional<Game> Game::FromDiscFiles(std::shared_ptr<ntdlib::disc::GameFiles> disc, const std::string& program)
{
    Game game;
    game.disc_ = std::move(disc);

    // The disc's files in the order of its file table.
    std::vector<const ntdlib::disc::GameFile*> files;
    for (const ntdlib::disc::GameFile& f : game.disc_->files)
    {
        files.push_back(&f);
    }
    std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->index < b->index; });

    // The start-up archive: a .baa file, with its Seqs and Waves folders beside it, or JAudio1's .aaf file, with its
    // Seqs and Banks folders.
    const ntdlib::disc::GameFile* baa = nullptr;
    for (const ntdlib::disc::GameFile* f : files)
    {
        if (AsciiLower(f->path).ends_with(".baa") || AsciiLower(f->path).ends_with(".aaf"))
        {
            if (baa != nullptr)
            {
                game.warnings_.push_back("several start-up archives on the disc: " + program + " reads " + baa->path);
                break;
            }
            baa = f;
        }
    }
    if (baa == nullptr)
    {
        return std::nullopt;
    }

    game.OpenArchive(game.ReadDiscFile(baa->path));
    game.disc_path_ = baa->path;
    const std::string folder = AsciiLower(baa->path.substr(0, baa->path.rfind('/') + 1));
    for (const ntdlib::disc::GameFile* f : files)
    {
        const std::string path = AsciiLower(f->path);
        if (!path.starts_with(folder))
        {
            continue;
        }

        const std::string below = path.substr(folder.size());
        if (below.starts_with("seqs/") && (below.ends_with(".arc") || (game.jaudio1_ && below.ends_with(".bdt"))) &&
            game.sequence_archive_.empty())
        {
            game.sequence_archive_ = game.ReadDiscFile(f->path);
            game.sequence_archive_name_ = f->path.substr(f->path.rfind('/') + 1);
            game.sequence_table_ = below.ends_with(".bdt");
        }
        else if (below.starts_with(game.jaudio1_ ? "banks/" : "waves/") && below.ends_with(".aw"))
        {
            game.disc_waves_.emplace(below.substr(6), f->path);
        }
        else if (game.jaudio1_ && below.find('/') == std::string::npos && below.ends_with(".asn") &&
                 game.names_.empty())
        {
            game.ReadNames(game.ReadDiscFile(f->path), f->path.substr(f->path.rfind('/') + 1));
        }
    }

    game.FindSequences();

    return game;
}

void Game::OpenArchive(std::vector<uint8_t> bytes)
{
    rules_ = RulesOf(xsflib::Sha1::Hex(xsflib::Sha1::Of(bytes)));

    if (!ntdlib::jaudio::IsAaf(bytes))
    {
        archive_ = std::make_unique<SoundArchive>(SoundArchive::Parse(std::move(bytes)));
        return;
    }

    // JASystem::TOscillator::release gives an oscillator with neither a release table nor a direct release a direct
    // release of 0x10.
    jaudio1_ = true;
    rules_.default_release = 0x10;
    effects_file_ = ntdlib::jaudio::AafEffectsSequenceFile(bytes);
    archive_ = std::make_unique<SoundArchive>(SoundArchive::ParseAaf(std::move(bytes)));
}

void Game::ReadNames(std::span<const uint8_t> names, const std::string& file)
{
    try
    {
        for (ntdlib::jaudio::AsnSound& sound : ntdlib::jaudio::ReadAsnSounds(names))
        {
            names_.emplace(sound.id, std::move(sound.name));
        }
    }
    catch (const FormatError& e)
    {
        warnings_.push_back(file + ": " + e.what() + ": its names are left out");
    }
}

std::vector<uint8_t> Game::ReadDiscFile(const std::string& path) const
{
    std::vector<uint8_t> data;
    try
    {
        disc_->read_file(path, data);
    }
    catch (const ntdlib::disc::Error& e)
    {
        throw FormatError(e.what());
    }

    return data;
}

void Game::FindSequences()
{
    if (sequence_archive_.empty())
    {
        warnings_.push_back("no sequence archive found: its music is skipped");
    }
    else
    {
        try
        {
            // Twilight Princess compresses its sequence archive with Yaz0.
            if (IsYaz0(sequence_archive_))
            {
                sequence_archive_ = DecodeYaz0(sequence_archive_);
            }

            sequence_files_ =
                sequence_table_ ? ntdlib::jaudio::ReadSequenceTable(sequence_archive_) : ReadRarc(sequence_archive_);
        }
        catch (const FormatError& e)
        {
            warnings_.push_back(sequence_archive_name_ + ": " + e.what() + ": its music is skipped");
        }
    }

    // The sound table's sequences. A sound of the collection starts at its offset in the collection; a sound of the
    // sequence archive at the start of its file.
    std::vector<bool> played(sequence_files_.size());
    std::map<uint16_t, uint8_t> unnamed; // the volume of the first JAudio1 sound that plays each file
    for (const SoundEntry& s : archive_->Sounds())
    {
        // A stream plays an audio file from the disc, not a sequence. A JAudio1 game's effects play in its effects
        // sequence.
        if ((jaudio1_ && s.source != SoundEntry::Source::kArchive) || (!jaudio1_ && LayoutOf(s.type).stream))
        {
            continue;
        }

        SequenceSound seq;
        seq.name = s.name.empty() ? "SOUND_" + std::to_string(s.section) + "_" + std::to_string(s.group) + "_" +
                                        std::to_string(s.index)
                                  : s.name;
        seq.volume = static_cast<float>(s.volume) / 127.0f;
        seq.default_release = rules_.default_release;
        seq.echo = rules_.echo;
        seq.jaudio1 = jaudio1_;
        if (s.source == SoundEntry::Source::kCollection)
        {
            const std::optional<uint32_t> start = archive_->CollectionSequence(s.group, s.resource);
            if (!start)
            {
                continue;
            }
            seq.data = archive_->Collection();
            seq.start = *start;

            // The collection's sounds are effects. Each plays at the volume of its category too.
            const auto category = rules_.category_volumes.find(s.group);
            if (category != rules_.category_volumes.end())
            {
                seq.volume *= category->second;
            }
        }
        else
        {
            const auto file = std::find_if(sequence_files_.begin(), sequence_files_.end(),
                                           [&](const ArchiveFile& f) { return f.id == s.resource; });
            if (file == sequence_files_.end())
            {
                continue;
            }
            played[static_cast<std::size_t>(file - sequence_files_.begin())] = true;
            seq.data = file->data;
            seq.start = 0;

            // A JAudio1 sequence takes its name from the name file. Without one, it takes its file's name. A later
            // sound without a name that plays the same file at the same volume would give the same conversion. It's
            // left out. One at another volume keeps the name of its ID.
            if (jaudio1_)
            {
                const auto named = names_.find(s.Id());
                const auto [first, fresh] = unnamed.try_emplace(file->id, s.volume);
                if (named != names_.end())
                {
                    seq.name = named->second;
                }
                else if (!fresh && first->second == s.volume)
                {
                    continue;
                }
                else if (fresh && !file->name.empty())
                {
                    seq.name = file->name.substr(0, file->name.rfind('.'));
                }
            }
        }
        sequences_.push_back(std::move(seq));
    }

    // The files of the sequence archive that no sound plays, but a JAudio1 game's effects sequence. A file of a
    // sequence table has no name. It takes its number.
    for (std::size_t i = 0; i < sequence_files_.size(); i++)
    {
        const ArchiveFile& file = sequence_files_[i];
        if (!played[i] && !file.data.empty() && !(jaudio1_ && file.id == effects_file_))
        {
            const std::string name =
                file.name.empty() ? "SEQUENCE_" + std::to_string(file.id) : file.name.substr(0, file.name.rfind('.'));
            SequenceSound seq{name, file.data, 0, 1.0f, rules_.default_release, rules_.echo};
            seq.jaudio1 = jaudio1_;
            sequences_.push_back(std::move(seq));
        }
    }
}

std::optional<std::vector<uint8_t>> Game::ReadWaveArchive(const std::string& name) const
{
    if (disc_)
    {
        const auto f = disc_waves_.find(AsciiLower(name));
        if (f == disc_waves_.end())
        {
            return std::nullopt;
        }

        return ReadDiscFile(f->second);
    }

    std::error_code error;
    for (const fs::path& folder : wave_folders_)
    {
        for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
        {
            if (e.is_regular_file(error) && AsciiLower(Utf8(e.path().filename())) == AsciiLower(name))
            {
                std::vector<uint8_t> data;
                return xsflib::ReadFile(e.path(), data) ? std::optional(std::move(data)) : std::nullopt;
            }
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::sequence_export::jaudio

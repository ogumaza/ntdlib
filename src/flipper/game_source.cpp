// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/game_source.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/program.h"

#include "ntdlib/disc/header.h"
#include "ntdlib/flipper/file_disc.h"
#include "ntdlib/ppc/cpu.h"

namespace ntdlib::flipper
{

std::unique_ptr<FileDisc> DiscFromFiles(const std::vector<xsflib::psf::FileChunk>& files,
                                        const std::string& header_path, uint32_t magic_offset, uint32_t magic,
                                        uint32_t offset_unit)
{
    std::map<std::string, const std::vector<uint8_t>*> latest;
    for (const xsflib::psf::FileChunk& file : files)
    {
        latest[file.path] = &file.data;
    }

    std::vector<uint8_t> header(disc::kHeaderSize, 0);
    xsflib::StoreBe32(&header[magic_offset], magic);
    std::vector<std::pair<std::string, std::span<const uint8_t>>> disc_files;
    for (const auto& [path, data] : latest)
    {
        if (path == header_path)
        {
            header = *data;
        }
        else
        {
            disc_files.emplace_back(path, *data);
        }
    }

    return std::make_unique<FileDisc>(std::move(header), disc_files, offset_unit);
}

void AppendAudioBlock(std::vector<int16_t>& out, std::span<const uint8_t, 32> block)
{
    for (int i = 0; i < 8; i++)
    {
        out.push_back(static_cast<int16_t>(xsflib::LoadBe16(&block[i * 4 + 2])));
        out.push_back(static_cast<int16_t>(xsflib::LoadBe16(&block[i * 4])));
    }
}

std::optional<std::string> StopMessage(ppc::StopReason reason)
{
    if (reason == ppc::StopReason::kFault)
    {
        return "the emulated program accessed memory that isn't there";
    }

    if (reason == ppc::StopReason::kIllegal)
    {
        return "the emulated program ran an instruction that the CPU doesn't support";
    }

    return std::nullopt;
}

} // namespace ntdlib::flipper

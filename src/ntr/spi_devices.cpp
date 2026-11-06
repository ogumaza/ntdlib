// SPDX-License-Identifier: MIT

#include "spi_devices.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/rom_writer.h"

namespace ntdlib::ntr
{

namespace
{

// The firmware's flash memory: 256 KiB, with the user's settings in its last two blocks of 0x100 bytes.
constexpr std::size_t kFlashSize = 0x40000;
constexpr std::size_t kUserSettings = 0x3FE00;
constexpr std::size_t kUserSettingsCopy = 0x3FF00;
constexpr std::size_t kUserSettingsSize = 0x70;

// The firmware header's fields: the console's type (0xFF: the first DS), and the place of the user's settings in
// units of 8 bytes.
constexpr std::size_t kFirmwareHeader = 0x200;
constexpr std::size_t kConsoleType = 0x1D;
constexpr std::size_t kUserSettingsPlace = 0x20;

// The memories' commands. 0x0A and 0x0B are the writes and reads of an EEPROM of 512 bytes with address bit 8 set, and
// a flash memory's page write and fast read.
enum MemoryCommand : uint8_t
{
    kWriteStatus = 0x01,
    kWrite = 0x02,
    kRead = 0x03,
    kWriteDisable = 0x04,
    kReadStatus = 0x05,
    kWriteEnable = 0x06,
    kWriteHigh = 0x0A,
    kReadHigh = 0x0B,
    kReadId = 0x9F,
    kSectorErase = 0xD8,
    kPageErase = 0xDB,
};

constexpr std::size_t kPageSize = 0x100;
constexpr std::size_t kSectorSize = 0x10000;

// The status register: the write enable latch, and the bits that a write of the register sets (the block protection
// and the write protection of the register).
constexpr uint8_t kWriteEnableLatch = 1u << 1;
constexpr uint8_t kProtectionBits = 0x8C;

// A flash memory's ID: the maker, the memory's type, then the capacity as a power of two.
constexpr uint8_t kFlashMaker = 0x20;
constexpr uint8_t kFlashType = 0x40;

// The devices on the ARM7's bus, by SPICNT's bits 8 and 9.
constexpr int kPowerDevice = 0;
constexpr int kFlashDevice = 1;
constexpr int kTouchDevice = 2;

// The power management device's register 0 at power-on: the sound amplifier and both backlights on.
constexpr uint8_t kPowerControl = 0x0D;

std::vector<uint8_t> FirmwareImage()
{
    std::vector<uint8_t> flash(kFlashSize, 0xFF);
    std::fill(flash.begin(), flash.begin() + kFirmwareHeader, 0);
    flash[kConsoleType] = 0xFF;
    xsflib::StoreLe16(&flash[kUserSettingsPlace], static_cast<uint16_t>(kUserSettings / 8));

    // Both copies, with update counts of 0 and 1, then each copy's CRC.
    const std::array<uint8_t, 0x70> settings = FirmwareUserSettings();
    for (const std::size_t place : {kUserSettings, kUserSettingsCopy})
    {
        std::copy(settings.begin(), settings.end(), flash.begin() + static_cast<std::ptrdiff_t>(place));
        xsflib::StoreLe16(&flash[place + kUserSettingsSize], place == kUserSettings ? 0 : 1);
        xsflib::StoreLe16(&flash[place + kUserSettingsSize + 2],
                          Crc16(std::span(flash).subspan(place, kUserSettingsSize)));
    }
    return flash;
}

} // namespace

std::array<uint8_t, 0x70> FirmwareUserSettings()
{
    std::array<uint8_t, 0x70> settings = {};
    settings[0x00] = 5;
    settings[0x03] = 1;
    settings[0x04] = 1;
    settings[0x06] = 'D';
    settings[0x08] = 'S';
    settings[0x1A] = 2;
    xsflib::StoreLe16(&settings[0x58], 0x0200); // ADC x and y, then the screen's x and y, of the first point
    xsflib::StoreLe16(&settings[0x5A], 0x0200);
    settings[0x5C] = 0x20;
    settings[0x5D] = 0x20;
    xsflib::StoreLe16(&settings[0x5E], 0x0E00); // the second point
    xsflib::StoreLe16(&settings[0x60], 0x0E00);
    settings[0x62] = 0xE0;
    settings[0x63] = 0xA0;
    settings[0x64] = 0x31; // the language in bits 0 to 2, the backlight in bits 4 and 5
    return settings;
}

SpiMemory::SpiMemory(int address_bytes, std::vector<uint8_t> data)
    : data_(std::move(data)), address_bytes_(address_bytes)
{
}

uint8_t SpiMemory::Transfer(uint8_t value, bool first)
{
    if (first)
    {
        command_ = value;
        position_ = 1;
        address_ = 0;
        if (command_ == kWriteEnable)
        {
            write_enabled_ = true;
        }
        else if (command_ == kWriteDisable)
        {
            write_enabled_ = false;
        }
        return 0xFF;
    }

    const auto at = static_cast<int>(position_++);
    const bool flash = address_bytes_ == 3;
    switch (command_)
    {
    case kReadStatus:
        return static_cast<uint8_t>(protection_ | (write_enabled_ ? kWriteEnableLatch : 0));
    case kWriteStatus:
        if (!flash && at == 1 && write_enabled_)
        {
            protection_ = value & kProtectionBits;
        }
        return 0xFF;
    case kReadId:
        if (!flash || at > 3)
        {
            return 0xFF;
        }
        return at == 1 ? kFlashMaker : at == 2 ? kFlashType : static_cast<uint8_t>(std::countr_zero(data_.size()));
    default:
        break;
    }

    const bool read = command_ == kRead || (command_ == kReadHigh && address_bytes_ != 2);
    const bool write = command_ == kWrite || (command_ == kWriteHigh && address_bytes_ != 2);
    const bool erase = flash && (command_ == kPageErase || command_ == kSectorErase);
    if (!read && !write && !erase)
    {
        return 0xFF;
    }

    // The address, the high byte first.
    if (at <= address_bytes_)
    {
        address_ = address_ << 8 | value;
        if (at < address_bytes_)
        {
            return 0xFF;
        }
        if (address_bytes_ == 1 && (command_ & 8) != 0)
        {
            address_ |= 0x100;
        }
        address_ %= static_cast<uint32_t>(data_.size());
        if (erase && write_enabled_)
        {
            const std::size_t size = std::min(command_ == kPageErase ? kPageSize : kSectorSize, data_.size());
            const std::size_t start = address_ / size * size;
            std::fill_n(data_.begin() + static_cast<std::ptrdiff_t>(start), size, 0xFF);
        }
        return 0xFF;
    }

    // A flash memory's fast read waits a byte before its data.
    if (flash && command_ == kReadHigh && at == address_bytes_ + 1)
    {
        return 0xFF;
    }

    uint8_t answer = 0xFF;
    if (read)
    {
        answer = data_[address_];
    }
    else if (write && write_enabled_)
    {
        // A flash memory's page program can only clear bits.
        data_[address_] = flash && command_ == kWrite ? static_cast<uint8_t>(data_[address_] & value) : value;
    }
    address_ = static_cast<uint32_t>((address_ + 1) % data_.size());
    return answer;
}

void SpiMemory::End()
{
    if (position_ > 0 && IsWrite())
    {
        write_enabled_ = false;
    }
    position_ = 0;
}

std::span<uint8_t> SpiMemory::Data()
{
    return data_;
}

bool SpiMemory::IsWrite() const
{
    switch (command_)
    {
    case kWriteStatus:
    case kWrite:
        return true;
    case kWriteHigh:
        return address_bytes_ != 2;
    case kPageErase:
    case kSectorErase:
        return address_bytes_ == 3;
    default:
        return false;
    }
}

SpiDevices::SpiDevices() : firmware_(3, FirmwareImage())
{
    power_[0] = kPowerControl;
}

uint8_t SpiDevices::Transfer(int device, uint8_t value, bool hold)
{
    const bool first = selected_ != device;
    if (first)
    {
        Deselect();
        selected_ = device;
        position_ = 0;
    }

    uint8_t answer = 0xFF;
    switch (device)
    {
    case kPowerDevice:
        answer = PowerByte(value);
        break;
    case kFlashDevice:
        answer = firmware_.Transfer(value, first);
        break;
    case kTouchDevice:
        answer = 0;
        break;
    default:
        break;
    }
    position_++;
    if (!hold)
    {
        Deselect();
    }
    return answer;
}

void SpiDevices::Deselect()
{
    if (selected_ == kFlashDevice)
    {
        firmware_.End();
    }
    selected_ = -1;
}

uint8_t SpiDevices::PowerByte(uint8_t value)
{
    // The first byte gives the register in bits 0 to 6, and a read in bit 7. The second reads or writes it.
    if (position_ == 0)
    {
        power_command_ = value;
        return 0xFF;
    }

    const std::size_t index = (power_command_ & 0x7F) % power_.size();
    if ((power_command_ & 0x80) != 0)
    {
        return power_[index];
    }
    power_[index] = value;
    return 0xFF;
}

} // namespace ntdlib::ntr

// SPDX-License-Identifier: MIT

// The DS's memories and devices on SPI buses: the firmware's flash memory and the power management and touch screen
// devices on the ARM7's bus, and a cartridge's backup memory. The layouts and the commands follow GBATEK's
// descriptions of the devices and of the firmware.
//
// The firmware's flash memory holds settings alone. Its header gives the place of the user's settings. The memory holds
// the settings twice, each copy with its CRC. The rest reads as erased, 0xFF. The wireless settings are therefore
// invalid. The power management device keeps the values written to its registers. The touch screen's controller answers
// 0 on every channel: the pen is up.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::ntr
{

// The firmware's user settings: version 5, the birthday 1 January, the nickname "DS", the touch screen's calibration at
// two points, English, and the backlight at its brightest. The firmware copies them to main memory at 0x027FFC80.
std::array<uint8_t, 0x70> FirmwareUserSettings();

// A memory on an SPI bus, addressed by 1, 2 or 3 bytes: an EEPROM of 512 bytes, whose read and write commands give
// address bit 8 in their bit 3, a larger EEPROM or a FRAM, or a flash memory. It answers its status, its ID (a flash
// memory only), reads, writes and erases. Each write takes effect at once, and the status never shows one in progress.
// As on the devices, a write requires the write enable command first.
class SpiMemory
{
public:
    SpiMemory(int address_bytes, std::vector<uint8_t> data);

    // A byte of a command, `first` for its first one. Returns the byte that the memory sends back.
    uint8_t Transfer(uint8_t value, bool first);

    // The command ends as the memory is deselected.
    void End();

    std::span<uint8_t> Data();

private:
    bool IsWrite() const;

    std::vector<uint8_t> data_;
    int address_bytes_;
    std::size_t position_ = 0; // the bytes of the command so far
    uint8_t command_ = 0;
    uint32_t address_ = 0;
    bool write_enabled_ = false;
    uint8_t protection_ = 0; // the status register's bits that a write of it sets
};

class SpiDevices
{
public:
    SpiDevices();

    // A transfer of a byte with device `device` (SPICNT's bits 8 and 9). Returns the byte that the device sends back.
    // The device stays selected after it while `hold` is set.
    uint8_t Transfer(int device, uint8_t value, bool hold);

    // Ends a command that a transfer held selected. A write of SPICNT that turns the bus off does this.
    void Deselect();

private:
    uint8_t PowerByte(uint8_t value);

    SpiMemory firmware_;
    std::array<uint8_t, 8> power_ = {};

    int selected_ = -1;        // the device that a command holds selected, or -1
    std::size_t position_ = 0; // the bytes of the command so far
    uint8_t power_command_ = 0;
};

} // namespace ntdlib::ntr

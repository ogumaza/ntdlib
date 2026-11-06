// SPDX-License-Identifier: MIT

// The ARM946E-S's system control coprocessor (CP15) with its tightly coupled memories, and the memory accesses that
// miss the page caches. The registers follow GBATEK's "ARM CP15 System Control Coprocessor".

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include "arm/cpu_impl.h"

namespace ntdlib::arm::detail
{
namespace
{

// The ARM946E-S's identification registers (c0,c0,0 to c0,c0,2): ARMv5TE, ARM946, revision 1; separate caches of 8 KiB
// (instructions) and 4 KiB (data) with lines of 32 bytes; an ITCM of 32 KiB and a DTCM of 16 KiB.
constexpr uint32_t kMainId = 0x41059461;
constexpr uint32_t kCacheType = 0x0F0D2112;
constexpr uint32_t kTcmSize = 0x00140180;

// The control register's bits that a program can write, and the bits that always read as set.
constexpr uint32_t kControlWritable = 0x000FF085;
constexpr uint32_t kControlFixed = 0x00000078;

// The bits of the region registers (c6) and of the TCM settings (c9,c1) that exist.
constexpr uint32_t kRegionBits = 0xFFFFF03F;
constexpr uint32_t kDtcmBits = 0xFFFFF03E;
constexpr uint32_t kItcmBits = 0x0000003E; // the ITCM's base is fixed at 0

// Sets a TCM's region from its c9,c1 setting: the size is 512 bytes shifted left by bits 1 to 5. From a shift of 23 on,
// the region covers the whole address space.
void SetRegion(Tcm& tcm, uint32_t setting)
{
    tcm.setting = setting;
    const uint32_t shift = (setting >> 1) & 31;
    const uint64_t size = shift >= 23 ? uint64_t{1} << 32 : uint64_t{512} << shift;
    tcm.region_mask = static_cast<uint32_t>(~(size - 1));
    tcm.base = setting & 0xFFFFF000 & tcm.region_mask;
    tcm.span_mask = static_cast<uint32_t>(std::min<uint64_t>(size, tcm.storage.size()) - 1);
    tcm.small = size < kPageSize;
}

} // namespace

void Core::UpdateTcms()
{
    itcm_.enabled = (control_ & control::kItcmEnable) != 0;
    itcm_.load_mode = (control_ & control::kItcmLoadMode) != 0;
    dtcm_.enabled = (control_ & control::kDtcmEnable) != 0;
    dtcm_.load_mode = (control_ & control::kDtcmLoadMode) != 0;
    SetRegion(itcm_, itcm_.setting);
    SetRegion(dtcm_, dtcm_.setting);
    InvalidatePages();
}

void Core::InvalidatePages()
{
    fetch_page_ = kNoPage;
    fetch_host_ = nullptr;
    for (PageCache* cache : {&fetch_pages_, &read_pages_, &write_pages_})
    {
        cache->fill(PageEntry{});
    }
}

uint8_t* Core::TcmFor(uint32_t address, Access access)
{
    // The ITCM wins where the two regions overlap.
    if (itcm_.enabled && (access != Access::kRead || !itcm_.load_mode) && TcmContains(itcm_, address))
    {
        return TcmAt(itcm_, address);
    }

    if (dtcm_.enabled && access != Access::kFetch && (access != Access::kRead || !dtcm_.load_mode) &&
        TcmContains(dtcm_, address))
    {
        return TcmAt(dtcm_, address);
    }

    return nullptr;
}

void Core::Fill(PageEntry& entry, uint32_t page, Access access)
{
    entry.page = page;
    entry.mixed = false;
    entry.host = nullptr;

    const uint32_t address = page << kPageBits;
    if (v5_)
    {
        for (Tcm* tcm : {&itcm_, &dtcm_})
        {
            const bool serves = tcm->enabled && (tcm == &itcm_ || access != Access::kFetch) &&
                                (access != Access::kRead || !tcm->load_mode);
            if (!serves || ((address ^ tcm->base) & tcm->region_mask & ~kPageOffsetMask) != 0)
            {
                continue;
            }

            // A region smaller than a page leaves the rest of the page to the bus, or to the other TCM.
            if (tcm->small)
            {
                entry.mixed = true;
                return;
            }

            entry.host = TcmAt(*tcm, address);
            return;
        }
    }

    entry.host = bus_.Page(address);
}

uint32_t Core::ReadUncached(const PageEntry& entry, uint32_t address, int size, Access access)
{
    if (entry.mixed)
    {
        if (uint8_t* tcm = TcmFor(address, access))
        {
            return LoadHost(tcm, size);
        }

        if (uint8_t* host = bus_.Page(address & ~kPageOffsetMask))
        {
            return LoadHost(host + (address & kPageOffsetMask), size);
        }
    }

    return bus_.Read(address, size);
}

uint32_t Core::SlowFetch(uint32_t address, int size)
{
    const uint32_t page = address >> kPageBits;
    PageEntry& entry = fetch_pages_[page % kPageCacheSize];
    if (entry.page != page)
    {
        Fill(entry, page, Access::kFetch);
    }

    if (entry.host != nullptr)
    {
        fetch_page_ = page;
        fetch_host_ = entry.host;
        return LoadHost(entry.host + (address & kPageOffsetMask), size);
    }

    return ReadUncached(entry, address, size, Access::kFetch);
}

uint32_t Core::SlowRead(uint32_t address, int size)
{
    const uint32_t page = address >> kPageBits;
    PageEntry& entry = read_pages_[page % kPageCacheSize];
    if (entry.page != page)
    {
        Fill(entry, page, Access::kRead);
    }

    if (entry.host != nullptr)
    {
        return LoadHost(entry.host + (address & kPageOffsetMask), size);
    }

    return ReadUncached(entry, address, size, Access::kRead);
}

void Core::SlowWrite(uint32_t address, int size, uint32_t value)
{
    const uint32_t page = address >> kPageBits;
    PageEntry& entry = write_pages_[page % kPageCacheSize];
    if (entry.page != page)
    {
        Fill(entry, page, Access::kWrite);
    }

    if (entry.host != nullptr)
    {
        StoreHost(entry.host + (address & kPageOffsetMask), size, value);
        return;
    }

    if (entry.mixed)
    {
        if (uint8_t* tcm = TcmFor(address, Access::kWrite))
        {
            StoreHost(tcm, size, value);
            return;
        }

        if (uint8_t* host = bus_.Page(address & ~kPageOffsetMask))
        {
            StoreHost(host + (address & kPageOffsetMask), size, value);
            return;
        }
    }

    bus_.Write(address, size, value);
}

bool Core::ReadCp15(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t& value)
{
    if (opcode1 != 0)
    {
        return false;
    }

    value = 0;
    switch (crn)
    {
    case 0:
        if (crm == 0)
        {
            value = opcode2 == 1 ? kCacheType : opcode2 == 2 ? kTcmSize : kMainId;
        }
        break;

    case 1:
        if (crm == 0 && opcode2 == 0)
        {
            value = control_;
        }
        break;

    case 2:
        if (crm == 0 && opcode2 <= 1)
        {
            value = cachable_[opcode2];
        }
        break;

    case 3:
        if (crm == 0 && opcode2 == 0)
        {
            value = write_bufferable_;
        }
        break;

    case 5:
        if (crm == 0 && opcode2 <= 3)
        {
            value = permissions_[opcode2];
        }
        break;

    case 6:
        // The data and instruction regions share their registers.
        if (opcode2 <= 1)
        {
            value = regions_[crm & 7];
        }
        break;

    case 9:
        if (crm == 0 && opcode2 <= 1)
        {
            value = lockdown_[opcode2];
        }
        else if (crm == 1 && opcode2 <= 1)
        {
            value = opcode2 == 0 ? dtcm_.setting : itcm_.setting;
        }
        break;

    case 13:
        // The process ID (c13,c0,0) is fixed at 0. The trace process ID has a mirror at c13,c1,1.
        if (opcode2 == 1)
        {
            value = trace_process_id_;
        }
        break;

    default:
        break;
    }

    return true;
}

bool Core::WriteCp15(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t value)
{
    if (opcode1 != 0)
    {
        return false;
    }

    switch (crn)
    {
    case 1:
        if (crm == 0 && opcode2 == 0)
        {
            control_ = (value & kControlWritable) | kControlFixed;
            load_interworks_ = (control_ & control::kPreArmV5) == 0;
            UpdateTcms();
        }
        break;

    case 2:
        if (crm == 0 && opcode2 <= 1)
        {
            cachable_[opcode2] = value & 0xFF;
        }
        break;

    case 3:
        if (crm == 0 && opcode2 == 0)
        {
            write_bufferable_ = value & 0xFF;
        }
        break;

    case 5:
        if (crm == 0 && opcode2 <= 3)
        {
            permissions_[opcode2] = opcode2 <= 1 ? value & 0xFFFF : value;
        }
        break;

    case 6:
        if (opcode2 <= 1)
        {
            regions_[crm & 7] = value & kRegionBits;
        }
        break;

    case 7:
        // The cache operations do nothing. Two of them wait for an interrupt.
        if ((crm == 0 && opcode2 == 4) || (crm == 8 && opcode2 == 2))
        {
            Halt();
        }
        break;

    case 9:
        if (crm == 0 && opcode2 <= 1)
        {
            lockdown_[opcode2] = value;
        }
        else if (crm == 1 && opcode2 == 0)
        {
            dtcm_.setting = value & kDtcmBits;
            UpdateTcms();
        }
        else if (crm == 1 && opcode2 == 1)
        {
            itcm_.setting = value & kItcmBits;
            UpdateTcms();
        }
        break;

    case 13:
        if (opcode2 == 1)
        {
            trace_process_id_ = value;
        }
        break;

    default:
        break;
    }

    return true;
}

} // namespace ntdlib::arm::detail

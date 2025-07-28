#include <string.h>
#include "vfat.hpp"
#include "../syscall.h"
#include "../../kernel/fat.hpp"
#include "../../kernel/vmm/vm_syscall.hpp"

// filesystem is shared with host
VFAT::VFAT(uint8_t id, uint64_t reg_addr)
{
    vfat_header_raw = std::make_unique<uint8_t[]>(0x3000);
    const auto vfat_align = (uint64_t)((uint64_t)(vfat_header_raw.get() + 0xFFF) &  ~(0xFFFULL));

    fat_info host_fat = {};
    SyscallControlVM(id, vm_control::query_host_fat_info, &host_fat, sizeof(host_fat));

    header_addr = (fat::BPB *)(vfat_align + host_fat.head_offset);
    SyscallSetMemory(id, vfat_align, reg_addr, 0x2000, 0); // consider offset

    // todo: query fat info from host
    memset(header_addr, 0, 0x1000);
    header_addr->bytes_per_sector       = 512;
    header_addr->sectors_per_cluster    = 2;
    header_addr->root_cluster           = 2;
    header_addr->num_fats               = 2;
    header_addr->reserved_sector_count  = 32;
    header_addr->fat_size_32            = 1588;

    uintptr_t fat_offset = header_addr->reserved_sector_count * header_addr->bytes_per_sector;

    guest_bpb_addr  = reg_addr       + host_fat.head_offset;
    first_fat_addr  = guest_bpb_addr + fat_offset;
    last_addr       = guest_bpb_addr + host_fat.fat_size;

    fat_addr vfat_info = {};
    vfat_info.guest_start     = reg_addr;
    vfat_info.guest_first_fat = first_fat_addr;
    vfat_info.guest_end       = last_addr;
    SyscallControlVM(id, vm_control::set_fat_addr, &vfat_info, sizeof(vfat_info));
}

uint64_t VFAT::get_guest_bpb_addr()
{
    return guest_bpb_addr;
}

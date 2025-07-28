#pragma once

#include <stdint.h>
#include <array>
#include "vm_syscall.hpp"
#include "EPT.hpp"
#include "vdevice.hpp"

struct vm_context {
    bool used = false;
    bool run = false;
    context ctx;
    uint64_t vmxon_region;
    uint64_t vmcs_region;
    uint64_t msr_host_region;
    uint64_t msr_guest_region;
    uint64_t msr_bitmap_region;
    ept_info *vm_ept;
    uint64_t next_instr_len = 0;
    exit_info exit;
    fat_addr fat_addr;
    std::unique_ptr<VDevice> dev;
};


uint8_t CreateVMInternal();
void DestroyVMInternal(uint8_t vm_id);
bool StartVMInternal(uint8_t vm_id);
bool SetMemoryInternal(uint8_t vm_id, uint64_t host_addr, uint64_t guest_addr, uint64_t mem_size, uint32_t protect);
bool ControlVMInternal(uint8_t vm_id, vm_control control_id, void *buffer, uint64_t size);
#pragma once

#include <stdint.h>

namespace vmm
{
    constexpr uint64_t PAGE_SIZE = 4096;
    constexpr uint8_t MAX_VM = 1;
}

struct context
{
    uint64_t CR3        = 0;
    uint64_t GDTR_BASE  = 0;
    uint64_t GDTR_LIMIT = 0;
    uint64_t rflags     = 0;

alignas(16) uint64_t rax = 0;
    uint64_t rcx    = 0;
    uint64_t rdx    = 0;
    uint64_t rbx    = 0;
    uint64_t rsp    = 0;
    uint64_t rbp    = 0;
    uint64_t rsi    = 0;
    uint64_t rdi    = 0;
    uint64_t r8     = 0;
    uint64_t r9     = 0;
    uint64_t r10    = 0;
    uint64_t r11    = 0;
    uint64_t r12    = 0;
    uint64_t r13    = 0;
    uint64_t r14    = 0;
    uint64_t r15    = 0;
    uint64_t rip    = 0;
};

struct exit_info
{
    uint32_t exit_reason = 0;
    uint64_t qualification = 0;
    uint64_t inv_addr = 0;
    void clear()
    {
        exit_reason = 0;
        qualification = 0;
        inv_addr = 0;
    }
};

struct device_info
{
    uint32_t acpi_io;
    uint32_t timer_interval;
};

struct fat_addr
{
    uint64_t guest_start;
    uint64_t guest_first_fat;
    uint64_t guest_end;
};

struct fat_info
{
    uint8_t head_offset;
    uint64_t fat_size;
};

enum class device_event
{
    mouse,
    keyboard,
    timer,
    event_num,
};

struct intr_data
{
    device_event event;
    union _data
    {
        struct mouse_data
        {
            int x;
            int y;
            bool change_bt;
            bool press;
            int button;
            bool reset;
        } mouse;
        struct keyboard_data
        {
            uint8_t modifier;
            uint8_t keycode;
            bool press;
        } keyboard;
    } data;
};


enum class vm_control{
    set_context,
    get_exit_info,
    get_context,
    req_inject,
    get_acpi_io,
    set_fat_addr,
    query_host_fat_info,
};

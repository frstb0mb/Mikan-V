#pragma once

#include <memory>
#include <stdint.h>
#include "ia32_compact.h"
#include "vm_syscall.hpp"


template<typename T, size_t max_size_>
class static_queue
{
private:
    uint64_t push_i = 0;
    uint64_t pop_i = 0;
    uint64_t num = 0;
    std::array<T, max_size_> array_ = {};
public:
    bool push(T data)
    {
        if (num == array_.size())
        {
            return false;
        }

        num++;
        array_[push_i] = data;
        push_i = (push_i + 1) % array_.size();
        return true;
    }

    T front()
    {
        if (num == 0)
        {
            return T();
        }

        return array_[pop_i];
    }

    T pop()
    {
        if (num == 0)
        {
            return T();
        }

        const auto ret = array_[pop_i];
        pop_i = (pop_i + 1) % array_.size();

        num--;
        return ret;
    }

    uint64_t size() const
    {
        return num;
    }

    uint64_t max_size() const
    {
        return array_.size();
    }

    bool empty() const
    {
        return !num;
    }
};

class VDevice
{
private:
int mouse_x = 200;
int mouse_y = 200;
uint8_t mouse_bt = 0;
class pic;
const std::unique_ptr<pic> vm_pic;

uint32_t timer_interval;

// vector and data
static_queue<std::pair<device_event, uint8_t>, 32> intr_list;
uint8_t pendings[static_cast<uint8_t>(device_event::event_num)]={};

public:
    VDevice();
    ~VDevice();
    bool io(vmx_exit_qualification_io_inst io_info, context &ctx);
    bool mmio(uint64_t guest_addr, uint64_t host_addr, context &ctx);
    void request_inject(const intr_data &data);
    int16_t process_inject(context &ctx);
    void check_device();
};

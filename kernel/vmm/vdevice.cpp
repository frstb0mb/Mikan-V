#include <array>
#include "acpi.hpp"
#include "logger.hpp"
#include "vdevice.hpp"

// The focus is on functionality rather than accurate emulation

constexpr uint8_t convUSBToPs2[256] = {
    0, 0, 0, 0, 30, 48, 46, 32,
    18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20,
    22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11,
    28, 0, 14, 15, 57, 12, 13, 26,
    27, 43, 0, 39, 40, 41, 52, 53,
    54, 0, 0, 59, 60, 0, 0, 0,        // F3 => F2 convert
    72, 73, 76, 77, 78, 0, 71, 72,
    81, 82, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 74, 75, 76, 70, 71, 74, 64,
    65, 66, 79, 80, 0, 0, 0, 0,
};

class pic_8259A
{
private:
    bool init           = false;
    bool use_icw4       = false;
    uint8_t icw_phase   = 0;
    uint8_t vector      = 0;
    bool irqs[8]        = {};
    uint8_t irr         = 0;
    uint8_t isr         = 0;
    uint8_t &read_0x20  = irr;
    const bool master;

public:
    pic_8259A() = delete;
    pic_8259A(bool is_master) : master(is_master)
    {
    }

    bool check_irr(uint8_t num)
    {
        if (num>=8)
        {
            return false;
        }

        if (!init && irqs[num])
        {
            const bool irr_stat = !!(irr & (1<<num));
            const bool isr_stat = !!(isr & (1<<num));
            if (!irr_stat && !isr_stat)
            {
                return true;
            }
        }

        return false;
    }

    void set_irr(uint8_t num)
    {
        if (num>=8)
        {
            return;
        }

        irr |= 1<<num;
        return;
    }

    void clear_irr(uint8_t num)
    {
        if (num>=8)
        {
            return;
        }

        irr ^= 1<<num;
    }

    bool check_isr(uint8_t num)
    {
        if (isr != 0)
        {
            return false;
        }

        if ((irr & (1 << num)) && irqs[num])
        {
            return true;
        }

        return false;
    }

    uint8_t set_isr(uint8_t num)
    {
        isr |= 1<<num;

        return vector + num;
    }

    uint8_t cmd_in()
    {
        // IRR or ISR
        return 0;
    }

    bool cmd_out(uint8_t val)
    {
        const uint8_t switch_bit = (val>>3) & 3;
        switch (switch_bit)
        {
            case 0: // OCW2
                // 011 == specified mode
                // EOI
                if (val&0x20)
                {
                    for (int i = 0; i < 8; i++)
                    {
                        if ((isr >> i) & 1)
                        {
                            isr ^= 1 << i;
                            break;
                        }
                    }

                    return true;
                }
                break;
            case 1: // OCW3
            {
                // change irr isr
                switch(val & 3)
                {
                    case 2:
                        read_0x20 = irr;
                    case 3:
                        read_0x20 = isr;
                        break;
                    default:
                        break;
                }
            }
            case 2: // ICW1
                if (val & 0x10)
                {
                    init = true;
                }
                if (val & 0x1)
                {
                    use_icw4 = true;
                }
                break;
            default:
                break;
        }

        return true;
    }

    uint8_t data_in()
    {
        // IMR
        return 0;
    }

    bool data_out(uint8_t val)
    {
        if (init)
        {
            switch (icw_phase)
            {
            case 0: // ICW2
                vector = val;
                icw_phase++;
                break;
            case 1: // ICW3
                if (master && val != 4)
                {
                    return false;
                }
                else if (!master && val != 2)
                {
                    return false;
                }

                icw_phase++;
                if (!use_icw4)
                {
                    icw_phase = 0;
                    init = false;
                }
                break;
            case 2: // ICW4
                icw_phase = 0;
                use_icw4 = false;
                init = false;
                break;
            default:
                return false;
            }
            return true;
        }

        for (int i = 0; i < 8; i++)
        {
            irqs[i] = !((val >> i) & 1);
        }

        return true;
    }
};

class ps2_controller
{
    bool enable = false;
    uint8_t next_read = 0;
    uint8_t next_write = 0;
    uint8_t ram[0x20] = {};
    bool enable_report_mouse = false;
public:
    ps2_controller()
    {
        ram[0] = 0x47;
    }

    bool can_report_mouse()
    {
        return enable_report_mouse;
    }

    void input_device(uint8_t val)
    {
        next_read = val;
    }

    uint8_t data_in()
    {
        return next_read;
    }

    bool data_out(uint8_t val)
    {
        if (next_write < 0x20)
        {
            ram[next_write] = val;
        }
        else
        {
            switch (val)
            {
                case 0xF4:
                    enable_report_mouse = true;
                    break;
                case 0xF5:
                    enable_report_mouse = false;
                    break;
                case 0xF6:
                    // set defaults
                    break;
                default:
                    return false;
            }
        }
        return true;
    }

    uint8_t cmd_in()
    {
        return 1;
    }

    bool cmd_out(uint8_t val)
    {
        switch (val)
        {
        case 0x20: // omit
        case 0x3F:
            next_read = ram[val&0x1f];
            break;
        case 0x60: // omit
        case 0x7F:
            next_write = val&0x1f;
            break;
        case 0xA8:
            enable = true;
            break;
        case 0xD4:
            next_write = 0x20;
            break;
        default:
            return false;
        }

        return true;
    }
};

// only timer
class LocalAPIC {
    enum class LVT_Mode{
        OneShot,
        Periodic,
        TSC_Deadline,
    };
private:
uint32_t initial_count = 0;
uint32_t current_count = 0;
//uint32_t divide_config; // ignore
uint8_t timer_vector = 0;
bool mask = false;
LVT_Mode mode;

public:
bool timer_irr; // we only use timer(Actually, need 256bit)
bool timer_isr;
    // 0xfee00320
    void set_lvt(uint32_t val)
    {
        timer_vector = val & 0xFF;
        mask = !!((val >> 16) & 1);
        mode = (LVT_Mode)((val >> 17) & 3);
    }

    // 0xfee00380
    void set_initial_count(uint32_t val)
    {
        initial_count = val;
        current_count = val;
    }

    uint32_t read_current_count()
    {
        return current_count;
    }

    bool countdown(uint32_t count)
    {
        if (initial_count == 0)
        {
            return false;
        }

        if (current_count < count)
        {
            current_count = 0;
        }
        else
        {
            current_count-=count;
        }

        if (current_count > 0)
        {
            return false;
        }

        switch(mode)
        {
            case LVT_Mode::OneShot:
            initial_count = 0;
            break;
            case LVT_Mode::Periodic:
            current_count = initial_count;
            break;
            case LVT_Mode::TSC_Deadline: // unsupported
            return false;
        }

        if (!mask && !timer_irr)
        {
            return true;
        }

        return false;
    }

    uint8_t set_isr()
    {
        if (!timer_isr)
        {
            timer_isr = true;
            return timer_vector;
        }

        return 0;
    }

    void eoi()
    {
        timer_isr = false;
    }
};

class VDevice::pic
{
private:
    pic_8259A master{true};
    pic_8259A slave{false};
    ps2_controller ps2 = {};
    LocalAPIC lapic = {};

public:
    bool can_intr(device_event event)
    {
        if (event == device_event::mouse)
        {
            if (master.check_irr(2) && slave.check_irr(4))
            {
                master.set_irr(2);
                slave.set_irr(4);
                return true;
            }
        }
        else if (event == device_event::keyboard)
        {
            if (master.check_irr(1))
            {
                master.set_irr(1);
                return true;
            }
        }
        else if (event == device_event::timer)
        {
            if (!lapic.timer_irr)
            {
                lapic.timer_irr = true;
                return true;
            }
        }
        return false;
    }

    uint8_t check_interrupt(device_event event)
    {
        if (event == device_event::mouse)
        {
            if (ps2.can_report_mouse())
            {
                const auto ret1 = master.check_isr(2);
                const auto ret2 = slave.check_isr(4);
                if (ret1 && ret2)
                {
                    return ret2;
                }
                if (!ret1 && !ret2)
                {
                    return 1;
                }
                if (!ret1)
                {
                    return 2;
                }
                if (!ret2)
                {
                    return 3;
                }
            }

            return 0;
        }
        return 0;
    }

    void clear_interrupt(device_event event)
    {
        if (event == device_event::mouse)
        {
            master.clear_irr(2);
            slave.clear_irr(4);
        }
        else if (event == device_event::keyboard)
        {
            master.clear_irr(1);
        }
        else if (event == device_event::timer)
        {
            lapic.timer_irr = false;
        }
    }

    uint8_t do_interrupt(device_event event)
    {
        if (event == device_event::mouse)
        {
            if (master.check_isr(2) && slave.check_isr(4) && ps2.can_report_mouse())
            {
                master.set_isr(2);
                return slave.set_isr(4);
            }
            return 0;
        }
        else if (event == device_event::keyboard)
        {
            if (master.check_isr(1))
            {
                return master.set_isr(1);
            }
            return 0;
        }
        else if (event == device_event::timer)
        {
            return lapic.set_isr();
        }
        return 0;
    }

    void input_ps2(uint8_t val)
    {
        ps2.input_device(val);
    }
    
    // true: complete emulation
    // false : no pic or emulation is failed
    bool emulation_in(uint16_t addr, uint8_t val, uint8_t &data)
    {
        switch (addr)
        {
            case 0x20:
                data = master.cmd_in();
                break;
            case 0x21:
                data = master.data_in();
                break;
            case 0x60: // PS/2 Data Port
                data = ps2.data_in();
                break;
            case 0x64: // PS/2 cmd or stat
                data = ps2.cmd_in();
                break;
            case 0xA0:
                data = slave.cmd_in();
                break;
            case 0xA1:
                data = slave.data_in();
                break;
            default:
                return false;
            }

        return true;
    }

    bool emulation_out(uint16_t addr, uint8_t val)
    {
        switch (addr)
        {
            case 0x20:
                return master.cmd_out(val);
            case 0x21:
                return master.data_out(val);
            case 0x60: // PS/2 Data Port
                return ps2.data_out(val);
            case 0x64: // PS/2 cmd or stat
                return ps2.cmd_out(val);
            case 0xA0:
                return slave.cmd_out(val);
            case 0xA1:
                return slave.data_out(val);
            default:
                return false;
        }
    }

    bool mmio_read(uint32_t addr, uint32_t &val)
    {
        switch (addr)
        {
            case 0xfee000b0:
            case 0xfee00320:
            case 0xfee00380:
                return false;
            case 0xfee00390:
                val = lapic.read_current_count();
                break;
            case 0xfee003e0:
                return false;
            default:
                return false;
        }
        return true;
    }

    bool mmio_write(uint32_t addr, uint32_t val)
    {
        switch (addr)
        {
            case 0xfee000b0:
                lapic.eoi();
                break;
            case 0xfee00320:
                lapic.set_lvt(val);
                break;
            case 0xfee00380:
                lapic.set_initial_count(val);
                break;
            case 0xfee00390:
                // current_count
                break;
            case 0xfee003e0:
                // divide_config
                break;
            default:
                return false;
        }
        return true;
    }

    bool check(uint32_t count)
    {
        return lapic.countdown(count);
    }
};

VDevice::VDevice() : vm_pic(std::make_unique<VDevice::pic>())
{
    timer_interval  = *reinterpret_cast<uint32_t*>(0xfee00380);
}

VDevice::~VDevice() = default;

bool VDevice::io(vmx_exit_qualification_io_inst io_info, context &ctx)
{
    if (io_info.string_instruction == VMX_EXIT_QUALIFICATION_IS_STRING_STRING ||
        io_info.operand_encoding == VMX_EXIT_QUALIFICATION_ENCODING_IMM ||
        io_info.rep_prefixed == VMX_EXIT_QUALIFICATION_IS_REP_REP)
    {
        // not supported
        return false;
    }

    uint16_t    addr    = ctx.rdx & 0xFFFF;
    uint8_t     val     = ctx.rax & 0xFF;

    if (io_info.direction_of_access)
    {
        uint8_t data = 0;
        if (vm_pic->emulation_in(addr, val, data))
        {
            ctx.rax = (ctx.rax & ~(0xFFul)) | data;

            return true;
        }
    }
    else
    {
        if (vm_pic->emulation_out(addr, val))
        {
            return true;
        }
    }

    // emulate other device
    switch (addr)
    {
    default:
        Log(kError, "[io] unknown IO access\n");
        return false;
    }
}

bool VDevice::mmio(uint64_t guest_addr, uint64_t host_addr, context &ctx)
{
    if (guest_addr < APIC_BASE || guest_addr >= APIC_BASE + 0x1000)
    {
        Log(kError, "[mmio] Not APIC Addr\n");
        return false;
    }

    auto op = *(uint8_t*)host_addr;
    auto modrm = *(uint8_t*)(host_addr + 1);
    uint32_t val = 0;

    if (op == 0xC7)
    {
        switch (modrm&0xC0)
        {
        case 0:
            val = *(uint32_t*)(host_addr + 1 + 1); // op+modrm
            break;
        case 0x40:
            val = *(uint32_t*)(host_addr + 1 + 1 + 1); // op+modrm+disp8
            break;
        case 0x80:
            val = *(uint32_t*)(host_addr + 1 + 1 + 4); // op+modrm+disp32 
            break;
        default:
            Log(kError, "[mmio] This modr/m is not supported(write access) %x\n", modrm);
            return false;
        }

        return vm_pic->mmio_write(guest_addr, val);
    }
    else if (op == 0x89)
    {
        val = *(uint64_t*)(&ctx.rax + ((modrm & 0x38) >> 3));
        return vm_pic->mmio_write(guest_addr, val);
    }
    else if (op == 0x8b)
    {
        if ((modrm & 0x38) == 0)
        {
            // support only rax
            uint32_t val = 0;
            if (vm_pic->mmio_read(guest_addr, val))
            {
                ctx.rax = val;
                return true;
            }
            Log(kError, "[mmio] Read is failed\n");
            return false;
        }

        Log(kError, "[mmio] This machine code is not supported(read access)\n");
        return false;
    }
    else
    {
        Log(kError, "[mmio] This machine code is not supported(unknown)\n");
    }

    return false;
}

uint64_t mouse_req = 0;

void VDevice::request_inject(const intr_data &intr_data)
{
    if (intr_list.max_size() < intr_list.size() + 3)
    {
        Log(kError,"Intr is full\n");
        return;
    }

    const auto event = intr_data.event;
    const auto &data = intr_data.data;
    if (event == device_event::mouse)
    {
        if (data.mouse.reset)
        {
            mouse_bt = 0;
            return;
        }
    }

    if (!vm_pic->can_intr(event))
    {
        return;
    }

    switch(event)
    {
        case device_event::mouse:
        {
            uint8_t flag = 8;
            const int32_t delta_x = data.mouse.x - mouse_x;
            const int32_t delta_y = data.mouse.y - mouse_y;
            mouse_x = data.mouse.x;
            mouse_y = data.mouse.y;
            if (data.mouse.change_bt)
            {
                if (data.mouse.press)
                {
                    mouse_bt |= 1<<data.mouse.button;
                }
                else
                {
                    mouse_bt &= ~(1<<data.mouse.button);
                }
            }
            flag |= mouse_bt;
            if (delta_x < 0)
            {
                flag |= 1<<4;
            }
            if (delta_y < 0)
            {
                flag |= 1<<5;
            }
            intr_list.push({event, flag});
            intr_list.push({event, delta_x});
            intr_list.push({event, -delta_y});
            pendings[static_cast<uint8_t>(event)] += 3;
            mouse_req++;
        }
        break;
        case device_event::keyboard:
        {
            constexpr int kLControlBitMask = 0b00000001u;
            constexpr int kLShiftBitMask   = 0b00000010u;
            constexpr int kLAltBitMask     = 0b00000100u;
            constexpr int kRShiftBitMask   = 0b00100000u;

            uint8_t mod_key = 0;
            switch(data.keyboard.modifier)
            {
                case kLControlBitMask:
                    mod_key = 0x1d;
                    break;
                case kLShiftBitMask  :
                    mod_key = 0x2a;
                    break;
                case kLAltBitMask    :
                    mod_key = 0x38;
                    break;
                case kRShiftBitMask  :
                    mod_key = 0x36;
                    break;
                default:
                    break;
            }
            if (!data.keyboard.press)
            {
                mod_key |= 0x80;
            }

            if (mod_key&0x7F)
            {
                intr_list.push({event, mod_key});
                pendings[static_cast<uint8_t>(event)]++;
            }

            auto keycode = convUSBToPs2[data.keyboard.keycode];
            if (!data.keyboard.press)
            {
                keycode |= 0x80;
            }

            intr_list.push({event, keycode});
            pendings[static_cast<uint8_t>(event)]++;
        }
        break;
        case device_event::timer:
        {
            intr_list.push({event, 0});
            pendings[static_cast<uint8_t>(event)]++;
        }
        default:
        break;
    }
}

int16_t VDevice::process_inject(context &ctx)
{
    if (intr_list.empty())
    {
        return 0;
    }

    if (!(ctx.rflags & (1<<9)))
    {
        return -1;
    }

    const auto intr = intr_list.front();
    auto vec = vm_pic->do_interrupt(intr.first);
    if (vec)
    {
        pendings[static_cast<uint8_t>(intr.first)]--;
        if (pendings[static_cast<uint8_t>(intr.first)] == 0)
        {
            vm_pic->clear_interrupt(intr.first);
        }

        switch (intr.first)
        {
            case device_event::mouse:
            case device_event::keyboard:
                vm_pic->input_ps2(intr.second);
                intr_list.pop();
                return vec;
            case device_event::timer:
                intr_list.pop();
                return vec;
            default:
                break;
        }
    }

    return 0;
}

void VDevice::check_device()
{
    if (vm_pic->check(timer_interval))
    {
        intr_data data = {};
        data.event = device_event::timer;
        request_inject(data);
    }
}

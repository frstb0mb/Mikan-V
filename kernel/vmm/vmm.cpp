#include <fat.hpp>
#include <paging.hpp>
#include <string.h>
#include <vector>
#include "acpi.hpp"
#include "asmfunc.h"
#include "fat.hpp"
#include "ia32_compact.h"
#include "logger.hpp"
#include "memory_manager.hpp"
#include "paging.hpp"
#include "uefi.hpp"
#include "vmm.hpp"

namespace {
    vm_context ctxs[vmm::MAX_VM] = {};
}

uint64_t InitVTXRegion(size_t allocsize)
{
    auto buffer = AllocAlignedMemory(allocsize);
    if (!buffer)
    {
        return 0;
    }

    memset(reinterpret_cast<uint64_t*>(buffer), 0, allocsize);

    ia32_vmx_basic_register basic = {};
    basic.flags = ReadMSR(IA32_VMX_BASIC);

    *reinterpret_cast<uint64_t*>(buffer) = basic.vmcs_revision_id;

    return buffer;
}

bool AllocMSRControlInfo(vm_context &ctx)
{
    auto host_region = AllocAlignedMemory(vmm::PAGE_SIZE);
    if (host_region == 0)
    {
        return false;
    }
    auto guest_region = AllocAlignedMemory(vmm::PAGE_SIZE);
    if (guest_region == 0)
    {
        FreePage(host_region, vmm::PAGE_SIZE);
        return false;
    }

    auto bitmap = AllocAlignedMemory(vmm::PAGE_SIZE);
    if (bitmap == 0)
    {
        FreePage(host_region, vmm::PAGE_SIZE);
        FreePage(guest_region, vmm::PAGE_SIZE);
        return false;
    }

    memset(reinterpret_cast<uint64_t*>(host_region), 0, vmm::PAGE_SIZE);
    memset(reinterpret_cast<uint64_t*>(guest_region), 0, vmm::PAGE_SIZE);
    memset(reinterpret_cast<uint64_t*>(bitmap), 0xFF, vmm::PAGE_SIZE);
    ctx.msr_host_region     = host_region;
    ctx.msr_guest_region    = guest_region;
    ctx.msr_bitmap_region   = bitmap;

    return true;
}


bool AllocVMXMem(vm_context &ctx)
{
    ctx.vmxon_region = InitVTXRegion(sizeof(vmxon));
    if (!ctx.vmxon_region) {
        return false;
    }

    ctx.vmcs_region = InitVTXRegion(sizeof(vmcs));
    if (!ctx.vmcs_region) {
        return false;
    }

    return false;
}

void AllocVMXMemory(vm_context &ctx)
{
    AllocVMXMem(ctx);
    AllocMSRControlInfo(ctx);
    ctx.vm_ept  = EPTInit();
    ctx.dev     = std::make_unique<VDevice>();
}

void FreeVMMem(vm_context &ctx)
{
    ctx.dev.release();
    EPTRelease(ctx.vm_ept);
    if (ctx.vmxon_region)
    {
        FreePage(ctx.vmxon_region, vmm::PAGE_SIZE);
    }

    if (ctx.vmcs_region)
    {
        FreePage(ctx.vmcs_region, vmm::PAGE_SIZE);
    }

    if (ctx.msr_host_region)
    {
        FreePage(ctx.msr_host_region, vmm::PAGE_SIZE);
    }

    if (ctx.msr_guest_region)
    {
        FreePage(ctx.msr_guest_region, vmm::PAGE_SIZE);
    }

    if (ctx.msr_bitmap_region)
    {
        FreePage(ctx.msr_bitmap_region, vmm::PAGE_SIZE);
    }

    ctx.vm_ept              = nullptr;
    ctx.vmxon_region        = 0;
    ctx.vmcs_region         = 0;
    ctx.msr_host_region     = 0;
    ctx.msr_guest_region    = 0;
    ctx.msr_bitmap_region   = 0;
}

uint8_t CreateVMInternal()
{
    for (uint8_t vm_id = 0; vm_id < vmm::MAX_VM; vm_id++)
    {
        if (!ctxs[vm_id].used)
        {
            ctxs[vm_id].used = true;
            AllocVMXMemory(ctxs[vm_id]);
            return vm_id;
        }
    }

    return vmm::MAX_VM;
}

void DestroyVMInternal(uint8_t vm_id)
{
    if (vm_id < vmm::MAX_VM)
    {
        FreeVMMem(ctxs[vm_id]);
        memset(&ctxs[vm_id].ctx,        0, sizeof(ctxs[vm_id].ctx));
        memset(&ctxs[vm_id].fat_addr,   0, sizeof(ctxs[vm_id].fat_addr));
        ctxs[vm_id].exit.clear();
        ctxs[vm_id].next_instr_len = 0;
        ctxs[vm_id].run  = false;
        ctxs[vm_id].used = false;
    }
}

bool SetVMFlags()
{
    if (!IsSupportVMX())
    {
        Log(kError, "[SetVMFlags] VMX is not supported\n");
        return false;
    }

    ia32_feature_control_register msr_ctr = {};
    msr_ctr.flags = ReadMSR(IA32_FEATURE_CONTROL);
    if (!msr_ctr.enable_vmx_outside_smx)
    {
        if (msr_ctr.lock_bit)
        {
            Log(kError, "[SetVMFlags] Already locked and cannot load\n");
            return false;
        }
        else
        {
            msr_ctr.lock_bit = 1;
            msr_ctr.enable_vmx_outside_smx = 1;
            WriteMSR(IA32_FEATURE_CONTROL, msr_ctr.flags);
        }
    }

    msr_ctr.flags = ReadMSR(IA32_FEATURE_CONTROL);
    if (!msr_ctr.lock_bit || !msr_ctr.enable_vmx_outside_smx)
    {
        Log(kError, "[SetVMFlags] write is failed\n");
        return false;
    }

    EnableVMX();

    return true;
}

bool SetMemoryInternal(uint8_t vm_id, uint64_t host_addr, uint64_t guest_addr, uint64_t mem_size, uint32_t protect)
{
    if (vm_id == vmm::MAX_VM)
    {
        return false;
    }

    auto &ctx = ctxs[vm_id];
    return EPTRegisterMem(*(ctx.vm_ept), host_addr, guest_addr, mem_size, protect);
}

bool SetMemoryInternal(vm_context &ctx, uint64_t host_addr, uint64_t guest_addr, uint64_t mem_size, uint32_t protect)
{
    return EPTRegisterMem(*(ctx.vm_ept), host_addr, guest_addr, mem_size, protect);
}

enum class SEGREGS
{
    ES = 0,
    CS,
    SS,
    DS,
    FS,
    GS,
    LDTR,
    TR
};

void SetVMCSHostState()
{
#define VMWRITE_HOSTSEGMENTSELECTOR(segment) vmx_vmwrite(VMCS_HOST_## segment ## _SEL, Get ## segment() & 0xF8)

    VMWRITE_HOSTSEGMENTSELECTOR(ES);
    VMWRITE_HOSTSEGMENTSELECTOR(CS);
    VMWRITE_HOSTSEGMENTSELECTOR(SS);
    VMWRITE_HOSTSEGMENTSELECTOR(DS);
    VMWRITE_HOSTSEGMENTSELECTOR(FS);
    VMWRITE_HOSTSEGMENTSELECTOR(GS);
    VMWRITE_HOSTSEGMENTSELECTOR(TR);

    segment_descriptor_register_64 desc = {};
    SaveGDT(&desc);

    segment_selector sel = {};
    sel.flags = GetTR();
    segment_descriptor_32 *seg_desc=  (segment_descriptor_32 *)(desc.base_address + (sel.index<<3));
    const auto tr_base = seg_desc->base_address_low | seg_desc->base_address_middle<<16 | seg_desc->base_address_high<<24 | *(uint64_t*)&seg_desc[1] << 32;
    vmx_vmwrite(VMCS_HOST_TR_BASE, tr_base);

    vmx_vmwrite(VMCS_HOST_GDTR_BASE, desc.base_address);
    SaveIDT(&desc);
    vmx_vmwrite(VMCS_HOST_IDTR_BASE, desc.base_address);

    vmx_vmwrite(VMCS_HOST_CR0, GetCR0());
    vmx_vmwrite(VMCS_HOST_CR3, GetCR3());
    vmx_vmwrite(VMCS_HOST_CR4, GetCR4());

    vmx_vmwrite(VMCS_HOST_RIP, (uint64_t)VMXRestoreState);

    vmx_vmwrite(VMCS_HOST_EFER, ReadMSR(IA32_EFER));
}

void FillGuestSelectorData(SEGREGS segment, uint16_t selector)
{
    const auto offset = static_cast<size_t>(segment) * 2;
    vmx_vmwrite(VMCS_GUEST_ES_BASE + offset, 0);
    vmx_vmwrite(VMCS_GUEST_ES_SEL + offset,           selector);

    if (segment == SEGREGS::TR || segment == SEGREGS::LDTR)
    {
        vmx_vmwrite(VMCS_GUEST_ES_LIMIT + offset,         0);
    }
    else
    {
        vmx_vmwrite(VMCS_GUEST_ES_LIMIT + offset,         0xFFFFFFFF);
    }

    vmx_segment_access_rights aw = {};
    aw.present = 1;
    if (segment == SEGREGS::CS)
    {
        aw.type             = 0xB;
        aw.descriptor_type  = 1;
        aw.granularity      = 1;
        aw.long_mode        = 1;
    }
    else if (segment == SEGREGS::TR)
    {
        aw.type             = 0xB;
        aw.descriptor_type  = 0;
        aw.granularity      = 0;
        aw.long_mode        = 0;
    }
    else if (segment == SEGREGS::LDTR)
    {
        aw.type             = 0x2;
        aw.descriptor_type  = 0;
        aw.granularity      = 0;
        aw.long_mode        = 0;
    }
    else
    {
        aw.type             = 0x3;
        aw.descriptor_type  = 1;
        aw.granularity      = 1;
        aw.long_mode        = 0;
        aw.default_big      = 1;
    }
    vmx_vmwrite(VMCS_GUEST_ES_ACCESS_RIGHTS + offset, aw.flags);
}

void SetDefaultGuestSegment()
{
    //FillGuestSelectorData(SEGREGS::CS, GetCS());
    FillGuestSelectorData(SEGREGS::CS, 8);
    FillGuestSelectorData(SEGREGS::ES, 0x00);
    FillGuestSelectorData(SEGREGS::SS, 0x00);
    FillGuestSelectorData(SEGREGS::DS, 0x00);
    FillGuestSelectorData(SEGREGS::FS, 0x00);
    FillGuestSelectorData(SEGREGS::GS, 0x00);
    FillGuestSelectorData(SEGREGS::LDTR,0x00);
    FillGuestSelectorData(SEGREGS::TR, 0x00);
}

void SetVMCSGuestState(const vm_context &ctx)
{
    SetDefaultGuestSegment();

    // default
    cr0 CR0 = {};
    CR0.flags = GetCR0() & 0xE007003F;
    CR0.paging_enable = 1;
    CR0.protection_enable = 1;
    vmx_vmwrite(VMCS_GUEST_CR0, CR0.flags);
    vmx_vmwrite(VMCS_GUEST_CR3, ctx.ctx.CR3);
    vmx_vmwrite(VMCS_GUEST_CR4, GetCR4());

    segment_descriptor_register_64 desc = {};
    vmx_vmwrite(VMCS_GUEST_GDTR_BASE, ctx.ctx.GDTR_BASE);
    vmx_vmwrite(VMCS_GUEST_GDTR_LIMIT, ctx.ctx.GDTR_LIMIT);
    SaveIDT(&desc);
    vmx_vmwrite(VMCS_GUEST_IDTR_BASE, 0);
    vmx_vmwrite(VMCS_GUEST_IDTR_LIMIT, 0);

    vmx_vmwrite(VMCS_GUEST_RSP, ctx.ctx.rsp);
    vmx_vmwrite(VMCS_GUEST_RIP, ctx.ctx.rip);
    vmx_vmwrite(VMCS_GUEST_RFLAGS, 0x2);

    vmx_vmwrite(VMCS_GUEST_EFER, 0x501); // LMA LME SCE

    vmx_vmwrite(VMCS_GUEST_VMCS_LINK_PTR, ~0ULL);
}

#define _countof(array) \
(sizeof(array)/sizeof(array[0]))

void MSRConfig(const vm_context &ctx, bool run)
{
    struct msr_entry
    {
        uint32_t index;
        uint32_t reserved;
        uint64_t data;
    };

    constexpr uint32_t AutoHandles[] =
    {
        IA32_STAR,
        IA32_LSTAR,
        IA32_FMASK,
    };

    // save host state
    auto host_region = (msr_entry *)ctx.msr_host_region;
    for (int i = 0; i < _countof(AutoHandles); i++)
    {
        const auto msr          = AutoHandles[i];
        host_region[i].index    = msr; 
        host_region[i].data     = ReadMSR(msr);
    }

    if (run)
    {
        return;
    }

    auto msr_bitmap     = (vmx_msr_bitmap *)ctx.msr_bitmap_region;
    auto guest_region   = (msr_entry *)ctx.msr_guest_region;
    for (int i = 0; i < _countof(AutoHandles); i++)
    {
        const auto msr      = AutoHandles[i];
        const auto offset   = msr & MSR_ID_LOW_MAX;
        const auto index    = offset/8;
        const uint8_t bit   = 1<<(offset%8);
        if (msr >= MSR_ID_HIGH_MIN)
        {
            msr_bitmap->rdmsr_high[index] ^= bit;
            msr_bitmap->wrmsr_high[index] ^= bit;
        }
        else
        {
            msr_bitmap->rdmsr_low[index] ^= bit;
            msr_bitmap->wrmsr_low[index] ^= bit;
        }
        guest_region[i].index = msr;
    }
    const auto offset = IA32_EFER & MSR_ID_LOW_MAX;
    msr_bitmap->rdmsr_high[offset/8] ^= 1<<(offset%8);
    msr_bitmap->wrmsr_high[offset/8] ^= 1<<(offset%8);

    vmx_vmwrite(VMCS_CTRL_MSR_BITMAP, ctx.msr_bitmap_region);

    constexpr auto msr_num = _countof(AutoHandles);
    vmx_vmwrite(VMCS_CTRL_EXIT_MSR_LOAD_COUNT, msr_num);
    vmx_vmwrite(VMCS_CTRL_ENTRY_MSR_LOAD_COUNT, msr_num);
    vmx_vmwrite(VMCS_CTRL_EXIT_MSR_STORE_COUNT, msr_num);
    vmx_vmwrite(VMCS_CTRL_VMEXIT_MSR_LOAD, ctx.msr_host_region);
    vmx_vmwrite(VMCS_CTRL_VMENTRY_MSR_LOAD, ctx.msr_guest_region);
    vmx_vmwrite(VMCS_CTRL_VMEXIT_MSR_STORE, ctx.msr_guest_region);
}

uint32_t MakeCtrlFlag(uint32_t Ctl, uint32_t Msr)
{
    union QWORD
    {
        struct
        {
            uint32_t low;
            uint32_t high;
        };
        uint64_t cont;
    };
    QWORD MsrValue = {};

    MsrValue.cont = ReadMSR(Msr);
    Ctl &= MsrValue.high;
    Ctl |= MsrValue.low;
    return Ctl;
}

void InitVMCS(vm_context &ctx)
{
    SetVMCSHostState();
    MSRConfig(ctx, ctx.run);
    if (ctx.run)
    {
        return;
    }

    SetVMCSGuestState(ctx);

    ia32_vmx_basic_register basic = {};
    basic.flags = ReadMSR(IA32_VMX_BASIC);

    ia32_vmx_procbased_ctls_register proc_ctl = {};
    proc_ctl.use_msr_bitmaps = 1;
    proc_ctl.activate_secondary_controls = 1;
    proc_ctl.unconditional_io_exiting = 1;
    vmx_vmwrite(VMCS_CTRL_PROC_EXEC, MakeCtrlFlag(proc_ctl.flags, basic.true_controls ? IA32_VMX_TRUE_PROCBASED_CTLS : IA32_VMX_PROCBASED_CTLS));

    ia32_vmx_procbased_ctls2_register proc_ctl2 = {};
    proc_ctl2.enable_ept = 1;
    vmx_vmwrite(VMCS_CTRL_PROC_EXEC2, MakeCtrlFlag(proc_ctl2.flags, IA32_VMX_PROCBASED_CTLS2));

    ia32_vmx_pinbased_ctls_register pin_ctl = {};
    pin_ctl.external_interrupt_exiting = 1;
    vmx_vmwrite(VMCS_CTRL_PIN_EXEC, MakeCtrlFlag(pin_ctl.flags, basic.true_controls ? IA32_VMX_TRUE_PINBASED_CTLS : IA32_VMX_PINBASED_CTLS));

    ia32_vmx_exit_ctls_register exit_ctl= {};
    exit_ctl.host_address_space_size = 1;
    exit_ctl.load_ia32_efer = 1;
    exit_ctl.save_ia32_efer = 1;
    vmx_vmwrite(VMCS_CTRL_PRIMARY_EXIT, MakeCtrlFlag(exit_ctl.flags, basic.true_controls ? IA32_VMX_TRUE_EXIT_CTLS : IA32_VMX_EXIT_CTLS));

    ia32_vmx_entry_ctls_register entry_ctl= {};
    entry_ctl.ia32e_mode_guest = 1;
    entry_ctl.load_ia32_efer = 1;
    vmx_vmwrite(VMCS_CTRL_ENTRY, MakeCtrlFlag(entry_ctl.flags, basic.true_controls ? IA32_VMX_TRUE_ENTRY_CTLS : IA32_VMX_ENTRY_CTLS));

    vmx_vmwrite(VMCS_CTRL_EPTP, ctx.vm_ept->EptPointer.flags);
}

bool Handler(vm_context &ctx)
{
    uint64_t exit_reason = 0, guest_rip = 0, rflags = 0;
    vmx_vmread(VMCS_EXIT_REASON, &exit_reason);
    vmx_vmread(VMCS_GUEST_RIP, &guest_rip);
    vmx_vmread(VMCS_GUEST_RFLAGS, &rflags);
    exit_reason &= 0xffff;

    ctx.exit.clear();
    ctx.exit.exit_reason = exit_reason;
    ctx.ctx.rip          = guest_rip;
    ctx.ctx.rflags       = rflags;

    switch (exit_reason)
    {
        case VMX_EXIT_REASON_EXT_INT:
        {
            // consume count if timer is interrupted
            const auto timer_vector = *reinterpret_cast<uint32_t*>(0xfee00320) & 0xFF;
            bool timer_interrupt = !!(*(uint32_t*)(0xFEE00200ul + (timer_vector/32)*0x10) & (1 << (timer_vector%32)));
            if (timer_interrupt)
            {
                ctx.dev->check_device();
            }

            break;
        }
        case VMX_EXIT_REASON_TRIPLE_FAULT:
            Log(kError, "[Handler] Triple Fault\n");
            break;
        case VMX_EXIT_REASON_INT_WINDOW:
        {
            ia32_vmx_procbased_ctls_register proc_ctl = {};
            vmx_vmread(VMCS_CTRL_PROC_EXEC, &proc_ctl.flags);
            proc_ctl.interrupt_window_exiting = 0;
            vmx_vmwrite(VMCS_CTRL_PROC_EXEC, proc_ctl.flags);
            return true;
        }
        case VMX_EXIT_REASON_VMCALL:
        {
            // For EFI Runtime Service
            vmx_vmread(VMCS_EXIT_INSTR_LENGTH, &ctx.next_instr_len);
            switch(ctx.ctx.rax)
            {
                case 1:
                {
                    EFI_TIME t = {};
                    const auto rt_ret = uefi_rt->GetTime(&t, nullptr);
                    ctx.ctx.rax = rt_ret;
                    auto host_result_addr = EPTQueryHostPage(*(ctx.vm_ept), ctx.ctx.rcx);
                    if (host_result_addr)
                    {
                        host_result_addr = host_result_addr * vmm::PAGE_SIZE + (ctx.ctx.rcx & 0xFFF);
                        memcpy((uint8_t*)host_result_addr, &t, sizeof(t));
                    }
                    
                    return true;
                }
                case 0xFFFF:
                default:
                Log(kError, "[Handler] Unsurpported or Shutdown VMCall\n");
                break;
            }
            return false;
        }
        case VMX_EXIT_REASON_IO_INSTR:
        {
            vmx_vmread(VMCS_EXIT_QUALIFICATION, &ctx.exit.qualification);
            vmx_vmread(VMCS_EXIT_INSTR_LENGTH, &ctx.next_instr_len);
            vmx_exit_qualification_io_inst io_info = {};
            io_info.flags = ctx.exit.qualification;
            if ((ctx.ctx.rdx & 0xFFFF) == acpi::fadt->pm_tmr_blk)
            {
                if (io_info.string_instruction == VMX_EXIT_QUALIFICATION_IS_STRING_STRING ||
                    io_info.operand_encoding == VMX_EXIT_QUALIFICATION_ENCODING_IMM ||
                    io_info.rep_prefixed == VMX_EXIT_QUALIFICATION_IS_REP_REP)
                {
                    break;
                }
                if (io_info.direction_of_access)
                {
                    ctx.ctx.rax = (ctx.ctx.rax & ~(0xFFFFFFFFul)) | IoIn32(acpi::fadt->pm_tmr_blk);
                    return true;
                }
            }
            else
            {
                io_info.flags = ctx.exit.qualification;
                if (ctx.dev->io(io_info, ctx.ctx))
                {
                    return true;
                }
            }
            break;
        }
        case VMX_EXIT_REASON_WRMSR:
        {
            uint64_t next_inst = 0;
            Log(kError, "[Handler] [%lx] WRMSR %lx %ld\n", ctx.ctx.rip, ctx.ctx.rcx, next_inst);
            break;
        }
        case VMX_EXIT_REASON_ERR_INVALID_GUEST_STATE:
            Log(kError, "[Handler] Guest state is invalid\n");
            break;
        case VMX_EXIT_REASON_EPT_VIOLATION:
        {
            uint64_t inv_addr = 0;
            vmx_vmread(VMCS_GUEST_PHYS_ADDR, &inv_addr);
            ctx.exit.inv_addr = inv_addr;
            vmx_vmread(VMCS_EXIT_INSTR_LENGTH, &ctx.next_instr_len);
            if (inv_addr >= ctx.fat_addr.guest_first_fat && inv_addr < ctx.fat_addr.guest_end)
            {
                ctx.next_instr_len = 0;
                if (!EPTQueryHostPage(*(ctx.vm_ept), inv_addr))
                {
                    const auto host_fat = (uint64_t)fat::boot_volume_image + inv_addr - ctx.fat_addr.guest_start;
                    SetMemoryInternal(ctx, host_fat & ~(0xFFFul), inv_addr & ~(0xFFFul), 0x1000, 0);
                    return true;
                }
            }
            else
            {
                auto host_rip = EPTQueryHostPage(*(ctx.vm_ept), ctx.ctx.rip);
                if (host_rip)
                {
                    host_rip = host_rip * vmm::PAGE_SIZE + (ctx.ctx.rip & 0xFFF);
                    return ctx.dev->mmio(inv_addr, host_rip, ctx.ctx);
                }
            }
            break;
        }
        case VMX_EXIT_REASON_EPT_MISCONFIG:
        {
            uint64_t inv_addr = 0;
            vmx_vmread(VMCS_GUEST_PHYS_ADDR, &inv_addr);
            Log(kError, "[Handler] EPT Misconfig %lx\n", inv_addr);
            EPTPageWalk(*(ctx.vm_ept), inv_addr);
            break;
        }
        default:
            Log(kError, "[Handler] unknown %ld\n", exit_reason);
            break;
    }

    return false;
}

void UpdateVMCS(vm_context &ctx)
{
    const auto vec = ctx.dev->process_inject(ctx.ctx);
    if (vec > 0)
    {
        vmentry_interrupt_info intr_info = {};
        intr_info.vector            = vec;
        intr_info.interruption_type = external_interrupt;
        intr_info.valid             = 1;
        vmx_vmwrite(VMCS_CTRL_ENTRY_INTERRUPTION_INFO, intr_info.flags);
    }
    if (vec < 0)
    {
        ia32_vmx_procbased_ctls_register proc_ctl = {};
        vmx_vmread(VMCS_CTRL_PROC_EXEC, &proc_ctl.flags);
        proc_ctl.interrupt_window_exiting = 1;
        vmx_vmwrite(VMCS_CTRL_PROC_EXEC, proc_ctl.flags);
    }

    if (ctx.next_instr_len)
    {
        vmx_vmwrite(VMCS_GUEST_RIP, ctx.ctx.rip + ctx.next_instr_len);
        ctx.next_instr_len = 0;
    }
}

bool StartVMInternal(uint8_t vm_id)
{
    if (vm_id == vmm::MAX_VM)
    {
        return false;
    }

    __asm__("cli");

    if (!SetVMFlags())
    {
        __asm__("sti");
        return false;
    }

    auto &ctx = ctxs[vm_id];

    int status = vmx_on(&ctx.vmxon_region);
    if (status)
    {
        Log(kError, "[StartVMInternal] Failed VMXON %d\n", status);
        __asm__("sti");
        return false;
    }

    bool resume = false;
    if (!ctx.run)
    {
        status = vmx_vmclear(&ctx.vmcs_region);
        if (status)
        {
            Log(kError, "[StartVMInternal] Failed VMCLEAR %d\n", status);
            goto END;
        }
    }

    status = vmx_vmptrld(&ctx.vmcs_region);
    if (status)
    {
        Log(kError, "[StartVMInternal] Failed VMPTRLD %d\n", status);
        goto END;
    }

    InitVMCS(ctx);
    ctx.run = true;

    do
    {
        UpdateVMCS(ctx);
        auto ret = vmlaunch(&(ctx.ctx.rax), resume);
        resume = true;
        if (ret)
        {
            uint64_t err = 0;
            vmx_vmread(VMCS_VM_INSTR_ERROR, &err);
            Log(kError, "[StartVMInternal] vmlaunch is failed %lx %ld\n",ret, err);
            break;
        }

    } while(Handler(ctx));

    vmx_vmclear(&ctx.vmcs_region);

END:
    status = vmx_off();
    __asm__("sti");
    if (status)
    {
        Log(kError, "[StartVMInternal] Failed VMXOFF %d\n", status);
        return false;
    }

    return true;
}

bool ControlVMInternal(uint8_t vm_id, vm_control control_id, void *buffer, uint64_t size)
{
    if (control_id == vm_control::get_acpi_io)
    {
        if (size < sizeof(uint32_t))
        {
            return false;
        }

        *(uint32_t*)(buffer) = acpi::fadt->pm_tmr_blk;
        return true;
    }

    if (vm_id == vmm::MAX_VM)
    {
        return false;
    }

    auto &ctx = ctxs[vm_id];

    switch (control_id)
    {
    case vm_control::set_context:
    {
        if (size < sizeof(context))
        {
            break;
        }

        memcpy(&ctx.ctx, buffer, sizeof(context));
        return true;
    }
    case vm_control::get_exit_info:
    {
        if (size < sizeof(exit_info))
        {
            break;
        }

        memcpy(buffer, &ctx.exit, sizeof(ctx.exit));
        return true;
    }
    case vm_control::get_context:
    {
        if (size < sizeof(context))
        {
            break;
        }

        memcpy(buffer, &ctx.ctx, sizeof(context));
        return true;
    }
    case vm_control::req_inject:
    {
        if (size < sizeof(intr_data))
        {
            return false;
        }

        auto intr = *(intr_data*)(buffer);
        ctx.dev->request_inject(intr);
        return true;
    }
    case vm_control::get_acpi_io:
    {
        // never reach here
        return true;
    }
    case vm_control::set_fat_addr:
    {
        if (size < sizeof(fat_addr))
        {
            break;
        }

        memcpy(&ctx.fat_addr, buffer, sizeof(ctx.fat_addr));
        return true;
    }
    case vm_control::query_host_fat_info:
    {
        if (size < sizeof(fat_info))
        {
            break;
        }

        fat_info fat    = {};
        fat.head_offset = (uint64_t)fat::boot_volume_image & 0xFFF;
        fat.fat_size    = fat::boot_volume_image->total_sectors_32 * fat::boot_volume_image->bytes_per_sector;
        memcpy(buffer, &fat, sizeof(fat));
        return true;
    }
    default:
        break;
    }
    
    return false;
}

#include <array>
#include <cstddef>
#include <memory>
#include <fcntl.h>
#include <string.h>
#include "../syscall.h"
#include "../../kernel/acpi.hpp"
#include "../../kernel/elf.hpp"
#include "../../kernel/error.hpp"
#include "../../kernel/frame_buffer_config.hpp"
#include "../../kernel/graphics.hpp"
#include "../../kernel/memory_map.hpp"
#include "../../kernel/uefi.hpp"
#include "../../kernel/vmm/ia32_compact.h"
#include "../../kernel/vmm/vm_syscall.hpp"
#include "vfat.hpp"

#define NEXT_ALIGN_ADDR(addr) (uint8_t*)((uint64_t)((addr) + 0xFFF) & ~(0xFFFul))
#define NEXT_ALIGN_VAL(addr) (uint64_t)((uint64_t)((addr) + 0xFFF) & ~(0xFFFul))

struct load_mod_info
{
  std::unique_ptr<uint8_t[]> mng_mod;
  uint8_t *align_addr;
  uint64_t base;
  uint64_t entry;
  uint64_t mod_size;
};

Elf64_Phdr *GetProgramHeader(Elf64_Ehdr *ehdr)
{
  return reinterpret_cast<Elf64_Phdr *>(
      reinterpret_cast<uintptr_t>(ehdr) + ehdr->e_phoff);
}

WithError<load_mod_info> CopyLoadSegmentsRVA(Elf64_Ehdr *ehdr)
{
  auto phdr = GetProgramHeader(ehdr);
  uint64_t base_addr = UINT64_MAX;
  uint64_t last_addr = 0;
  for (int i = 0; i < ehdr->e_phnum; ++i)
  {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    base_addr = std::min(base_addr, phdr[i].p_paddr);
    last_addr = std::max(last_addr, phdr[i].p_paddr + phdr[i].p_memsz);
  }

  const auto need_size = NEXT_ALIGN_VAL(last_addr - base_addr) + vmm::PAGE_SIZE;
  auto dst = std::make_unique<uint8_t[]>(need_size);
  const auto align_addr = NEXT_ALIGN_ADDR(dst.get());

  for (int i = 0; i < ehdr->e_phnum; ++i)
  {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    const auto src = reinterpret_cast<uint8_t *>(ehdr) + phdr[i].p_offset;
    const auto rva = reinterpret_cast<uint64_t>(phdr[i].p_vaddr - base_addr);
    memcpy(align_addr + rva, src, phdr[i].p_filesz);
    memset(align_addr + rva + phdr[i].p_filesz, 0, phdr[i].p_memsz - phdr[i].p_filesz);
  }

  return {{std::move(dst), align_addr, base_addr, ehdr->e_entry, need_size}, MAKE_ERROR(Error::kSuccess)};
}

WithError<load_mod_info> LoadELF(Elf64_Ehdr *ehdr)
{
  if (ehdr->e_type != ET_EXEC)
  {
    return {{}, MAKE_ERROR(Error::kInvalidFormat)};
  }

  return CopyLoadSegmentsRVA(ehdr);
}

std::tuple<int, char *, size_t> MapFile(const char *filepath)
{
  SyscallResult res = SyscallOpenFile(filepath, O_RDONLY);
  if (res.error)
  {
    fprintf(stderr, "%s: %s\n", strerror(res.error), filepath);
    exit(1);
  }

  const int fd = res.value;
  size_t filesize;
  res = SyscallMapFile(fd, &filesize, 0);
  if (res.error)
  {
    fprintf(stderr, "%s\n", strerror(res.error));
    exit(1);
  }

  return {fd, reinterpret_cast<char *>(res.value), filesize};
}

WithError<load_mod_info> LoadKernel(const char *path)
{
  const auto [fd, content, filesize] = MapFile(path);

  auto elf_header = reinterpret_cast<Elf64_Ehdr *>(content);
  return LoadELF(elf_header);
}

// resolution + frame
constexpr uint64_t buffer_width = 800;
constexpr uint64_t buffer_height = 400;
bool IsInside(int x, int y)
{
  return 4 <= x && x < 4 + buffer_width && 24 <= y && y < 24 + buffer_height;
}

bool CheckEvent(uint8_t vm_id, uint64_t layer_id)
{
  AppEvent events[1];
  auto [n, err] = SyscallReadEventNB(events, 1);
  if (err)
  {
    return true;
  }

  bool inject = false;

  switch (events->type)
  {
  case AppEvent::Type::kQuit:
    return false;
  case AppEvent::Type::kMouseMove:
    if (IsInside(events->arg.mouse_move.x, events->arg.mouse_move.y))
    {
      const auto [active, _] = SyscallGetActiveLayerID();
      if (active == layer_id)
      {
        intr_data data = {};
        data.event = device_event::mouse;
        data.data.mouse.x = events->arg.mouse_move.x;
        data.data.mouse.y = events->arg.mouse_move.y;
        SyscallControlVM(vm_id, vm_control::req_inject, &data, sizeof(data));
      }
    }

    return true;
  case AppEvent::Type::kMouseButton:
    if (IsInside(events->arg.mouse_button.x, events->arg.mouse_button.y))
    {
      const auto [active, _] = SyscallGetActiveLayerID();
      if (active == layer_id)
      {
        intr_data data = {};
        data.event = device_event::mouse;
        data.data.mouse.x = events->arg.mouse_button.x;
        data.data.mouse.y = events->arg.mouse_button.y;
        data.data.mouse.change_bt = true;
        data.data.mouse.press = !!events->arg.mouse_button.press;
        data.data.mouse.button = events->arg.mouse_button.button;
        SyscallControlVM(vm_id, vm_control::req_inject, &data, sizeof(data));
        inject = true;
      }
    }

    if (!inject)
    {
      intr_data data = {};
      data.data.mouse.reset = true;
      SyscallControlVM(vm_id, vm_control::req_inject, &data, sizeof(data));
    }

    return true;
  case AppEvent::Type::kKeyPush:
  {
    const auto [active, _] = SyscallGetActiveLayerID();
    if (active == layer_id)
    {
      intr_data data = {};
      data.event = device_event::keyboard;
      data.data.keyboard.keycode = events->arg.keypush.keycode;
      data.data.keyboard.modifier = events->arg.keypush.modifier;
      data.data.keyboard.press = !!events->arg.keypush.press;
      SyscallControlVM(vm_id, vm_control::req_inject, &data, sizeof(data));
    }

    return true;
  }
  default:
    return true;
  }

  return false;
}

bool handler(uint8_t vm_id)
{
  exit_info info = {};
  SyscallControlVM(vm_id, vm_control::get_exit_info, &info, sizeof(info));
  switch (info.exit_reason)
  {
  case VMX_EXIT_REASON_EXT_INT:
    return true;
  default:
    printf("unhandled exit %d\n", info.exit_reason);
    return false;
  }

  return true;
}

void InitializePageTable(uint64_t *pml4, uint64_t guest_pagetable)
{
  const auto pdp_table = &pml4[512];
  pml4[0] = (guest_pagetable + vmm::PAGE_SIZE) | 0x003;
  using pgdr = uint64_t[512];
  pgdr *page_directory = (pgdr *)&pdp_table[512];
  for (int i_pdpt = 0; i_pdpt < 512; ++i_pdpt)
  {
    pdp_table[i_pdpt] = (guest_pagetable + 0x2000 + vmm::PAGE_SIZE * i_pdpt) | 0x003;
    for (int i_pd = 0; i_pd < 512; ++i_pd)
    {
      page_directory[i_pdpt][i_pd] = i_pdpt * 0x40000000 + i_pd * 0x200000 | 0x083;
    }
  }
}

struct acpi_info
{
  acpi::RSDP acpi_table;
  acpi::XSDT xsdt;
  uint64_t entry;
  acpi::FADT fadt;
} __attribute__((packed));

void InitializeACPITable(acpi_info &acpi, uint64_t xsdt_addr)
{
  memcpy(acpi.acpi_table.signature, "RSD PTR ", 8);
  acpi.acpi_table.revision = 2;
  acpi.acpi_table.xsdt_address = xsdt_addr;
  acpi.acpi_table.checksum = 0xDF;
  acpi.acpi_table.extended_checksum = 0x6A;

  memcpy(acpi.xsdt.header.signature, "XSDT", 4);
  acpi.xsdt.header.length = sizeof(acpi::XSDT) + 1 * sizeof(uint64_t);
  acpi.xsdt.header.checksum = 0xCF;

  acpi.entry = (uint8_t *)&acpi.fadt - (uint8_t *)&acpi.xsdt + xsdt_addr;

  uint32_t timer_io=0;
  SyscallControlVM(0, vm_control::get_acpi_io, &timer_io, sizeof(timer_io));
  memcpy(acpi.fadt.header.signature, "FACP", 4);
  acpi.fadt.pm_tmr_blk = timer_io;
}

struct efi_info
{
  EFI_RUNTIME_SERVICES rt;
  uint8_t func[0x100];
};

void InitializeEFITable(efi_info &table, uint64_t addr)
{
  memset(table.func, 0xCC, sizeof(table.func));

  /**
   * mov rax, 0x1
   * vmcall
   * ret
   */
  constexpr uint8_t GetTimeWrapper[] = "\x48\xC7\xC0\x01\x00\x00\x00\x0F\x01\xC1\xC3";
  /**
   * mov rax, 0xFFFF
   * vmcall
   * ret
   */
  constexpr uint8_t ResetSystemWrapper[] = "\x48\xC7\xC0\xFF\xFF\x00\x00\x0F\x01\xC1\xC3";
  memcpy(table.func, GetTimeWrapper, sizeof(GetTimeWrapper));
  memcpy(table.func + sizeof(GetTimeWrapper), ResetSystemWrapper, sizeof(ResetSystemWrapper));

  table.rt.GetTime      = (EFI_GET_TIME)(addr + offsetof(efi_info, func));
  table.rt.ResetSystem  = (EFI_RESET_SYSTEM)((uint64_t)table.rt.GetTime + sizeof(GetTimeWrapper));
}

struct kernel_args
{
  FrameBufferConfig screen_config;
  MemoryMap mmap;
  MemoryDescriptor memory_desc;
  acpi_info acpi;
  efi_info efi;
  uint32_t frame_buffer[buffer_height][buffer_width];
  alignas(vmm::PAGE_SIZE) uint8_t heap_space[130 * 1024 * 1024];
};

void InitializeGuestEnv(kernel_args* dst, uint64_t guest_arg_addr)
{
  memset(dst, 0, sizeof(kernel_args));

  // screen config
  dst->screen_config.pixel_format           = PixelFormat::kPixelBGRResv8BitPerColor;
  dst->screen_config.horizontal_resolution  = buffer_width;
  dst->screen_config.vertical_resolution    = buffer_height;
  dst->screen_config.pixels_per_scan_line   = buffer_width;
  dst->screen_config.frame_buffer           = (uint8_t *)(guest_arg_addr + offsetof(kernel_args, frame_buffer));

  // memory map
  dst->mmap.buffer                  = (uint8_t *)(guest_arg_addr + offsetof(kernel_args, memory_desc));
  dst->mmap.map_size                = sizeof(MemoryDescriptor) * 1;
  dst->mmap.descriptor_size         = sizeof(MemoryDescriptor);
  dst->memory_desc.physical_start   = (uintptr_t)(guest_arg_addr + offsetof(kernel_args, heap_space));
  dst->memory_desc.number_of_pages  = sizeof(dst->heap_space) / vmm::PAGE_SIZE;
  dst->memory_desc.type             = static_cast<uint32_t>(MemoryType::kEfiConventionalMemory);

  InitializeACPITable(dst->acpi, guest_arg_addr + offsetof(kernel_args, acpi.xsdt));
  InitializeEFITable(dst->efi, guest_arg_addr + offsetof(kernel_args, efi));
}

int main(int argc, char **argv)
{
  auto [layer_id, err_openwin] = SyscallOpenWindow(buffer_width + 10, buffer_height + 30, 10, 10, "Mikan-V"); // add frame size
  if (err_openwin)
  {
    return err_openwin;
  }

  auto [kernel_info, err_load] = LoadKernel("kernel.elf");
  if (err_load)
  {
    printf("Error Load %d\n", err_load.Line());
    SyscallCloseWindow(layer_id);
    return 0;
  }

  auto ret = SyscallCreateVM();
  if (ret.error != 0)
  {
    printf("Failed to CreateVM\n");
    SyscallCloseWindow(layer_id);
    return 0;
  }
  do
  {
    auto mem_ret = SyscallSetMemory(ret.value, (uint64_t)kernel_info.align_addr, kernel_info.base, kernel_info.mod_size, 0);
    if (!mem_ret.value)
    {
      printf("Failed To SetMemory %d\n",__LINE__);
      break;
    }

    constexpr uint64_t guest_gdt            = 0x0000a000;
    constexpr uint64_t guest_stack_addr     = 0x0000B000;
    constexpr uint64_t guest_pagetable      = 0x00A00000;
    constexpr uint64_t guest_arg_addr       = 0x02000000;
    constexpr uint64_t guest_vfat_base_addr = 0x10000000;

    auto desctable = std::make_unique<uint8_t[]>(vmm::PAGE_SIZE*2);
    const auto desctable_align = (uint64_t*)NEXT_ALIGN_ADDR(desctable.get());
    desctable_align[0] = 0;
    desctable_align[1] = 0x00af9b000000ffff;
    mem_ret = SyscallSetMemory(ret.value, (uint64_t)desctable_align, guest_gdt, vmm::PAGE_SIZE, 0);
    if (!mem_ret.value)
    {
      printf("Failed To SetMemory %d\n",__LINE__);
      break;
    }

    auto page = std::make_unique<uint8_t[]>(vmm::PAGE_SIZE * vmm::PAGE_SIZE + vmm::PAGE_SIZE * 3);
    const auto page_align = (uint64_t*)NEXT_ALIGN_ADDR(page.get());
    InitializePageTable(page_align, guest_pagetable);
    mem_ret = SyscallSetMemory(ret.value, (uint64_t)page_align, guest_pagetable, vmm::PAGE_SIZE * vmm::PAGE_SIZE + vmm::PAGE_SIZE * 2, 0);
    if (!mem_ret.value)
    {
      printf("Failed To SetMemory %d\n",__LINE__);
      break;
    }

    auto guest_arg = std::make_unique<uint8_t[]>(sizeof(kernel_args) + vmm::PAGE_SIZE);
    const auto guest_arg_align = (kernel_args*)NEXT_ALIGN_ADDR(guest_arg.get());
    InitializeGuestEnv(guest_arg_align, guest_arg_addr);
    mem_ret = SyscallSetMemory(ret.value, (uint64_t)guest_arg_align, guest_arg_addr, NEXT_ALIGN_VAL(sizeof(kernel_args)), 0);
    if (!mem_ret.value)
    {
      printf("Failed To SetMemory %d\n",__LINE__);
      break;
    }

    auto guest_stack = std::make_unique<uint8_t[]>(vmm::PAGE_SIZE*2);
    mem_ret = SyscallSetMemory(ret.value, NEXT_ALIGN_VAL(guest_stack.get()), guest_stack_addr, vmm::PAGE_SIZE, 0);
    if (!mem_ret.value)
    {
      printf("Failed To SetMemory %d\n",__LINE__);
      break;
    }

    VFAT vfat(ret.value, guest_vfat_base_addr);

    context ctx = {};
    ctx.CR3         = guest_pagetable;
    ctx.GDTR_BASE   = guest_gdt;
    ctx.GDTR_LIMIT  = 0xF;
    ctx.rip         = kernel_info.entry;
    ctx.rsp         = guest_stack_addr + 0xF08;
    ctx.rdi         = guest_arg_addr;
    ctx.rsi         = guest_arg_addr + offsetof(kernel_args, mmap);
    ctx.rdx         = guest_arg_addr + offsetof(kernel_args, acpi.acpi_table);
    ctx.rcx         = vfat.get_guest_bpb_addr();
    ctx.r8          = guest_arg_addr + offsetof(kernel_args, efi);
    mem_ret = SyscallControlVM(ret.value, vm_control::set_context, &ctx, sizeof(context));
    if (!mem_ret.value)
    {
      printf("Failed To SetContext %d\n",__LINE__);
      break;
    }

    auto vm_ret = SyscallStartVM(ret.value);
    while (vm_ret.value)
    {
      exit_info info = {};
      SyscallControlVM(ret.value, vm_control::get_exit_info, &info, sizeof(info));
      if (!handler(ret.value))
      {
        break;
      }

      if (!CheckEvent(ret.value, layer_id))
      {
        break;
      }

      vm_ret = SyscallStartVM(ret.value);
      SyscallWinDrawFromBuffer(layer_id, guest_arg_align->frame_buffer[0], buffer_width * buffer_height);
    }
  } while(false);

  SyscallCloseWindow(layer_id);
  SyscallDestroyVM(ret.value);
}

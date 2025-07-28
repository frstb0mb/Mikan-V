#include <string.h>
#include "asmfunc.h"
#include "EPT.hpp"
#include "ia32_compact.h"
#include "logger.hpp"
#include "memory_manager.hpp"
#include "vm_syscall.hpp"

uint64_t AllocAlignedMemory(uint64_t mem_size) {
    auto frame = mem_size / kBytesPerFrame;
    auto [ stk, err ] = memory_manager->Allocate(frame);
    if (err)
    {
      return 0;
    }
    return reinterpret_cast<uint64_t>(stk.Frame());
}

void FreePage(uint64_t addr, uint64_t mem_size)
{
    memory_manager->Free(FrameID(addr / kBytesPerFrame), mem_size / kBytesPerFrame);
}

namespace
{
    void __stosq(uint64_t *Destination, uint64_t Data, size_t Count)
    {
        for (size_t i = 0; i < Count; i++)
        {
            memcpy(&Destination[i], &Data, sizeof(uint64_t));
        }
    }

    ept_page *EPTInitialize(ept_info &epti)
    {
        constexpr uint64_t PML3E_COUNT = 512;
        constexpr uint64_t PML2E_COUNT = 512;

        ept_page *page_table = reinterpret_cast<ept_page *>(AllocAlignedMemory((sizeof(ept_page) / vmm::PAGE_SIZE) * vmm::PAGE_SIZE));
        if (page_table == NULL)
        {
            return NULL;
        }

        memset(page_table, 0, sizeof(ept_page));
        page_table->pml4[0].page_frame_number    = (uint64_t)(&page_table->pml3[0]) / vmm::PAGE_SIZE;
        page_table->pml4[0].read_access          = 1;
        page_table->pml4[0].write_access         = 1;
        page_table->pml4[0].execute_access       = 1;

        epdpte pml3e = {};
        pml3e.flags           = 0;
        pml3e.read_access     = 1;
        pml3e.write_access    = 1;
        pml3e.execute_access  = 1;
        __stosq((uint64_t *)&page_table->pml3[0], pml3e.flags, PML3E_COUNT);

        for (uint64_t i = 0; i < PML3E_COUNT; i++)
        {
            page_table->pml3[i].page_frame_number = (uint64_t)(&page_table->pml2[i][0]) / vmm::PAGE_SIZE;
        }

        epde PML2EntryTemplate = {};
        __stosq((uint64_t *)&page_table->pml2[0], PML2EntryTemplate.flags, PML3E_COUNT * PML2E_COUNT);

        return page_table;
    }

    bool EPTAllInitialize(ept_info &epti)
    {
        ept_page *PageTable = EPTInitialize(epti);
        if (!PageTable)
        {
            return false;
        }
        epti.EptPageTable = PageTable;

        eptp EPTP = {};
        EPTP.memory_type                    = MEMORY_TYPE_WB;
        EPTP.enable_access_and_dirty_flags  = 0;
        EPTP.page_walk_length               = 3;
        EPTP.page_frame_number              = (uint64_t)(&PageTable->pml4) / vmm::PAGE_SIZE;

        epti.EptPointer = EPTP;

        return true;
    }
}

uint64_t vtop(uint64_t va)
{
    ept_page_split qaddr;
    qaddr.addr = va;
    auto pml4 = (pml4e_64 *)GetCR3();
    auto pdpt = (pdpte_64 *)(pml4[qaddr.pml4i].page_frame_number * vmm::PAGE_SIZE);
    auto pdt = (pde_64 *)(pdpt[qaddr.pml3i].page_frame_number * vmm::PAGE_SIZE);

    pde_2mb_64 *check_large = (pde_2mb_64 *)&pdt[qaddr.pml2i];
    if (check_large->large_page)
    {
        return check_large->page_frame_number * 0x200000 + (va & 0x1FF000);
    }

    auto pt = (pte_64 *)(pdt[qaddr.pml2i].page_frame_number * vmm::PAGE_SIZE);
    return pt[qaddr.pml1i].page_frame_number * vmm::PAGE_SIZE;
}

bool EPTRegisterMem(ept_info &epti, const uint64_t host_addr, const uint64_t guest_addr, const uint64_t mem_size, const uint32_t protect)
{
    Log(kDebug, "RegAddr  Host:%lx<==>Guest:%lx size:%lx\n", host_addr, guest_addr, mem_size);

    if (!epti.mapped.Allocate(guest_addr / vmm::PAGE_SIZE, mem_size / vmm::PAGE_SIZE))
    {
        Log(kError, "already allocated\n");
        return false;
    }

    for (uint64_t page_range = 0; page_range < mem_size; page_range += 0x1000)
    {
        ept_page_split dpage = {};
        dpage.addr = guest_addr + page_range;
        auto &pml2e = epti.EptPageTable->pml2[dpage.pml3i][dpage.pml2i];

        auto pte = (epte *)(pml2e.page_frame_number * vmm::PAGE_SIZE);
        if (!pml2e.write_access && !pml2e.read_access && !pml2e.execute_access)
        {
            pte = reinterpret_cast<epte *>(AllocAlignedMemory((sizeof(epte) * EPTE_ENTRY_COUNT / vmm::PAGE_SIZE) * vmm::PAGE_SIZE));
            if (!pte)
            {
                Log(kError, "INSUFFICIENT MEMORY\n");
                return false;
            }
            memset(pte, 0, sizeof(epte) * EPTE_ENTRY_COUNT);
            pml2e.write_access      = 1;
            pml2e.read_access       = 1;
            pml2e.execute_access    = 1;
            pml2e.page_frame_number = (uint64_t)(pte) / vmm::PAGE_SIZE;
        }

        auto &entry = pte[dpage.pml1i];

        if (protect)
        {
            entry.write_access      = 0;
            entry.execute_access    = 0;
        }
        else
        {
            entry.write_access      = 1;
            entry.execute_access    = 1;
        }
        entry.read_access = 1;
        entry.memory_type = MEMORY_TYPE_WB;

        entry.page_frame_number = vtop(host_addr + page_range) / vmm::PAGE_SIZE;
    }

    return true;
}

void EPTUnRegisterAll(ept_info &epti)
{
    for (int i = 0; i < EPDPTE_ENTRY_COUNT; i++)
    {
        for (int j = 0; j < EPDE_ENTRY_COUNT; j++)
        {
            auto &pml2e = epti.EptPageTable->pml2[i][j];
            if (pml2e.write_access && pml2e.read_access && pml2e.execute_access)
            {
                FreePage(pml2e.page_frame_number * vmm::PAGE_SIZE, sizeof(epte) * EPTE_ENTRY_COUNT);
            }
            pml2e.write_access      = 0;
            pml2e.read_access       = 0;
            pml2e.execute_access    = 0;
            pml2e.page_frame_number = 0;
        }
        epti.EptPageTable->pml3[i].write_access         = 0;
        epti.EptPageTable->pml3[i].read_access          = 0;
        epti.EptPageTable->pml3[i].execute_access       = 0;
        epti.EptPageTable->pml3[i].page_frame_number    = 0;
    }
    for (int i = 0; i < EPML4_ENTRY_COUNT; i++)
    {
        epti.EptPageTable->pml4[i].write_access         = 0;
        epti.EptPageTable->pml4[i].read_access          = 0;
        epti.EptPageTable->pml4[i].execute_access       = 0;
        epti.EptPageTable->pml4[i].page_frame_number    = 0;
    }

    epti.mapped.clear();
}

ept_info *EPTInit()
{
    const auto ptr = (ept_info *)AllocAlignedMemory(sizeof(ept_info));
    if (!ptr)
    {
        return nullptr;
    }
    memset(ptr, 0, sizeof(ept_info));
    EPTAllInitialize(*ptr);

    return ptr;
}

void EPTRelease(ept_info *ptr)
{
    if (ptr)
    {
        EPTUnRegisterAll(*ptr);
        if (ptr->EptPageTable)
        {
            FreePage((uint64_t)ptr->EptPageTable, sizeof(ept_page));
            ptr->EptPageTable = nullptr;
        }

        FreePage((uint64_t)ptr, sizeof(ept_info));
    }
}

void EPTPageWalk(ept_info &epti, uint64_t check)
{
    const auto check_pages = (ept_page *)epti.EptPageTable;
    ept_page_split cpage = {};
    cpage.addr = check;

    auto pml4e = check_pages->pml4[cpage.pml4i];
    Log(kError, "pml4 index:%i r:%d w:%d e:%d\n", cpage.pml4i, pml4e.read_access,
        pml4e.write_access, pml4e.execute_access);

    auto pml3e = check_pages->pml3[cpage.pml3i];
    Log(kError, "pml3 index:%i r:%d w:%d e:%d\n", cpage.pml3i, pml3e.read_access,
        pml3e.write_access, pml3e.execute_access);

    auto pml2e_t = check_pages->pml2[cpage.pml3i][cpage.pml2i];
    epde_2mb pml2e;
    pml2e.flags = pml2e_t.flags;
    Log(kError, "pml2 index:%i r:%d w:%d e:%d large:%d T:%d\n", cpage.pml2i, pml2e.read_access,
        pml2e.write_access, pml2e.execute_access, pml2e.large_page, pml2e.memory_type);

    if (!pml2e.large_page)
    {
        auto pte = (epte *)(pml2e_t.page_frame_number * vmm::PAGE_SIZE);
        if (!pte)
        {
            Log(kError, "Entry is not found\n");
            return;
        }

        auto &entry = pte[cpage.pml1i];

        Log(kError, "pml1 index:%d r:%d w:%d e:%d T:%d frame:%lx ptr:%p\n", cpage.pml1i, entry.read_access,
            entry.write_access, entry.execute_access, entry.memory_type, entry.page_frame_number, &pte[cpage.pml1i]);
    }
}

uint64_t EPTQueryHostPage(ept_info &epti, uint64_t check)
{
    ept_page_split cpage    = {};
    cpage.addr              = check;
    auto pml2e_t            = epti.EptPageTable->pml2[cpage.pml3i][cpage.pml2i];
    epde_2mb pml2e          = {};
    pml2e.flags             = pml2e_t.flags;

    if (pml2e.large_page)
    {
        return pml2e.page_frame_number;
    }
    else
    {
        auto pte = (epte *)(pml2e_t.page_frame_number * vmm::PAGE_SIZE);
        if (!pte)
        {
            return 0;
        }

        auto &entry = pte[cpage.pml1i];

        return entry.page_frame_number;
    }
}
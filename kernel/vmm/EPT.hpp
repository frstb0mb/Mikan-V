#pragma once

#include <array>
#include <stdint.h>
#include <utility>
#include "ia32_compact.h"
#include "memory_manager.hpp"

struct ept_page
{
    alignas(4096) std::array<epml4e, 512> pml4;
    alignas(4096) std::array<epdpte, 512> pml3;
    alignas(4096) std::array<std::array<epde, 512>, 512> pml2;
};

class BitmapForEPT : protected BitmapMemoryManager
{
private:
    std::array<MapLineType, kFrameCount / kBitsPerMapLine> alloc_map_ = {};
    FrameID range_begin_;
    FrameID range_end_;
    bool GetBit(FrameID frame) const
    {
        auto line_index = frame.ID() / kBitsPerMapLine;
        auto bit_index = frame.ID() % kBitsPerMapLine;

        return (alloc_map_[line_index] & (static_cast<MapLineType>(1) << bit_index)) != 0;
    }
    void SetBit(FrameID frame, bool allocated)
    {
        auto line_index = frame.ID() / kBitsPerMapLine;
        auto bit_index = frame.ID() % kBitsPerMapLine;

        if (allocated)
        {
            alloc_map_[line_index] |= (static_cast<MapLineType>(1) << bit_index);
        }
        else
        {
            alloc_map_[line_index] &= ~(static_cast<MapLineType>(1) << bit_index);
        }
    }

public:
    BitmapForEPT() : range_begin_{FrameID{0}}, range_end_{FrameID{kFrameCount}}
    {
    }
    bool Allocate(size_t frame, size_t num_frames)
    {
        const size_t start = frame;
        const size_t end = frame + num_frames;

        size_t first_line = start / kBitsPerMapLine;
        size_t last_line = (end - 1) / kBitsPerMapLine;

        // check
        for (auto i = start; i < std::min(end, (first_line + 1) * kBitsPerMapLine); ++i)
        {
            if (GetBit(FrameID{i}))
            {
                return false;
            }
        }
        for (auto line = first_line + 1; line < last_line; ++line)
        {
            if (alloc_map_[line] != 0)
            {
                return false;
            }
        }
        if (last_line > first_line)
        {
            for (size_t i = last_line * kBitsPerMapLine; i < end; ++i)
            {
                if (GetBit(FrameID{i}))
                {
                    return false;
                }
            }
        }

        // set
        for (auto i = start; i < std::min(end, (first_line + 1) * kBitsPerMapLine); ++i)
        {
            SetBit(FrameID{i}, true);
        }
        for (auto line = first_line + 1; line < last_line; ++line)
        {
            alloc_map_[line] = ~0ULL;
        }
        if (last_line > first_line)
        {
            for (size_t i = last_line * kBitsPerMapLine; i < end; ++i)
            {
                SetBit(FrameID{i}, true);
            }
        }

        return true;
    }
    void clear()
    {
        alloc_map_.fill(0);
    }
};

struct ept_info
{
    ept_page *EptPageTable;
    eptp EptPointer;
    BitmapForEPT mapped;
};

union ept_page_split
{
    struct
    {
        uint64_t offset : 12;
        uint64_t pml1i : 9;
        uint64_t pml2i : 9;
        uint64_t pml3i : 9;
        uint64_t pml4i : 9;
        uint64_t unused : 16;
    };

    uint64_t addr;
};

uint64_t AllocAlignedMemory(uint64_t mem_size);
void FreePage(uint64_t addr, uint64_t page_num);

ept_info *EPTInit();
void EPTRelease(ept_info *ptr);
bool EPTRegisterMem(ept_info &epti, const uint64_t host_addr, const uint64_t guest_addr, const uint64_t mem_size, const uint32_t protect);
void EPTUnRegisterAll(ept_info &epti);
void EPTPageWalk(ept_info &epti, uint64_t check);
uint64_t vtop(uint64_t va);
uint64_t EPTQueryHostPage(ept_info &epti, uint64_t check);
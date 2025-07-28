#include <stdint.h>
#include <memory>
#include "../../kernel/fat.hpp"

class VFAT {
private:
    fat::BPB *header_addr;
    uint64_t guest_bpb_addr;
    uint64_t first_fat_addr;
    uint64_t last_addr;
    std::unique_ptr<uint8_t[]> vfat_header_raw;
public:
    VFAT(uint8_t id, uint64_t reg_addr);
    uint64_t get_guest_bpb_addr();
};
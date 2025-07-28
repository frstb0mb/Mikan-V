#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include "../kernel/vmm/vm_syscall.hpp"

extern "C" {
#else
#include <stddef.h>
#include <stdint.h>
#endif

#include "../kernel/logger.hpp"
#include "../kernel/app_event.hpp"

struct SyscallResult {
  uint64_t value;
  int error;
};

struct SyscallResult SyscallLogString(enum LogLevel level, const char* message);
struct SyscallResult SyscallPutString(int fd, const char* s, size_t len);
void SyscallExit(int exit_code) __attribute__((noreturn));
struct SyscallResult SyscallOpenWindow(int w, int h, int x, int y, const char* title);

#define LAYER_NO_REDRAW (0x00000001ull << 32)
struct SyscallResult SyscallWinWriteString(
    uint64_t layer_id_flags, int x, int y, uint32_t color, const char* s);
struct SyscallResult SyscallWinFillRectangle(
    uint64_t layer_id_flags, int x, int y, int w, int h, uint32_t color);
struct SyscallResult SyscallGetCurrentTick();
struct SyscallResult SyscallWinRedraw(uint64_t layer_id_flags);
struct SyscallResult SyscallWinDrawLine(
    uint64_t layer_id_flags, int x0, int y0, int x1, int y1, uint32_t color);

struct SyscallResult SyscallCloseWindow(uint64_t layer_id_flags);
struct SyscallResult SyscallReadEvent(struct AppEvent* events, size_t len);
struct SyscallResult SyscallReadEventNB(struct AppEvent* events, size_t len);

#define TIMER_ONESHOT_REL 1
#define TIMER_ONESHOT_ABS 0
struct SyscallResult SyscallCreateTimer(
    unsigned int type, int timer_value, unsigned long timeout_ms);

struct SyscallResult SyscallOpenFile(const char* path, int flags);
struct SyscallResult SyscallReadFile(int fd, void* buf, size_t count);
struct SyscallResult SyscallDemandPages(size_t num_pages, int flags);
struct SyscallResult SyscallMapFile(int fd, size_t* file_size, int flags);
struct SyscallResult SyscallIsTerminal(int fd);

// CPP only
#ifdef __cplusplus
struct SyscallResult SyscallCreateVM();
struct SyscallResult SyscallDestroyVM(uint8_t vm_id);
struct SyscallResult SyscallStartVM(uint8_t vm_id);
struct SyscallResult SyscallSetMemory(uint8_t vm_id, uint64_t host_addr, uint64_t guest_addr, uint64_t mem_size, uint32_t protect);
struct SyscallResult SyscallControlVM(uint8_t vm_id, vm_control control_id, void *buffer, uint64_t size);
#endif

struct SyscallResult SyscallWinDrawFromBuffer(uint64_t layer_id_flags, uint32_t *buffer, uint64_t size);
struct SyscallResult SyscallGetActiveLayerID();

#ifdef __cplusplus
} // extern "C"
#endif

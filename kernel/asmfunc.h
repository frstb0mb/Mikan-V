#pragma once

#include <stdint.h>

extern "C" {
  void IoOut8(uint16_t addr, uint8_t data);
  uint8_t IoIn8(uint16_t addr);
  void IoOut32(uint16_t addr, uint32_t data);
  uint32_t IoIn32(uint16_t addr);
  uint16_t GetCS();
  uint16_t GetDS();
  uint16_t GetES();
  uint16_t GetFS();
  uint16_t GetGS();
  uint16_t GetSS();
  uint16_t GetTR();
  uint64_t GetLDTR();
  void LoadIDT(uint16_t limit, uint64_t offset);
  void *SaveIDT(void *addr);
  void LoadGDT(uint16_t limit, uint64_t offset);
  void *SaveGDT(void *addr);
  void SetCSSS(uint16_t cs, uint16_t ss);
  void SetDSAll(uint16_t value);
  uint64_t GetCR0();
  void SetCR0(uint64_t value);
  uint64_t GetCR2();
  void SetCR3(uint64_t value);
  uint64_t GetCR3();
  uint64_t GetCR4();
  uint64_t GetRFLAGS();
  void SwitchContext(void* next_ctx, void* current_ctx);
  void RestoreContext(void* ctx);
  int CallApp(int argc, char** argv, uint16_t ss, uint64_t rip, uint64_t rsp, uint64_t* os_stack_ptr);
  void IntHandlerLAPICTimer();
  void LoadTR(uint16_t sel);
  void WriteMSR(uint32_t msr, uint64_t value);
  void SyscallEntry(void);
  void ExitApp(uint64_t rsp, int32_t ret_val);
  void InvalidateTLB(uint64_t addr);
  uint64_t get_rsp();
  uint64_t ReadMSR(uint64_t msr);
  uint64_t EnableVMX();
  uint64_t vmx_on(uint64_t *vmxon_region);
  uint64_t vmx_off();
  uint64_t vmx_vmclear(uint64_t *vmcs_region);
  uint64_t vmx_vmptrld(uint64_t *vmcs_region);
  uint64_t vmx_vmwrite(uint64_t field, uint64_t val);
  uint64_t  vmx_vmread(uint64_t field, uint64_t *val);
  bool IsSupportVMX();
  uint64_t vmlaunch(void*, bool);
  void VMXRestoreState();
}

/**
 * @file interrupt.cpp
 *
 * 割り込み用のプログラムを集めたファイル．
 */

#include "interrupt.hpp"

#include <csignal>

#include "asmfunc.h"
#include "segment.hpp"
#include "timer.hpp"
#include "task.hpp"
#include "graphics.hpp"
#include "font.hpp"
#include "logger.hpp"
#include "usb/classdriver/mouse.hpp"
#include "keyboard.hpp"

std::array<InterruptDescriptor, 256> idt;

void SetIDTEntry(InterruptDescriptor& desc,
                 InterruptDescriptorAttribute attr,
                 uint64_t offset,
                 uint16_t segment_selector) {
  desc.attr = attr;
  desc.offset_low = offset & 0xffffu;
  desc.offset_middle = (offset >> 16) & 0xffffu;
  desc.offset_high = offset >> 32;
  desc.segment_selector = segment_selector;
}

void NotifyEndOfInterrupt() {
  volatile auto end_of_interrupt = reinterpret_cast<uint32_t*>(0xfee000b0);
  *end_of_interrupt = 0;
}

namespace {
  __attribute__((interrupt))
  void IntHandlerXHCI(InterruptFrame* frame) {
    task_manager->SendMessage(1, Message{Message::kInterruptXHCI});
    NotifyEndOfInterrupt();
  }

  void PrintHex(uint64_t value, int width, Vector2D<int> pos) {
    for (int i = 0; i < width; ++i) {
      int x = (value >> 4 * (width - i - 1)) & 0xfu;
      if (x >= 10) {
        x += 'a' - 10;
      } else {
        x += '0';
      }
      WriteAscii(*screen_writer, pos + Vector2D<int>{8 * i, 0}, x, {0, 0, 0});
    }
  }

  void PrintFrame(InterruptFrame* frame, const char* exp_name) {
    WriteString(*screen_writer, {500, 16*0}, exp_name, {0, 0, 0});
    WriteString(*screen_writer, {500, 16*1}, "CS:RIP", {0, 0, 0});
    PrintHex(frame->cs, 4, {500 + 8*7, 16*1});
    PrintHex(frame->rip, 16, {500 + 8*12, 16*1});
    WriteString(*screen_writer, {500, 16*2}, "RFLAGS", {0, 0, 0});
    PrintHex(frame->rflags, 16, {500 + 8*7, 16*2});
    WriteString(*screen_writer, {500, 16*3}, "SS:RSP", {0, 0, 0});
    PrintHex(frame->ss, 4, {500 + 8*7, 16*3});
    PrintHex(frame->rsp, 16, {500 + 8*12, 16*3});
  }

  void KillApp(InterruptFrame* frame) {
    const auto cpl = frame->cs & 0x3;
    if (cpl != 3) {
      return;
    }

    auto& task = task_manager->CurrentTask();
    __asm__("sti");
    ExitApp(task.OSStackPointer(), 128 + SIGSEGV);
  }

  __attribute__((interrupt))
  void IntHandlerPF(InterruptFrame* frame, uint64_t error_code) {
    uint64_t cr2 = GetCR2();
    if (auto err = HandlePageFault(error_code, cr2); !err) {
      return;
    }
    KillApp(frame);
    PrintFrame(frame, "#PF");
    WriteString(*screen_writer, {500, 16*4}, "ERR", {0, 0, 0});
    PrintHex(error_code, 16, {500 + 8*4, 16*4});
    while (true) __asm__("hlt");
  }

#define FaultHandlerWithError(fault_name) \
  __attribute__((interrupt)) \
  void IntHandler ## fault_name (InterruptFrame* frame, uint64_t error_code) { \
    KillApp(frame); \
    PrintFrame(frame, "#" #fault_name); \
    WriteString(*screen_writer, {500, 16*4}, "ERR", {0, 0, 0}); \
    PrintHex(error_code, 16, {500 + 8*4, 16*4}); \
    while (true) __asm__("hlt"); \
  }

#define FaultHandlerNoError(fault_name) \
  __attribute__((interrupt)) \
  void IntHandler ## fault_name (InterruptFrame* frame) { \
    KillApp(frame); \
    PrintFrame(frame, "#" #fault_name); \
    while (true) __asm__("hlt"); \
  }

  FaultHandlerNoError(DE)
  FaultHandlerNoError(DB)
  FaultHandlerNoError(BP)
  FaultHandlerNoError(OF)
  FaultHandlerNoError(BR)
  FaultHandlerNoError(UD)
  FaultHandlerNoError(NM)
  FaultHandlerWithError(DF)
  FaultHandlerWithError(TS)
  FaultHandlerWithError(NP)
  FaultHandlerWithError(SS)
  FaultHandlerWithError(GP)
  // FaultHandlerWithError(PF)
  FaultHandlerNoError(MF)
  FaultHandlerWithError(AC)
  FaultHandlerNoError(MC)
  FaultHandlerNoError(XM)
  FaultHandlerNoError(VE)

  __attribute__((interrupt))
  void IntHandlerKeyBoard(InterruptFrame* frame)
  {
    auto keycode = IoIn8(0x60);
    SendPS2Key(keycode);
    IoOut8(0x20, 0x20);
  }

  uint8_t mouse_index = 0;
  uint8_t mouse_code[3] = {};

  // ref: https://github.com/29jm/SnowflakeOS/blob/b7335e50918dea7cae0119d81341f29d59bcebe9/kernel/src/devices/mouse.c
  __attribute__((interrupt))
  void IntHandlerMouse(InterruptFrame* frame)
  { 
    mouse_code[mouse_index] = static_cast<uint8_t>(IoIn8(0x60));

    mouse_index = (mouse_index + 1)%3;
    if (mouse_index == 0)
    {
      if (!usb::HIDMouseDriver::default_observer)
      {
        goto END;
      }

      #define MOUSE_Y_OVERFLOW (1 << 7)
      #define MOUSE_X_OVERFLOW (1 << 6)
      #define MOUSE_Y_NEG (1 << 5)
      #define MOUSE_X_NEG (1 << 4)
      uint8_t flags = mouse_code[0];
      int32_t delta_x = (int32_t) mouse_code[1];
      int32_t delta_y = (int32_t) mouse_code[2];
      // Packets with X or Y overflow are probably garbage
      if (flags & MOUSE_X_OVERFLOW || flags & MOUSE_Y_OVERFLOW)
      {
          goto END;
      }

      // Two's complement by hand
      if (flags & MOUSE_X_NEG)
      {
          delta_x |= 0xFFFFFF00;
      }

      if (flags & MOUSE_Y_NEG)
      {
          delta_y |= 0xFFFFFF00;
      }

      usb::HIDMouseDriver::default_observer(flags, delta_x, -delta_y);
    }
END:
    IoOut8(0xA0, 0x20);
    IoOut8(0x20, 0x20);
  }
}

// ref: https://stackoverflow.com/questions/38877152/ps-2-mouse-not-firing
void mouse_wait(unsigned char type)
{
  unsigned int _time_out=100000;
  if(type==0)
  {
    while(_time_out--) //Data
    {
      if((IoIn8(0x64) & 1)==1)
      {
        return;
      }
    }
    return;
  }
  else
  {
    while(_time_out--) //Signal
    {
      if((IoIn8(0x64) & 2)==0)
      {
        return;
      }
    }
    return;
  }
}

void mouse_write(unsigned char a_write)
{
  // Wait to be able to send a command
  mouse_wait(1);
  // Tell the mouse we are sending a command
  IoOut8(0x64, 0xD4); //  	Write next byte to second PS/2 port input buffer
  // Wait for the final part
  mouse_wait(1);
  // Finally write
  IoOut8(0x60, a_write);
}

unsigned char mouse_read()
{
  // Get response from mouse
  mouse_wait(0);
  return IoIn8(0x60);
}

// 0x64 : PS2_CMDPORT
// 0x60 : PS2_DATAPORT
void enable_ps2_mouse()
{
  mouse_wait(1);
  IoOut8(0x64, 0xA8); // Enable second PS/2 port

  // Read Config and enable second ps/2 port interrupt
  mouse_wait(1);
  IoOut8(0x64, 0x20);

  unsigned char status_byte;
  mouse_wait(0);
  status_byte = (IoIn8(0x60) | 2);

  // write ps/2 config
  mouse_wait(1);
  IoOut8(0x64, 0x60); // Write next byte to "byte 0" of internal RAM

  mouse_wait(1);
  IoOut8(0x60, status_byte);

  mouse_write(0xF6); // Set Defaults
  mouse_read();

  mouse_write(0xF4); // Enable Data Reporting
  mouse_read();
}


// Due to the lack of USB virtualization support, the PIC is used instead
void EnableLegacyPIC()
{
  // mouse
  enable_ps2_mouse();

  // init master
  IoOut8(0x20, 0x11);  // ICW1
  IoOut8(0x21, 0x50);  // base int vector
  IoOut8(0x21, 0x04);  // slave pos
  IoOut8(0x21, 0x01);  // 8086mode, disable aeoi
  IoOut8(0x21, 0xF9);  // enable keyboard and slave
  // init slave
  IoOut8(0xA0, 0x11);
  IoOut8(0xA1, 0x58);  // slave vector
  IoOut8(0xA1, 0x02);  // slave id
  IoOut8(0xA1, 0x01);
  IoOut8(0xA1, 0xEF);  // enalbe only mouse
}

void InitializeInterrupt() {
  auto set_idt_entry = [](int irq, auto handler) {
    SetIDTEntry(idt[irq],
                MakeIDTAttr(DescriptorType::kInterruptGate, 0),
                reinterpret_cast<uint64_t>(handler),
                kKernelCS);
  };
  set_idt_entry(InterruptVector::kXHCI, IntHandlerXHCI);
  SetIDTEntry(idt[InterruptVector::kLAPICTimer],
              MakeIDTAttr(DescriptorType::kInterruptGate, 0 /* DPL */,
                          true /* present */, kISTForTimer /* IST */),
              reinterpret_cast<uint64_t>(IntHandlerLAPICTimer),
              kKernelCS);
  set_idt_entry(0,  IntHandlerDE);
  set_idt_entry(1,  IntHandlerDB);
  set_idt_entry(3,  IntHandlerBP);
  set_idt_entry(4,  IntHandlerOF);
  set_idt_entry(5,  IntHandlerBR);
  set_idt_entry(6,  IntHandlerUD);
  set_idt_entry(7,  IntHandlerNM);
  set_idt_entry(8,  IntHandlerDF);
  set_idt_entry(10, IntHandlerTS);
  set_idt_entry(11, IntHandlerNP);
  set_idt_entry(12, IntHandlerSS);
  set_idt_entry(13, IntHandlerGP);
  set_idt_entry(14, IntHandlerPF);
  set_idt_entry(16, IntHandlerMF);
  set_idt_entry(17, IntHandlerAC);
  set_idt_entry(18, IntHandlerMC);
  set_idt_entry(19, IntHandlerXM);
  set_idt_entry(20, IntHandlerVE);
  set_idt_entry(InterruptVector::kKeyBoardPS2, IntHandlerKeyBoard);
  set_idt_entry(InterruptVector::kMousePS2, IntHandlerMouse);
  LoadIDT(sizeof(idt) - 1, reinterpret_cast<uintptr_t>(&idt[0]));
  EnableLegacyPIC();
}

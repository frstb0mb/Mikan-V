#include "keyboard.hpp"

#include <memory>
#include "usb/classdriver/keyboard.hpp"
#include "task.hpp"

namespace {

const char keycode_map[256] = {
  0,    0,    0,    0,    'a',  'b',  'c',  'd', // 0
  'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l', // 8
  'm',  'n',  'o',  'p',  'q',  'r',  's',  't', // 16
  'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2', // 24
  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0', // 32
  '\n', '\b', 0x08, '\t', ' ',  '-',  '=',  '[', // 40
  ']', '\\',  '#',  ';', '\'',  '`',  ',',  '.', // 48
  '/',  0,    0,    0,    0,    0,    0,    0,   // 56
  0,    0,    0,    0,    0,    0,    0,    0,   // 64
  0,    0,    0,    0,    0,    0,    0,    0,   // 72
  0,    0,    0,    0,    '/',  '*',  '-',  '+', // 80
  '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7', // 88
  '8',  '9',  '0',  '.', '\\',  0,    0,    '=', // 96
  0,    0,    0,    0,    0,    0,    0,    0,   // 104
  0,    0,    0,    0,    0,    0,    0,    0,   // 112
  0,    0,    0,    0,    0,    0,    0,    0,   // 120
  0,    0,    0,    0,    0,    0,    0,    0,   // 128
  0,    '\\', 0,    0,    0,    0,    0,    0,   // 136
};

const char keycode_map_shifted[256] = {
  0,    0,    0,    0,    'A',  'B',  'C',  'D', // 0
  'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L', // 8
  'M',  'N',  'O',  'P',  'Q',  'R',  'S',  'T', // 16
  'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@', // 24
  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')', // 32
  '\n', '\b', 0x08, '\t', ' ',  '_',  '+',  '{', // 40
  '}',  '|',  '~',  ':',  '"',  '~',  '<',  '>', // 48
  '?',  0,    0,    0,    0,    0,    0,    0,   // 56
  0,    0,    0,    0,    0,    0,    0,    0,   // 64
  0,    0,    0,    0,    0,    0,    0,    0,   // 72
  0,    0,    0,    0,    '/',  '*',  '-',  '+', // 80
  '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7', // 88
  '8',  '9',  '0',  '.', '\\',  0,    0,    '=', // 96
  0,    0,    0,    0,    0,    0,    0,    0,   // 104
  0,    0,    0,    0,    0,    0,    0,    0,   // 112
  0,    0,    0,    0,    0,    0,    0,    0,   // 120
  0,    0,    0,    0,    0,    0,    0,    0,   // 128
  0,    '|',  0,    0,    0,    0,    0,    0,   // 136
};

const uint8_t convPs2ToUSB[] {
  0, 0, 30, 31, 32, 33, 34, 35,
  36, 37, 38, 39, 45, 46, 42, 43,
  20, 26, 8, 21, 23, 28, 24, 12,
  18, 19, 47, 48, 40, 0, 4, 22,
  7, 9, 10, 11, 13, 14, 15, 51,
  52, 53, 0, 49, 29, 27, 6, 25, 5,
  17, 16, 54, 55, 56, 0, 0, 0,
  44, 0, 0, 59, 60, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 95, 96,
  97, 92, 93, 0, 94, 0, 89, 90,
  91, 98, 99, 0, 0, 0, 0, 0,
};

} // namespace

void InitializeKeyboard() {
  usb::HIDKeyboardDriver::default_observer =
    [](uint8_t modifier, uint8_t keycode, bool press) {
      const bool shift = (modifier & (kLShiftBitMask | kRShiftBitMask)) != 0;
      char ascii = keycode_map[keycode];
      if (shift) {
        ascii = keycode_map_shifted[keycode];
      }
      Message msg{Message::kKeyPush};
      msg.arg.keyboard.modifier = modifier;
      msg.arg.keyboard.keycode = keycode;
      msg.arg.keyboard.ascii = ascii;
      msg.arg.keyboard.press = press;
      task_manager->SendMessage(1, msg);
    };
}

uint8_t SetOrClear(uint8_t bits, uint8_t flag, bool set)
{
  if (set)
  {
    bits |= flag;
  }
  else
  {
    bits &= ~flag;
  }
  return bits;
}

void SendPS2Key(uint8_t keycode)
{
  static uint8_t modifier = 0;
  if (!usb::HIDKeyboardDriver::default_observer)
  {
    return;
  }

  const bool press = keycode < 0x80;

  if (!press)
  {
    keycode-=0x80;
  }
  if (keycode >= 88)
  {
    return;
  }

  switch (keycode)
  {
    case 0x1d: // lctrl
      modifier = SetOrClear(modifier, kLControlBitMask, press);
      break;
    case 0x2a: // lshift
      modifier = SetOrClear(modifier, kLShiftBitMask, press);
      break;
    case 0x36: // rshift
      modifier = SetOrClear(modifier, kRShiftBitMask, press);
      break;
    case 0x38: // lalt
      modifier = SetOrClear(modifier, kLAltBitMask, press);
      break;
  }
  
  usb::HIDKeyboardDriver::default_observer(modifier, convPs2ToUSB[keycode], press);
}
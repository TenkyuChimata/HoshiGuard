#pragma once

#include "clock_options.h"

#if HOSHIGUARD_CLOCK_PROFILE
#include <stdint.h>

namespace board_clock {
enum class Fault : uint32_t {
  None = 0U,
  Oscillator = 1U,
  BusClock = 2U,
  PeripheralClock = 3U,
  Verification = 4U,
  ClockSecurity = 5U,
  UnexpectedNmi = 6U
};
// UART 初期化前・NMI 内でも SWD から診断できる。リセットを跨いで保持はしない。
extern volatile Fault lastFault;
void guard();
}  // namespace board_clock
#endif

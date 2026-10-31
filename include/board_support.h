#pragma once

#include "hardware_options.h"

// GPIO 操作はこのモジュールだけに閉じ込める。無効時は呼び出し側もコンパイルから除外する。
namespace board {
#if HOSHIGUARD_STATUS_LED
void beginStatusLed();
void setStatusLed(bool on);
#endif
#if HOSHIGUARD_OLED_SWITCH
bool oledDisabledAtBoot();
#endif
#if HOSHIGUARD_GNSS_RESET
void beginGnssReset();
void setGnssReset(bool asserted);
#endif
}

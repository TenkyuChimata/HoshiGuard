#include "board_support.h"

#if defined(ARDUINO) && (HOSHIGUARD_STATUS_LED || HOSHIGUARD_OLED_SWITCH || HOSHIGUARD_GNSS_RESET)
#include <Arduino.h>

namespace board {
#if HOSHIGUARD_STATUS_LED
void beginStatusLed() {
  __HAL_RCC_GPIOC_CLK_ENABLE();
  // PC13 は電流源として使用しない。外部 3.3 V → 抵抗 → LED → PC13 の接続専用。
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
  GPIO_InitTypeDef pin = {};
  pin.Pin = GPIO_PIN_13;
  pin.Mode = GPIO_MODE_OUTPUT_OD;
  pin.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &pin);
}
void setStatusLed(bool on) {
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
#endif
#if HOSHIGUARD_OLED_SWITCH
bool oledDisabledAtBoot() {
  // SW4 OPTIONS 第 1 位 = PB12。OFF/未接続は内部プルダウンにより表示を許可する。
  pinMode(PB12, INPUT_PULLDOWN);
  return digitalRead(PB12) == HIGH;
}
#endif
#if HOSHIGUARD_GNSS_RESET
void beginGnssReset() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_AFIO_CLK_ENABLE();
  // JTDI の PA15 を解放し、PA13/PA14 の SWD は残す。PB3/PB4 は使用しない。
  __HAL_AFIO_REMAP_SWJ_NOJTAG();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
  GPIO_InitTypeDef pin = {};
  pin.Pin = GPIO_PIN_15;
  pin.Mode = GPIO_MODE_OUTPUT_OD;
  pin.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &pin);
}
void setGnssReset(bool asserted) {
  // 非アサート時は高インピーダンス。MAX-M10S 内蔵 V_IO プルアップに任せる。
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
#endif
}
#endif

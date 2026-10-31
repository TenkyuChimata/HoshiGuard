#include "hardware_options.h"

#if USE_TIM1_INPUT_CAPTURE && defined(ARDUINO)
#include <Arduino.h>
#include <HardwareTimer.h>

#include "config.h"
#include "tim1_capture.h"

namespace pps_hardware {
namespace {
HardwareTimer timer;
bool initialized = false;
bool active = false;

struct Registers {
  uint32_t status() const { return TIM1->SR; }
  uint16_t capture() const { return static_cast<uint16_t>(TIM1->CCR1); }
  bool edgePending() const { return (EXTI->PR & GPIO_PIN_8) != 0U; }
  void discard() const {
    TIM1->CCER = 0U;
    // rc_w0: RMW で並行して立ったフラグを消さない。UIF は保持する。
    TIM1->SR = ~(TIM_SR_CC1IF | TIM_SR_CC1OF);
    TIM1->CCER = TIM_CCER_CC1E;
  }
};
}

bool beginTim1() {
  active = false;
  if (digitalPinToPinName(PPS_PIN) != PA_8) return false;
  if (initialized && (HardwareTimer_Handle[TIMER1_INDEX] == 0 ||
      &HardwareTimer_Handle[TIMER1_INDEX]->handle != timer.getHandle())) return false;
  // 別の HardwareTimer、PWM、HAL/直接操作、remap の使用を上書きしない。
  if (!initialized && (HardwareTimer_Handle[TIMER1_INDEX] != 0 ||
      (RCC->APB2ENR & RCC_APB2ENR_TIM1EN) != 0U ||
      (AFIO->MAPR & AFIO_MAPR_TIM1_REMAP) != 0U)) return false;
  uint32_t clock = HAL_RCC_GetPCLK2Freq();
  // PPRE2 の 0xx はすべて分周なし。最上位 bit が 1 の場合だけタイマークロックを 2 倍にする。
  if ((RCC->CFGR & RCC_CFGR_PPRE2_2) != 0U) clock *= 2U;
  if (clock < 1000000UL || clock % 1000000UL != 0U ||
      clock / 1000000UL > 65536UL) return false;
  if (!initialized) {
    timer.setup(TIM1);
    initialized = true;
  }
  TIM1->CR1 = 0U;
  timer.setMode(1U, TIMER_INPUT_CAPTURE_RISING, PPS_PIN);
  TIM1->CCER = 0U;
  TIM1->DIER = 0U;  // TIM1 の割り込み・DMA は一切使用しない。
  TIM1->SMCR = 0U;
  TIM1->PSC = clock / 1000000UL - 1U;
  TIM1->ARR = 65535U;
  TIM1->RCR = 0U;
  TIM1->CNT = 0U;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->SR = 0U;
  TIM1->CCER = TIM_CCER_CC1E;
  TIM1->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
  active = TIM1->PSC == clock / 1000000UL - 1U && TIM1->ARR == 65535U &&
           TIM1->CCMR1 == TIM_CCMR1_CC1S_0 && TIM1->DIER == 0U &&
           TIM1->CCER == TIM_CCER_CC1E &&
           TIM1->CR1 == (TIM_CR1_URS | TIM_CR1_CEN);
  if (!active) {
    TIM1->CCER = 0U;
    TIM1->CR1 = 0U;
  }
  return active;
}

uint32_t sampleTim1() {
  if (!active) return 0U;
  Registers io;
  return readCapture(io);
}

void discardTim1() {
  if (!active) return;
  Registers io;
  io.discard();
}
}  // namespace pps_hardware
#endif

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
volatile bool active = false;
uint32_t clockConfiguration = 0U;
uint32_t clockControl = 0U;

bool ownsTimer() {
  return initialized && HardwareTimer_Handle[TIMER1_INDEX] != 0 &&
         &HardwareTimer_Handle[TIMER1_INDEX]->handle == timer.getHandle();
}

bool configured() {
  // 所有者が変わった場合は、他者の TIM1 レジスターを読み書きしない。
  return ownsTimer() && (RCC->APB2ENR & RCC_APB2ENR_TIM1EN) != 0U &&
         (RCC->APB2RSTR & RCC_APB2RSTR_TIM1RST) == 0U &&
         RCC->CFGR == clockConfiguration && RCC->CR == clockControl &&
         HAL_RCC_GetHCLKFreq() == kCounterHz &&
         (AFIO->MAPR & AFIO_MAPR_TIM1_REMAP) == 0U &&
         TIM1->PSC == 0U && TIM1->ARR == kCounterPeriod - 1U && TIM1->RCR == 0U &&
         TIM1->CR1 == (TIM_CR1_URS | TIM_CR1_CEN) && TIM1->CR2 == 0U &&
         TIM1->SMCR == 0U && TIM1->CCMR1 == TIM_CCMR1_CC1S_0 && TIM1->CCMR2 == 0U &&
         TIM1->CCER == TIM_CCER_CC1E && TIM1->DIER == 0U && TIM1->BDTR == 0U &&
         NVIC_GetEnableIRQ(TIM1_UP_IRQn) == 0U && NVIC_GetEnableIRQ(TIM1_CC_IRQn) == 0U;
}

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
  if (initialized && (!ownsTimer() || (RCC->APB2ENR & RCC_APB2ENR_TIM1EN) == 0U)) return false;
  // 別の HardwareTimer、PWM、HAL/直接操作、remap の使用を上書きしない。
  if (!initialized && (HardwareTimer_Handle[TIMER1_INDEX] != 0 ||
      (RCC->APB2ENR & RCC_APB2ENR_TIM1EN) != 0U)) return false;
  if ((AFIO->MAPR & AFIO_MAPR_TIM1_REMAP) != 0U ||
      (RCC->APB2RSTR & RCC_APB2RSTR_TIM1RST) != 0U) return false;
  clockConfiguration = RCC->CFGR;
  clockControl = RCC->CR;
  // HAL の SYSCLK 復号と RCC の分周設定を併用し、古い SystemCoreClock も拒否する。
  const uint32_t sysclk = HAL_RCC_GetSysClockFreq();
  uint32_t halTimerClock = HAL_RCC_GetPCLK2Freq();
  if ((clockConfiguration & RCC_CFGR_PPRE2_2) != 0U) halTimerClock *= 2U;
  if (sysclk != kCounterHz || tim1InputClock(sysclk, clockConfiguration) != kCounterHz ||
      HAL_RCC_GetHCLKFreq() != kCounterHz || halTimerClock != kCounterHz ||
      (clockControl & RCC_CR_PLLRDY) == 0U ||
      (clockControl & ((clockConfiguration & RCC_CFGR_PLLSRC) != 0U ?
                       RCC_CR_HSERDY : RCC_CR_HSIRDY)) == 0U ||
      ((clockConfiguration & RCC_CFGR_SW) << 2U) != (clockConfiguration & RCC_CFGR_SWS)) return false;
  if (!initialized) {
    timer.setup(TIM1);
    initialized = true;
  }
  // STM32duino 2.4.0 の Base MSP は DIER=0 でも NVIC を有効にするため、明示的に戻す。
  NVIC_DisableIRQ(TIM1_UP_IRQn);
  NVIC_DisableIRQ(TIM1_CC_IRQn);
  NVIC_ClearPendingIRQ(TIM1_UP_IRQn);
  NVIC_ClearPendingIRQ(TIM1_CC_IRQn);
  TIM1->CR1 = 0U;
  timer.setMode(1U, TIMER_INPUT_CAPTURE_RISING, PPS_PIN);
  TIM1->CCER = 0U;
  TIM1->DIER = 0U;  // TIM1 の割り込み・DMA は一切使用しない。
  TIM1->CR2 = 0U;
  TIM1->SMCR = 0U;
  TIM1->CCMR2 = 0U;
  TIM1->BDTR = 0U;
  TIM1->PSC = 0U;
  TIM1->ARR = kCounterPeriod - 1U;
  TIM1->RCR = 0U;
  TIM1->CNT = 0U;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->SR = 0U;
  TIM1->CCER = TIM_CCER_CC1E;
  TIM1->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
  active = configured();
  if (!active) {
    TIM1->CCER = 0U;
    TIM1->CR1 = 0U;
  }
  return active;
}

bool tim1Active() {
  // 異常はラッチする。明示的な begin() まで証跡を再開せず、他者の設定も上書きしない。
  if (active && !configured()) active = false;
  return active;
}

uint32_t sampleTim1() {
  if (!tim1Active()) return kUnavailable;
  Registers io;
  return readCapture(io);
}

void discardTim1() {
  if (!tim1Active()) return;
  Registers io;
  io.discard();
}
}  // namespace pps_hardware
#endif

#include "clock_options.h"

#if HOSHIGUARD_CLOCK_PROFILE && defined(ARDUINO)
#include <Arduino.h>

#include "board_clock.h"

static_assert(HSE_VALUE == 8000000U, "72 MHz 設定には実基板と一致する 8 MHz HSE 定義が必要です");

namespace board_clock {
volatile Fault lastFault = Fault::None;
namespace {
bool initialized = false;
const uint32_t kClockMask = RCC_CFGR_SW | RCC_CFGR_SWS | RCC_CFGR_HPRE |
    RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_ADCPRE | RCC_CFGR_PLLSRC |
    RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL | RCC_CFGR_USBPRE;
const uint32_t kClockBits = RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL | RCC_CFGR_PPRE1_DIV2 |
    RCC_CFGR_ADCPRE_DIV6 | RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;
const uint32_t kReadyMask = RCC_CR_HSION | RCC_CR_HSIRDY | RCC_CR_HSEON | RCC_CR_HSERDY |
    RCC_CR_HSEBYP | RCC_CR_PLLON | RCC_CR_PLLRDY | RCC_CR_CSSON;
const uint32_t kReadyBits = RCC_CR_HSION | RCC_CR_HSIRDY | RCC_CR_HSEON | RCC_CR_HSERDY |
    RCC_CR_PLLON | RCC_CR_PLLRDY | RCC_CR_CSSON |
    (HOSHIGUARD_CLOCK_PROFILE == 2 ? RCC_CR_HSEBYP : 0U);

bool registersValid() {
  return (RCC->CFGR & kClockMask) == kClockBits &&
         (RCC->CR & kReadyMask) == kReadyBits && SystemCoreClock == 72000000U &&
         (FLASH->ACR & (FLASH_ACR_LATENCY | FLASH_ACR_PRFTBE | FLASH_ACR_HLFCYA)) ==
             (FLASH_LATENCY_2 | FLASH_ACR_PRFTBE) &&
         SysTick->LOAD == 71999U && (SysTick->CTRL & 7U) == 7U;
}

[[noreturn]] void stop(Fault reason) {
  // HSI への自動切替後に、古い BRR・SysTick のままデータを送り続けない。
  __disable_irq();
  if ((RCC->APB2ENR & RCC_APB2ENR_USART1EN) != 0U) USART1->CR1 = 0U;
  if ((RCC->APB1ENR & RCC_APB1ENR_USART2EN) != 0U) USART2->CR1 = 0U;
  if ((RCC->APB2ENR & RCC_APB2ENR_TIM1EN) != 0U) {
    TIM1->DIER = 0U;
    TIM1->CCER = 0U;
    TIM1->CR1 = 0U;
  }
  if ((RCC->APB1ENR & RCC_APB1ENR_I2C1EN) != 0U) I2C1->CR1 = 0U;
  SysTick->CTRL = 0U;
  lastFault = reason;
  // 意図的な安全停止。給餌しないため、既に動作中の IWDG は独立 LSI でリセットできる。
  // SWD または外部リセットで調査・再起動する。時刻系の部分的な自動復旧は行わない。
  for (;;) __WFI();
}
}  // namespace

void guard() {
  if (!initialized || !registersValid()) stop(Fault::Verification);
}

void configure() {
  // USB の再開等で再度呼ばれても、動作中の PLL や時基を再設定しない。
  if (initialized) { guard(); return; }
  RCC_OscInitTypeDef oscillator = {};
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI;
  oscillator.HSEState = HOSHIGUARD_CLOCK_PROFILE == 2 ? RCC_HSE_BYPASS : RCC_HSE_ON;
  oscillator.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  oscillator.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) stop(Fault::Oscillator);
  // SYSCLK を HSE 由来の PLL へ切り替える前から、発振器故障を NMI で受ける。
  HAL_RCC_EnableCSS();

  __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
  RCC_ClkInitTypeDef clocks = {};
  clocks.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clocks.APB1CLKDivider = RCC_HCLK_DIV2;
  clocks.APB2CLKDivider = RCC_HCLK_DIV1;
  // HAL が Flash wait state、SystemCoreClock と 1 ms SysTick を更新する。
  if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK) stop(Fault::BusClock);

  RCC_PeriphCLKInitTypeDef peripherals = {};
  peripherals.PeriphClockSelection = RCC_PERIPHCLK_ADC | RCC_PERIPHCLK_USB;
  peripherals.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  peripherals.UsbClockSelection = RCC_USBCLKSOURCE_PLL_DIV1_5;
  if (HAL_RCCEx_PeriphCLKConfig(&peripherals) != HAL_OK) stop(Fault::PeripheralClock);

  RCC_ClkInitTypeDef readback = {};
  uint32_t latency = 0U;
  HAL_RCC_GetClockConfig(&readback, &latency);
  if (!registersValid() || HAL_RCC_GetSysClockFreq() != 72000000U ||
      HAL_RCC_GetHCLKFreq() != 72000000U || HAL_RCC_GetPCLK1Freq() != 36000000U ||
      HAL_RCC_GetPCLK2Freq() != 72000000U || latency != FLASH_LATENCY_2 ||
      readback.SYSCLKSource != RCC_SYSCLKSOURCE_PLLCLK || readback.AHBCLKDivider != RCC_SYSCLK_DIV1 ||
      readback.APB1CLKDivider != RCC_HCLK_DIV2 || readback.APB2CLKDivider != RCC_HCLK_DIV1)
    stop(Fault::Verification);
  initialized = true;
}

void nmi() {
  const bool css = __HAL_RCC_GET_IT(RCC_IT_CSS) != 0U;
  // RM0008 §7.2.7: CSSC を消去しないと NMI が繰り返される。HAL callback の後処理を待たない。
  if (css) __HAL_RCC_CLEAR_IT(RCC_IT_CSS);
  stop(css ? Fault::ClockSecurity : Fault::UnexpectedNmi);
}
}  // namespace board_clock

// STM32duino の weak variant 関数と、startup の weak NMI vector を選択時だけ置換する。
extern "C" void SystemClock_Config(void) { board_clock::configure(); }
extern "C" void NMI_Handler(void) { board_clock::nmi(); }
#endif

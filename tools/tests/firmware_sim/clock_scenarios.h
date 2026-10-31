#pragma once

#include "board_clock.h"
#include "clock_core_hal.h"
#include "uart_brr.h"

namespace {
void initializeClock() {
  RCC->CFGR=0U;
  RCC->CR=RCC_CR_HSION|RCC_CR_HSIRDY;
  SystemCoreClock=sim::sysclk=sim::hclk=sim::pclk2=8000000U;
  SysTick->LOAD=7999U;
  SystemClock_Config();
}

#if HOSHIGUARD_CLOCK_PROFILE
void assertClockStopped(board_clock::Fault expected) {
  assert(board_clock::lastFault==expected && sim::irqDisabled);
  assert(USART1->CR1==0U && USART2->CR1==0U && I2C1->CR1==0U && SysTick->CTRL==0U);
  assert(TIM1->CR1==0U && TIM1->DIER==0U && TIM1->CCER==0U);
}
#endif

void clockScenario(const char *scenario) {
#if HOSHIGUARD_CLOCK_PROFILE
  if (strcmp(scenario,"clock-normal")) {
    const bool runtime=!strncmp(scenario,"clock-runtime-",14U);
    bool stopped=false;
    if (!runtime) sim::clockFault=scenario;
    // 故障後、開いていた UART/捕捉/I2C から新しいデータ処理が続かない。
    RCC->APB2ENR=RCC_APB2ENR_USART1EN|RCC_APB2ENR_TIM1EN;
    RCC->APB1ENR=RCC_APB1ENR_USART2EN|RCC_APB1ENR_I2C1EN;
    USART1->CR1=USART2->CR1=I2C1->CR1=1U;
    TIM1->CR1=TIM1->CCER=TIM1->DIER=1U;
    board_clock::Fault expected=board_clock::Fault::Verification;
    try {
      initializeClock();
      if (runtime) {
        const char *fault=scenario+14U;
        if (!strcmp(fault,"css")) {
          // CSS は PLL/HSE を落として HSI へ切替える。SystemCoreClock は自動更新されない。
          RCC->CR &= ~(RCC_CR_HSEON|RCC_CR_HSERDY|RCC_CR_PLLON|RCC_CR_PLLRDY);
          RCC->CFGR &= ~(RCC_CFGR_SW|RCC_CFGR_SWS);
          RCC->CIR |= RCC_IT_CSS;
          expected=board_clock::Fault::ClockSecurity;
          NMI_Handler();
        } else if (!strcmp(fault,"nmi")) {
          expected=board_clock::Fault::UnexpectedNmi;
          NMI_Handler();
        } else {
          if (!strcmp(fault,"divider")) RCC->CFGR ^= RCC_CFGR_PPRE1_DIV2;
          else if (!strcmp(fault,"source")) RCC->CFGR &= ~RCC_CFGR_SWS;
          else if (!strcmp(fault,"hse")) RCC->CR &= ~RCC_CR_HSERDY;
          else if (!strcmp(fault,"css-off")) RCC->CR &= ~RCC_CR_CSSON;
          else if (!strcmp(fault,"core")) SystemCoreClock=48000000U;
          else if (!strcmp(fault,"latency")) FLASH->ACR &= ~FLASH_ACR_LATENCY;
          else if (!strcmp(fault,"systick")) SysTick->LOAD=47999U;
          else assert(false);
          loop(); // 製品の入口で、送信・時刻処理より先に停止することを確認する。
        }
      }
    } catch (const sim::ClockStopped &) {stopped=true;}
    assert(stopped);
    if (!strcmp(scenario,"clock-oscillator") || !strcmp(scenario,"clock-pll")) expected=board_clock::Fault::Oscillator;
    else if (!strcmp(scenario,"clock-bus")) expected=board_clock::Fault::BusClock;
    else if (!strcmp(scenario,"clock-peripheral")) expected=board_clock::Fault::PeripheralClock;
    assertClockStopped(expected);
    assert((RCC->CIR&RCC_IT_CSS)==0U);
    assert(Serial2.beginCount==0U && DebugUart.beginCount==0U);
    return;
  }
#else
  assert(!strcmp(scenario,"clock-normal"));
#endif
  initializeClock();
#if HOSHIGUARD_CLOCK_PROFILE
  assert(SystemCoreClock==72000000U && HAL_RCC_GetSysClockFreq()==72000000U);
  assert(HAL_RCC_GetHCLKFreq()==72000000U && HAL_RCC_GetPCLK1Freq()==36000000U && HAL_RCC_GetPCLK2Freq()==72000000U);
  assert(sim::oscillatorRequest.HSEState==(HOSHIGUARD_CLOCK_PROFILE==2 ? RCC_HSE_BYPASS : RCC_HSE_ON));
  assert(sim::oscillatorRequest.PLL.PLLSource==RCC_PLLSOURCE_HSE && sim::oscillatorRequest.PLL.PLLMUL==RCC_PLL_MUL9);
  assert(SysTick->LOAD==71999U && (RCC->CR&RCC_CR_CSSON)!=0U);
  assert((FLASH->ACR&FLASH_ACR_LATENCY)==FLASH_LATENCY_2 && (FLASH->ACR&FLASH_ACR_PRFTBE)!=0U);
  SystemClock_Config();
  assert(sim::oscillatorCalls==1U && sim::clockCalls==1U && sim::peripheralCalls==1U);
  board_clock::guard();
#else
  assert(SystemCoreClock==48000000U && HAL_RCC_GetPCLK1Freq()==24000000U && HAL_RCC_GetPCLK2Freq()==48000000U);
  assert(sim::oscillatorRequest.PLL.PLLSource==RCC_PLLSOURCE_HSI_DIV2);
  assert(sim::oscillatorRequest.PLL.PLLMUL==RCC_PLL_MUL12 && SysTick->LOAD==47999U);
#endif
  // 製品と同じ HAL の BRR マクロを使い、丸めを含めて USART1/2 の値を確認する。
  const bool high=SystemCoreClock==72000000U;
  assert(UART_BRR_SAMPLING16(HAL_RCC_GetPCLK2Freq(),115200U)==(high ? 625U : 417U));
  assert(UART_BRR_SAMPLING16(HAL_RCC_GetPCLK1Freq(),115200U)==(high ? 312U : 208U));
  assert(UART_BRR_SAMPLING16(HAL_RCC_GetPCLK1Freq(),38400U)==(high ? 937U : 625U));
  // 製品の OLED 初期化そのものを実行する。I2C の電気特性は模擬しない。
  oled::Bus bus=stm32OledBus();
  assert(bus.begin(bus.context));
  assert(I2C1->CR2==(high ? 36U : 24U));
  assert(I2C1->CCR==(I2C_CCR_FS|(high ? 30U : 20U)));
  assert(I2C1->TRISE==(high ? 11U : 8U));
  bus.end(bus.context);
  // STM32duino の実 clock.c を使い、SysTick 由来の小数 µs と 32 bit 周回を確認する。
  for (uint32_t ms : {1234U,4294967U,4294968U}) for (uint32_t us : {0U,1U,499U,999U}) {
    sim::nowMs=ms;
    SysTick->VAL=us==0U ? SysTick->LOAD : SysTick->LOAD+1U-us*(SystemCoreClock/1000000U);
    assert(getCurrentMillis()==ms);
    assert(getCurrentMicros()==static_cast<uint32_t>(static_cast<uint64_t>(ms)*1000U+us));
  }
  const uint32_t previous=getCurrentMillis();
  SysTick_Handler();
  assert(getCurrentMillis()==previous+1U);
}
}

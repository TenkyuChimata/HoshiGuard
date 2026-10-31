#pragma once

// 実機の電気特性や割り込み遅延を再現しない、ファームウェア入口のホスト試験用スタブ。
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <deque>
#include <vector>

#define F(value) value
enum { PA2=2, PA3=3, PA8=8, PA9=9, PA10=10, PA15=15, PB12=28 };
enum { LOW=0, HIGH=1, INPUT=0, INPUT_PULLDOWN=2, RISING=3 };
enum { HEX=16 };
enum { GPIO_PIN_RESET=0, GPIO_PIN_SET=1, GPIO_MODE_OUTPUT_OD=4, GPIO_SPEED_FREQ_LOW=2 };
static const uint32_t GPIO_PIN_8 = 1U << 8;
static const uint32_t GPIO_PIN_13 = 1U << 13;
static const uint32_t GPIO_PIN_15 = 1U << 15;
struct GPIO_TypeDef { unsigned port; };
struct GPIO_InitTypeDef { uint32_t Pin, Mode, Pull, Speed; };
namespace sim {
extern uint32_t nowMs;
extern GPIO_TypeDef ports[3];
extern int modes[48], levels[48];
extern unsigned swdOnly, resetAssertions, resetReleases;
extern void (*ppsInterrupt)();
extern void (*gnssWrite)(const uint8_t *, size_t);
extern bool continuousRx, stallRx, stallTx, stallOled;
extern unsigned rxReads;
struct Stalled {};
}
#define GPIOA (&sim::ports[0])
#define GPIOC (&sim::ports[2])
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define __HAL_RCC_AFIO_CLK_ENABLE() ((void)0)
#define __HAL_AFIO_REMAP_SWJ_NOJTAG() (++sim::swdOnly)
inline uint32_t millis() { return sim::nowMs; }
inline uint32_t micros() { return sim::nowMs * 1000U; }
inline void pinMode(int pin, int mode) { sim::modes[pin] = mode; }
inline int digitalRead(int pin) { return sim::levels[pin]; }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int, void (*handler)(), int) { sim::ppsInterrupt = handler; }
inline void detachInterrupt(int) { sim::ppsInterrupt = 0; }
inline void noInterrupts() {}
inline void interrupts() {}
inline void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *pin) {
  for (unsigned bit=0; bit<16; ++bit)
    if (pin->Pin & (1U << bit)) sim::modes[port->port*16U+bit] = static_cast<int>(pin->Mode);
}
inline void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint32_t mask, int level) {
  for (unsigned bit=0; bit<16; ++bit) {
    if (!(mask & (1U << bit))) continue;
    sim::levels[port->port*16U+bit] = level;
    if (port == GPIOA && bit == 15U) {
      if (level == GPIO_PIN_RESET) ++sim::resetAssertions;
      else ++sim::resetReleases;
    }
  }
}
inline char *ultoa(unsigned long value, char *buffer, int) {
  snprintf(buffer, 32U, "%lu", value);
  return buffer;
}
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *data, size_t size) {
    for (size_t i=0; i<size; ++i) write(data[i]);
    return size;
  }
  // Binary モードの診断 Print は抑止される。文字整形自体は既存の native 試験で検証する。
  template<class T> size_t print(const T &) { return 0U; }
  template<class T> size_t print(const T &, int) { return 0U; }
  template<class T> size_t println(const T &) { return 0U; }
  size_t println() { return 0U; }
};
class HardwareSerial : public Print {
 public:
  HardwareSerial(int=0, int=0) : baud(0U), beginCount(0U), room(64) {}
  void begin(uint32_t value) { baud=value; ++beginCount; }
  void end() { rx.clear(); }
  int available() const;
  int availableForWrite() const { return room; }
  int read();
  size_t write(uint8_t value) override { return write(&value, 1U); }
  size_t write(const uint8_t *data, size_t size) override;
  uint32_t baud;
  unsigned beginCount;
  int room;
  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
};
extern HardwareSerial Serial2;

// TIM1/IWDG はレジスターの副作用だけを模擬する。実時間・電気特性の証明には使わない。
struct SimStatus {
  uint32_t bits = 0U;
  operator uint32_t() const { return bits; }
  SimStatus &operator=(uint32_t value) { bits &= value; return *this; }
};
struct SimCapture {
  uint16_t bits = 0U;
  operator uint32_t() const;
};
struct TIM_TypeDef {
  uint32_t CR1=0U, CR2=0U, CCER=0U, DIER=0U, SMCR=0U, PSC=0U, ARR=0U, RCR=0U, CNT=0U, EGR=0U, CCMR1=0U, CCMR2=0U, BDTR=0U;
  SimStatus SR;
  SimCapture CCR1;
};
struct SimRcc { uint32_t APB2ENR=0U, APB1ENR=0U, APB2RSTR=0U, CFGR=0x001D040AU, CR=0x03030003U, CSR=0U, CIR=0U; };
struct SimAfio { uint32_t MAPR=0U; };
struct SimExti { uint32_t PR=0U; };
struct SimIwdg { uint32_t prescaler=0U, reload=0U, lastFeed=0U; unsigned feeds=0U; bool enabled=false; };
namespace sim {
extern TIM_TypeDef tim1;
extern SimRcc rcc;
extern SimAfio afio;
extern SimExti exti;
extern SimIwdg iwdg;
extern uint32_t pclk2;
extern uint32_t sysclk, hclk;
extern bool timerIrqEnabled[2], timerIrqPending[2];
extern unsigned timerSetups;
extern bool lsiEnabled;
}
#define TIM1 (&sim::tim1)
#define RCC (&sim::rcc)
#define AFIO (&sim::afio)
#define EXTI (&sim::exti)
#define IWDG (&sim::iwdg)
#define RCC_APB2ENR_TIM1EN (1U << 11)
#define RCC_APB2RSTR_TIM1RST (1U << 11)
#define RCC_CR_PLLRDY (1U << 25)
#define RCC_CR_HSERDY (1U << 17)
#define RCC_CR_HSIRDY (1U << 1)
#define RCC_CFGR_PLLSRC (1U << 16)
#define RCC_CFGR_SW 3U
#define RCC_CFGR_SWS 12U
#define RCC_CFGR_PPRE2 (7U << 11)
#define RCC_CFGR_PPRE2_2 (4U << 11)
#define AFIO_MAPR_TIM1_REMAP (3U << 6)
#define TIM_SR_CC1IF (1U << 1)
#define TIM_SR_CC1OF (1U << 9)
#define TIM_CCER_CC1E 1U
#define TIM_CR1_URS 4U
#define TIM_CR1_CEN 1U
#define TIM_EGR_UG 1U
#define TIM_CCMR1_CC1S_0 1U
#define IWDG_RLR_RL 4095U
#define LSI_VALUE 40000U
#define UNUSED(value) (void)(value)
inline uint32_t HAL_RCC_GetPCLK2Freq() { return sim::pclk2; }
inline uint32_t HAL_RCC_GetSysClockFreq() { return sim::sysclk; }
inline uint32_t HAL_RCC_GetHCLKFreq() { return sim::hclk; }
enum { TIM1_UP_IRQn=0, TIM1_CC_IRQn=1 };
inline void NVIC_DisableIRQ(int irq) { sim::timerIrqEnabled[irq]=false; }
inline void NVIC_ClearPendingIRQ(int irq) { sim::timerIrqPending[irq]=false; }
inline uint32_t NVIC_GetEnableIRQ(int irq) { return sim::timerIrqEnabled[irq] ? 1U : 0U; }
inline int digitalPinToPinName(int pin) { return pin; }
static const int PA_8=PA8;

#include "clock_hal.h"

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
  int available() const { return static_cast<int>(rx.size()); }
  int availableForWrite() const { return room; }
  int read() { if (rx.empty()) return -1; int value=rx.front(); rx.pop_front(); return value; }
  size_t write(uint8_t value) override { return write(&value, 1U); }
  size_t write(const uint8_t *data, size_t size) override;
  uint32_t baud;
  unsigned beginCount;
  int room;
  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
};
extern HardwareSerial Serial2;

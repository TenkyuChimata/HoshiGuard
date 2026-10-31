#include "config.h"

#if HOSHIGUARD_OLED && defined(ARDUINO)
#include <Arduino.h>

#include "oled_stm32.h"

#if !defined(STM32F1xx)
#error "OLED I2C は STM32F1 専用です"
#endif

namespace {
OledI2cWriter writer({I2C1->CR1, I2C1->DR, I2C1->SR1, I2C1->SR2});
static_assert(I2C_CR1_START == (1U << 8) && I2C_CR1_STOP == (1U << 9) &&
              I2C_SR1_SB == 1U && I2C_SR1_ADDR == (1U << 1) &&
              I2C_SR1_BTF == (1U << 2) && I2C_SR1_TXE == (1U << 7) &&
              I2C_SR2_BUSY == (1U << 1) && I2C_CR1_PE == 1U &&
              I2C_CR1_SWRST == (1U << 15) &&
              (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF |
               I2C_SR1_OVR | I2C_SR1_TIMEOUT) == 0x4F00U,
              "I2C レジスター配置の不一致");

bool beginBus(void *) {
  const uint32_t clock = HAL_RCC_GetPCLK1Freq();
  if (clock < 4000000U || clock > 36000000U) return false;
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_AFIO_CLK_ENABLE();
  __HAL_RCC_I2C1_CLK_ENABLE();
  __HAL_AFIO_REMAP_I2C1_DISABLE();
  __HAL_RCC_I2C1_FORCE_RESET();
  __HAL_RCC_I2C1_RELEASE_RESET();
  GPIO_InitTypeDef pins = {};
  pins.Pin = GPIO_PIN_6 | GPIO_PIN_7;
  pins.Mode = GPIO_MODE_AF_OD;
  pins.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &pins);
  I2C1->CR1 = 0U;
  I2C1->CR2 = clock / 1000000U;
  // Fast mode、duty 2。切り上げ除算で 400 kHz を超えない。
  I2C1->CCR = I2C_CCR_FS | ((clock + 1199999U) / 1200000U);
  I2C1->TRISE = ((clock / 1000000U) * 300U) / 1000U + 1U;
  I2C1->OAR1 = 1U << 14;
  I2C1->CR1 = I2C_CR1_PE;
  return true;
}
bool start(void *, const uint8_t *data, size_t length, uint32_t nowUs) {
  return writer.start(data, length, nowUs);
}
OledTransfer poll(void *, uint32_t nowUs) { return writer.poll(nowUs); }
void end(void *) {
  writer.stop();
  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6 | GPIO_PIN_7);
  __HAL_RCC_I2C1_CLK_DISABLE();
}
}

oled::Bus stm32OledBus() { return {0, beginBus, start, poll, end}; }
#endif

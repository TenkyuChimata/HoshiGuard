#include "Arduino.h"
#include <string.h>
#include <assert.h>
#include "clock_options.h"

uint32_t SystemCoreClock=8000000U;
namespace sim {
SimFlash flash;
SimSysTick systick;
SimUsart usart1,usart2;
SimI2c i2c1;
RCC_OscInitTypeDef oscillatorRequest;
RCC_ClkInitTypeDef clockRequest;
RCC_PeriphCLKInitTypeDef peripheralRequest;
unsigned oscillatorCalls=0U,clockCalls=0U,peripheralCalls=0U;
const char *clockFault="";
bool irqDisabled=false;
unsigned i2cBegins=0U;
}
HAL_StatusTypeDef HAL_RCC_OscConfig(RCC_OscInitTypeDef *request) {
  sim::oscillatorRequest=*request;
  ++sim::oscillatorCalls;
  if (!strcmp(sim::clockFault,"clock-oscillator")) return HAL_ERROR;
  RCC->CR=RCC_CR_HSION|RCC_CR_HSIRDY|request->HSEState|
          (request->HSEState!=0U ? RCC_CR_HSERDY : 0U)|RCC_CR_PLLON|RCC_CR_PLLRDY;
  RCC->CFGR=request->PLL.PLLSource|request->PLL.PLLMUL;
  if (!strcmp(sim::clockFault,"clock-pll")) {RCC->CR &= ~RCC_CR_PLLRDY; return HAL_ERROR;}
  return HAL_OK;
}
HAL_StatusTypeDef HAL_RCC_ClockConfig(RCC_ClkInitTypeDef *request,uint32_t latency) {
#if HOSHIGUARD_CLOCK_PROFILE
  assert((RCC->CR&RCC_CR_CSSON)!=0U);
#endif
  sim::clockRequest=*request;
  ++sim::clockCalls;
  if (!strcmp(sim::clockFault,"clock-bus")) return HAL_ERROR;
  RCC->CFGR |= request->SYSCLKSource|(request->SYSCLKSource<<2U)|request->AHBCLKDivider|
               request->APB1CLKDivider|(request->APB2CLKDivider<<3U);
  FLASH->ACR=(FLASH->ACR&~FLASH_ACR_LATENCY)|latency;
  const uint32_t multiplier=(sim::oscillatorRequest.PLL.PLLMUL>>18U)+2U;
  SystemCoreClock=sim::sysclk=sim::hclk=sim::pclk2=
      (sim::oscillatorRequest.PLL.PLLSource==RCC_PLLSOURCE_HSE ? HSE_VALUE : 4000000U)*multiplier;
  SysTick->LOAD=SystemCoreClock/1000U-1U;
  SysTick->CTRL=7U;
  if (!strcmp(sim::clockFault,"clock-core")) SystemCoreClock=48000000U;
  if (!strcmp(sim::clockFault,"clock-systick")) SysTick->LOAD=47999U;
  if (!strcmp(sim::clockFault,"clock-latency")) FLASH->ACR &= ~FLASH_ACR_LATENCY;
  if (!strcmp(sim::clockFault,"clock-prefetch")) FLASH->ACR &= ~FLASH_ACR_PRFTBE;
  if (!strcmp(sim::clockFault,"clock-bypass")) RCC->CR ^= RCC_CR_HSEBYP;
  return HAL_OK;
}
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *request) {
  sim::peripheralRequest=*request;
  ++sim::peripheralCalls;
  if (!strcmp(sim::clockFault,"clock-peripheral")) return HAL_ERROR;
  RCC->CFGR |= request->AdcClockSelection|request->UsbClockSelection;
  if (!strcmp(sim::clockFault,"clock-usb")) RCC->CFGR |= RCC_CFGR_USBPRE;
  return HAL_OK;
}
void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef *out,uint32_t *latency) {
  *out=sim::clockRequest;
  *latency=FLASH->ACR&FLASH_ACR_LATENCY;
  if (!strcmp(sim::clockFault,"clock-readback")) out->APB1CLKDivider=0U;
}

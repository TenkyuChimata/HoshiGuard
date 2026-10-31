#pragma once
#include "Arduino.h"
#include "stm32yyxx_ll_rcc.h"

// 導入済み core の clock.c をそのまま実行するための未使用周辺機能境界。
enum sourceClock_t {LSI_CLOCK,HSI_CLOCK,LSE_CLOCK,HSE_CLOCK};
#define RCC_PLL_NONE 0U
#define RESET 0U
#define RCC_OSCILLATORTYPE_NONE 0U
#define RCC_OSCILLATORTYPE_LSI 4U
#define RCC_OSCILLATORTYPE_LSE 8U
#define RCC_LSI_ON 1U
#define RCC_LSE_ON 1U
#define RCC_FLAG_LSIRDY (1U<<27)
#define RCC_FLAG_HSIRDY RCC_CR_HSIRDY
#define RCC_FLAG_HSERDY RCC_CR_HSERDY
#define RCC_FLAG_LSERDY (1U<<28)
#define __HAL_RCC_GET_FLAG(flag) (RCC->CR & (flag))
#define CFG_HW_RCC_SEMID 0
#define HSEM_LOCK_DEFAULT_RETRY 0
inline void enableBackupDomain() {}
inline void hsem_lock(int,int) {}
inline void hsem_unlock(int) {}
inline uint32_t HAL_GetTick() {return sim::nowMs;}
inline void HAL_IncTick() {++sim::nowMs;}
inline void HAL_SYSTICK_IRQHandler() {}
extern "C" uint32_t getCurrentMicros(void);
extern "C" uint32_t getCurrentMillis(void);
extern "C" void SysTick_Handler(void);

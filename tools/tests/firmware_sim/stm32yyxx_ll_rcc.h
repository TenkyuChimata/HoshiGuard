#pragma once
#include "Arduino.h"
inline void LL_RCC_LSI_Enable() { sim::lsiEnabled=true; }
inline int LL_RCC_LSI_IsReady() { return 1; }
inline bool LL_RCC_IsActiveFlag_IWDGRST() { return (RCC->CSR & (1U << 29)) != 0U; }
inline void LL_RCC_ClearResetFlags() { RCC->CSR &= ~(0xFC000000U); }

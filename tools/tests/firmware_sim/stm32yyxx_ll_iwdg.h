#pragma once
#include "Arduino.h"
#define LL_IWDG_PRESCALER_256 6U
inline void LL_IWDG_Enable(SimIwdg *r) { r->enabled=true; }
inline void LL_IWDG_EnableWriteAccess(SimIwdg *) {}
inline void LL_IWDG_SetPrescaler(SimIwdg *r, uint32_t value) { r->prescaler=value; }
inline void LL_IWDG_SetReloadCounter(SimIwdg *r, uint32_t value) { r->reload=value; }
inline int LL_IWDG_IsReady(SimIwdg *) { return 1; }
inline void LL_IWDG_ReloadCounter(SimIwdg *r) { ++r->feeds; r->lastFeed=sim::nowMs; }
inline uint32_t LL_IWDG_GetReloadCounter(SimIwdg *r) { return r->reload; }
inline uint32_t LL_IWDG_GetPrescaler(SimIwdg *r) { return r->prescaler; }
inline int LL_IWDG_IsActiveFlag_RVU(SimIwdg *) { return 0; }
inline int LL_IWDG_IsActiveFlag_PVU(SimIwdg *) { return 0; }

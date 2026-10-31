#pragma once
#include "Arduino.h"
static const unsigned TIMER1_INDEX=0U, TIMER_NUM=1U;
enum { TIMER_INPUT_CAPTURE_RISING=1 };
struct TIM_HandleTypeDef { TIM_TypeDef *Instance; };
struct timerObj_t { TIM_HandleTypeDef handle; };
extern timerObj_t *HardwareTimer_Handle[TIMER_NUM];
class HardwareTimer {
 public:
  void setup(TIM_TypeDef *timer) {
    ++sim::timerSetups;
    object_.handle.Instance=timer;
    HardwareTimer_Handle[TIMER1_INDEX]=&object_;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
  }
  void setMode(unsigned, int, int) { TIM1->CCMR1=TIM_CCMR1_CC1S_0; }
  TIM_HandleTypeDef *getHandle() { return &object_.handle; }
 private:
  timerObj_t object_ = {};
};

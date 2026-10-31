#pragma once

// 追加回路は既定で使用しない。OLED の既存既定値は config.h で維持する。
#ifndef HOSHIGUARD_STATUS_LED
#define HOSHIGUARD_STATUS_LED 0
#endif
#ifndef HOSHIGUARD_OLED_SWITCH
#define HOSHIGUARD_OLED_SWITCH 0
#endif
#ifndef HOSHIGUARD_GNSS_RESET
#define HOSHIGUARD_GNSS_RESET 0
#endif
// TIM1 は EXTI の時刻を置換せず、同じエッジの CCR1 証跡だけを追加する。
#ifndef USE_TIM1_INPUT_CAPTURE
#define USE_TIM1_INPUT_CAPTURE 0
#endif
#ifndef HOSHIGUARD_IWDG
#define HOSHIGUARD_IWDG 0
#endif

#if (HOSHIGUARD_STATUS_LED != 0 && HOSHIGUARD_STATUS_LED != 1) || \
    (HOSHIGUARD_OLED_SWITCH != 0 && HOSHIGUARD_OLED_SWITCH != 1) || \
    (HOSHIGUARD_GNSS_RESET != 0 && HOSHIGUARD_GNSS_RESET != 1) || \
    (USE_TIM1_INPUT_CAPTURE != 0 && USE_TIM1_INPUT_CAPTURE != 1) || \
    (HOSHIGUARD_IWDG != 0 && HOSHIGUARD_IWDG != 1)
#error "追加ハードウェアのスイッチには 0 または 1 を指定してください"
#endif

#if defined(ARDUINO) && (USE_TIM1_INPUT_CAPTURE || HOSHIGUARD_IWDG) && !defined(STM32F103xB)
#error "TIM1/IWDG の検証対象は STM32F103x8/xB です"
#endif

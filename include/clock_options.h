#pragma once

// 0: variant の従来動作、1: 8 MHz 水晶、2: 8 MHz 外部発振器入力。1/2 は HSE×9。
#ifndef HOSHIGUARD_CLOCK_PROFILE
#define HOSHIGUARD_CLOCK_PROFILE 0
#endif

#if HOSHIGUARD_CLOCK_PROFILE < 0 || HOSHIGUARD_CLOCK_PROFILE > 2
#error "クロック設定には 0（従来）/1（水晶）/2（外部入力）を指定してください"
#endif
#if defined(ARDUINO) && HOSHIGUARD_CLOCK_PROFILE && !defined(STM32F103xB)
#error "72 MHz クロック設定の検証対象は STM32F103x8/xB です"
#endif

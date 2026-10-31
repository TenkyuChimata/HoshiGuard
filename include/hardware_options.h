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

#if (HOSHIGUARD_STATUS_LED != 0 && HOSHIGUARD_STATUS_LED != 1) || \
    (HOSHIGUARD_OLED_SWITCH != 0 && HOSHIGUARD_OLED_SWITCH != 1) || \
    (HOSHIGUARD_GNSS_RESET != 0 && HOSHIGUARD_GNSS_RESET != 1)
#error "追加ハードウェアのスイッチには 0 または 1 を指定してください"
#endif

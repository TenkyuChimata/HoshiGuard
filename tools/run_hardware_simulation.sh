#!/bin/sh
set -eu
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1793404800}
export SOURCE_DATE_EPOCH
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d /tmp/hoshiguard-hardware-sim.XXXXXX)
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM

# Arduino/HAL 境界だけを模擬し、製品の setup/loop、UART/UBX、復旧、PPS、Bridge を実行する。
# I2C の電気動作は含めず、OLED 故障は常に NACK として注入する。
set --
for source in "$ROOT"/src/*.cpp; do
  case "$source" in */main.cpp|*/oled_stm32.cpp) continue ;; esac
  set -- "$@" "$source"
done
c++ -std=gnu++11 -Wall -Wextra -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DARDUINO=10800 -DBRIDGE_BINARY_MODE=1 -DDEBUG_LEVEL=0 \
  -DLOG_QUEUE_DEPTH=1 -DLOG_RECORD_MAX=128 \
  -DHOSHIGUARD_OLED_SWITCH=1 -DHOSHIGUARD_GNSS_RESET=1 \
  -I"$ROOT/tools/tests/firmware_sim" -I"$ROOT/include" \
  "$@" "$ROOT/tools/tests/firmware_sim/hardware_main.cpp" -o "$BUILD/hardware-test"
for scenario in silent ubx pps switch-on switch-off; do
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$BUILD/hardware-test" "$scenario"
done

# 全追加回路の無効時は GPIO に触れず、元の受信経路で動くことも確認する。
c++ -std=gnu++11 -Wall -Wextra -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DARDUINO=10800 -DBRIDGE_BINARY_MODE=1 -DDEBUG_LEVEL=0 \
  -DLOG_QUEUE_DEPTH=1 -DLOG_RECORD_MAX=128 \
  -DHOSHIGUARD_OLED=0 -DHOSHIGUARD_OLED_SWITCH=0 -DHOSHIGUARD_GNSS_RESET=0 \
  -I"$ROOT/tools/tests/firmware_sim" -I"$ROOT/include" \
  "$@" "$ROOT/tools/tests/firmware_sim/hardware_main.cpp" -o "$BUILD/minimal-test"
for scenario in ubx pps disabled; do
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$BUILD/minimal-test" "$scenario"
done

# LED と復旧・OLED を同時にコンパイルしても、それぞれ独立して動く。
c++ -std=gnu++11 -Wall -Wextra -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DARDUINO=10800 -DBRIDGE_BINARY_MODE=1 -DDEBUG_LEVEL=0 \
  -DLOG_QUEUE_DEPTH=1 -DLOG_RECORD_MAX=128 \
  -DHOSHIGUARD_STATUS_LED=1 -DHOSHIGUARD_OLED_SWITCH=1 -DHOSHIGUARD_GNSS_RESET=1 \
  -I"$ROOT/tools/tests/firmware_sim" -I"$ROOT/include" \
  "$@" "$ROOT/tools/tests/firmware_sim/hardware_main.cpp" -o "$BUILD/combined-test"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$BUILD/combined-test" switch-on

# 実際に導入された STM32duino の IWatchdog.cpp を LL 境界だけ差し替えて実行する。
WATCHDOG="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoststm32/libraries/IWatchdog/src"
for tim1 in 0 1; do
  for iwdg in 0 1; do
    binary="$BUILD/optional-$tim1-$iwdg"
    if [ "$iwdg" = 1 ]; then watchdog_source="$WATCHDOG/IWatchdog.cpp"; else watchdog_source=""; fi
    c++ -std=gnu++11 -Wall -Wextra -Wpedantic \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -DARDUINO=10800 -DSTM32F103xB -DBRIDGE_BINARY_MODE=1 -DDEBUG_LEVEL=0 \
      -DLOG_QUEUE_DEPTH=1 -DLOG_RECORD_MAX=128 \
      -DHOSHIGUARD_STATUS_LED=1 -DHOSHIGUARD_OLED_SWITCH=1 -DHOSHIGUARD_GNSS_RESET=1 \
      -DUSE_TIM1_INPUT_CAPTURE="$tim1" -DHOSHIGUARD_IWDG="$iwdg" \
      -I"$ROOT/tools/tests/firmware_sim" -I"$ROOT/include" -I"$WATCHDOG" \
      "$@" ${watchdog_source:+"$watchdog_source"} \
      "$ROOT/tools/tests/firmware_sim/hardware_main.cpp" -o "$binary"
    for scenario in silent ubx pps switch-on switch-off; do
      ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
        HOSHIGUARD_SIM_WIRE="$binary.$scenario.wire" \
        "$binary" "$scenario" > "$binary.$scenario.log"
      cat "$binary.$scenario.log"
      # 同じ入力に対する Bridge 全 wire byte が全組合せで一致する。
      cmp "$BUILD/optional-0-0.$scenario.wire" "$binary.$scenario.wire"
    done
    if [ "$tim1" = 1 ]; then
      for scenario in capture capture-apb capture-apb-undivided capture-runtime \
          timer-owner timer-gated timer-remap timer-clock timer-8mhz timer-48mhz timer-64mhz \
          timer-stale-clock timer-ahb timer-apb4 timer-unlocked timer-hse-unready timer-switching timer-reset; do
        ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$binary" "$scenario"
      done
    fi
    if [ "$iwdg" = 1 ]; then
      for scenario in watchdog stall-rx stall-tx stall-oled; do
        ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$binary" "$scenario"
      done
    fi
  done
done

# クロック設定だけを HAL 境界で模擬する。従来 variant の実ソースもリンクして weak 選択を検査する。
FRAMEWORK="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoststm32"
CLOCK_VARIANT="$FRAMEWORK/variants/STM32F1xx/F103C8T_F103CB(T-U)/generic_clock.c"
mkdir -p "$BUILD/clock-core"
for header in backup clock lock_resource otp stm32yyxx_ll_cortex stm32yyxx_ll_rcc; do
  echo '#include "clock_core_hal.h"' > "$BUILD/clock-core/$header.h"
done
# UART の算式を複製せず、導入済み HAL ヘッダーの定義をそのまま取得する。
python3 - "$FRAMEWORK" "$BUILD/clock-core/uart_brr.h" <<'PY'
import pathlib,sys
source=(pathlib.Path(sys.argv[1])/'system/Drivers/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal_uart.h').read_text()
names=('UART_DIV_SAMPLING16','UART_DIVMANT_SAMPLING16','UART_DIVFRAQ_SAMPLING16','UART_BRR_SAMPLING16')
definitions=[]
for name in names:
    lines=source.splitlines()
    index=next(i for i,line in enumerate(lines) if line.startswith('#define '+name+'('))
    definition=[lines[index]]
    while definition[-1].endswith('\\'):
        index+=1
        definition.append(lines[index])
    definitions.append('\n'.join(definition))
pathlib.Path(sys.argv[2]).write_text('\n'.join(definitions)+'\n')
PY
for profile in 0 1 2; do
  for tim1 in 0 1; do
    for iwdg in 0 1; do
      if [ "$profile" = 0 ] && { [ "$tim1" != 0 ] || [ "$iwdg" != 0 ]; }; then continue; fi
      binary="$BUILD/clock-$profile-$tim1-$iwdg"
      if [ "$iwdg" = 1 ]; then watchdog_source="$WATCHDOG/IWatchdog.cpp"; else watchdog_source=""; fi
      c++ -std=gnu++11 -Wall -Wextra -Wpedantic \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -DARDUINO=10800 -DSTM32F103xB -DSTM32F1xx -DARDUINO_GENERIC_F103C8TX -DUSBCON \
        -DHOSHIGUARD_REAL_OLED=1 \
        -DHOSHIGUARD_CLOCK_SIM=1 -DHOSHIGUARD_CLOCK_PROFILE="$profile" \
        -DBRIDGE_BINARY_MODE=1 -DDEBUG_LEVEL=0 -DLOG_QUEUE_DEPTH=1 -DLOG_RECORD_MAX=128 \
        -DHOSHIGUARD_STATUS_LED=1 -DHOSHIGUARD_OLED_SWITCH=1 -DHOSHIGUARD_GNSS_RESET=1 \
        -DUSE_TIM1_INPUT_CAPTURE="$tim1" -DHOSHIGUARD_IWDG="$iwdg" \
        -I"$BUILD/clock-core" -I"$ROOT/tools/tests/firmware_sim" -I"$ROOT/include" -I"$WATCHDOG" \
        "$@" ${watchdog_source:+"$watchdog_source"} "$CLOCK_VARIANT" \
        "$FRAMEWORK/libraries/SrcWrapper/src/stm32/clock.c" "$ROOT/src/oled_stm32.cpp" \
        "$ROOT/tools/tests/firmware_sim/clock_hal.cpp" \
        "$ROOT/tools/tests/firmware_sim/hardware_main.cpp" -o "$binary"
      for scenario in clock-normal silent ubx pps switch-on switch-off; do
        ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
          HOSHIGUARD_SIM_WIRE="$binary.$scenario.wire" "$binary" "$scenario"
        if [ "$scenario" != clock-normal ]; then
          cmp "$BUILD/clock-0-0-0.$scenario.wire" "$binary.$scenario.wire"
        fi
      done
      if [ "$profile" != 0 ]; then
        for scenario in clock-oscillator clock-pll clock-bus clock-peripheral \
            clock-core clock-systick clock-latency clock-prefetch clock-bypass clock-usb clock-readback \
            clock-runtime-css clock-runtime-nmi clock-runtime-divider clock-runtime-source \
            clock-runtime-hse clock-runtime-css-off clock-runtime-core clock-runtime-latency clock-runtime-systick; do
          ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$binary" "$scenario"
        done
      fi
    done
  done
done

# 不正値と未対応 MCU は黙って有効化せず、コンパイル時に拒否する。
for flag in USE_TIM1_INPUT_CAPTURE HOSHIGUARD_IWDG; do
  if c++ -x c++ -E -I"$ROOT/include" -include hardware_options.h -D"$flag"=2 /dev/null \
       > "$BUILD/config.log" 2>&1; then exit 1; fi
  if c++ -x c++ -E -I"$ROOT/include" -include hardware_options.h -DARDUINO=10800 -D"$flag"=1 /dev/null \
       > "$BUILD/config.log" 2>&1; then exit 1; fi
done
if c++ -x c++ -E -I"$ROOT/include" -include clock_options.h -DHOSHIGUARD_CLOCK_PROFILE=3 /dev/null \
     > "$BUILD/config.log" 2>&1; then exit 1; fi
if c++ -x c++ -E -I"$ROOT/include" -include clock_options.h -DARDUINO=10800 -DHOSHIGUARD_CLOCK_PROFILE=2 /dev/null \
     > "$BUILD/config.log" 2>&1; then exit 1; fi
for setting in '-DHSE_VALUE=12000000U'; do
  if c++ -std=gnu++11 -c -DARDUINO=10800 -DSTM32F103xB -DHOSHIGUARD_CLOCK_PROFILE=2 "$setting" \
      -I"$ROOT/tools/tests/firmware_sim" -I"$ROOT/include" "$ROOT/src/board_clock.cpp" \
      -o "$BUILD/invalid-clock.o" > "$BUILD/config.log" 2>&1; then exit 1; fi
done

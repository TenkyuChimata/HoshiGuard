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
      for scenario in capture capture-apb capture-apb-undivided timer-owner timer-gated timer-remap timer-clock; do
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

# 不正値と未対応 MCU は黙って有効化せず、コンパイル時に拒否する。
for flag in USE_TIM1_INPUT_CAPTURE HOSHIGUARD_IWDG; do
  if c++ -x c++ -E -I"$ROOT/include" -include hardware_options.h -D"$flag"=2 /dev/null \
       > "$BUILD/config.log" 2>&1; then exit 1; fi
  if c++ -x c++ -E -I"$ROOT/include" -include hardware_options.h -DARDUINO=10800 -D"$flag"=1 /dev/null \
       > "$BUILD/config.log" 2>&1; then exit 1; fi
done

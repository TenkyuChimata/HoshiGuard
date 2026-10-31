#!/bin/sh
set -eu
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

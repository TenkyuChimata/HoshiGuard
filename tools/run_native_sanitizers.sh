#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=/tmp/hoshiguard-native-sanitizer
UNITY="$ROOT/.pio/libdeps/native/Unity/src"

mkdir -p "$BUILD"
cc -std=c11 -I"$UNITY" -c "$UNITY/unity.c" -o "$BUILD/unity.o"

c++ -std=gnu++11 -Wall -Wextra -Wpedantic -Wformat=2 \
  -Wformat-truncation=2 -Wformat-overflow=2 -Wconversion -Wsign-conversion \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" -I"$UNITY" \
  "$ROOT/src/ubx_parser.cpp" \
  "$ROOT/src/qzss_l1s.cpp" \
  "$ROOT/src/dcr_dcx_decoder.cpp" \
  "$ROOT/src/gnss_config.cpp" \
  "$ROOT/src/baud_switch.cpp" \
  "$ROOT/src/gnss_self_test.cpp" \
  "$ROOT/src/gnss_rx_probe.cpp" \
  "$ROOT/src/gnss_uart_diag.cpp" \
  "$ROOT/src/raw_capture.cpp" \
  "$ROOT/src/safe_format.cpp" \
  "$ROOT/src/startup_metadata.cpp" \
  "$ROOT/src/startup_sequence.cpp" \
  "$ROOT/src/tim_tp.cpp" \
  "$ROOT/src/time_sync.cpp" \
  "$ROOT/src/ubx_dispatcher.cpp" \
  "$ROOT/src/bridge_protocol.cpp" \
  "$ROOT/test/test_main.cpp" \
  "$ROOT/test/test_ubx_parser.cpp" \
  "$ROOT/test/test_qzss_l1s.cpp" \
  "$ROOT/test/test_v020.cpp" \
  "$ROOT/test/test_v021.cpp" \
  "$ROOT/test/test_startup_logging.cpp" \
  "$ROOT/test/test_v022.cpp" \
  "$ROOT/test/test_v023.cpp" \
  "$ROOT/test/test_v024.cpp" \
  "$ROOT/test/test_v025.cpp" \
  "$BUILD/unity.o" -o "$BUILD/hoshiguard-tests"

# 管理実行環境の ptrace 制約により LeakSanitizer は動作しない。AddressSanitizer と UndefinedBehaviorSanitizer は有効のままとする。
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
  "$BUILD/hoshiguard-tests"

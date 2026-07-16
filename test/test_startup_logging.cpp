#include <unity.h>

#include <string.h>

#include "raw_capture.h"
#include "safe_format.h"
#include "startup_metadata.h"
#include "startup_sequence.h"

namespace {
StartupMetadata metadata() {
  const StartupMetadata value = {
      "0.2.2", "Jul 14 2026", "15:49:34", "unknown", "RAM",
      "UNSET", "UNSET", "UNSET", false};
  return value;
}

struct HookState {
  bool metadataResult;
  uint8_t calls;
};

bool metadataHook(void *context) {
  HookState *state = static_cast<HookState *>(context);
  state->calls = static_cast<uint8_t>(state->calls | 0x01U);
  return state->metadataResult;
}

void ppsHook(void *context) {
  HookState *state = static_cast<HookState *>(context);
  state->calls = static_cast<uint8_t>(state->calls | 0x02U);
}

void gnssHook(void *context) {
  HookState *state = static_cast<HookState *>(context);
  state->calls = static_cast<uint8_t>(state->calls | 0x04U);
}
}  // namespace

void test_safe_format_short_result() {
  char buffer[16];
  const SafeFormatResult result = safeSnprintf(buffer, sizeof(buffer), "%s", "abc");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Complete),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(3U, result.length);
  TEST_ASSERT_EQUAL_STRING("abc", buffer);
}

void test_safe_format_exact_capacity_minus_one() {
  char buffer[4];
  const SafeFormatResult result = safeSnprintf(buffer, sizeof(buffer), "%s", "abc");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Complete),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(sizeof(buffer) - 1U, result.length);
  TEST_ASSERT_EQUAL_CHAR('\0', buffer[sizeof(buffer) - 1U]);
}

void test_safe_format_truncation_is_clamped() {
  char buffer[4];
  const SafeFormatResult result = safeSnprintf(buffer, sizeof(buffer), "%s", "abcd");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Truncated),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(sizeof(buffer) - 1U, result.length);
  TEST_ASSERT_EQUAL_STRING("abc", buffer);
}

void test_safe_format_negative_result_is_error() {
  const SafeFormatResult result = safeFormattedLength(-1, 32U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Error),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(0U, result.length);
}

void test_safe_length_never_exceeds_capacity_minus_one() {
  for (int requested = 0; requested < 100; ++requested) {
    const SafeFormatResult result = safeFormattedLength(requested, 8U);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(7U, result.length);
  }
}

void test_metadata_contains_complete_disabled_value() {
  char buffer[224];
  const SafeFormatResult result =
      formatStartupMetadata(metadata(), buffer, sizeof(buffer));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Complete),
                        static_cast<int>(result.status));
  TEST_ASSERT_NOT_NULL(strstr(buffer, "META firmware=0.2.2"));
  TEST_ASSERT_NOT_NULL(strstr(buffer, "nav_coordinates=disabled"));
  TEST_ASSERT_EQUAL_UINT32(strlen(buffer), result.length);
}

void test_metadata_small_buffer_cannot_read_past_end() {
  struct Guarded {
    char before;
    char buffer[24];
    char after;
  } guarded;
  memset(&guarded, 0x5A, sizeof(guarded));
  const SafeFormatResult result = formatStartupMetadata(
      metadata(), guarded.buffer, sizeof(guarded.buffer));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Truncated),
                        static_cast<int>(result.status));
  TEST_ASSERT_EQUAL_UINT32(sizeof(guarded.buffer) - 1U, result.length);
  TEST_ASSERT_EQUAL_CHAR('\0', guarded.buffer[sizeof(guarded.buffer) - 1U]);
  TEST_ASSERT_EQUAL_HEX8(0x5A, static_cast<uint8_t>(guarded.before));
  TEST_ASSERT_EQUAL_HEX8(0x5A, static_cast<uint8_t>(guarded.after));
}

void test_truncated_metadata_contains_no_uninitialized_control_byte() {
  char buffer[32];
  memset(buffer, 0x01, sizeof(buffer));
  const SafeFormatResult result =
      formatStartupMetadata(metadata(), buffer, sizeof(buffer));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Truncated),
                        static_cast<int>(result.status));
  for (size_t i = 0U; i < result.length; ++i)
    TEST_ASSERT_NOT_EQUAL_HEX8(0x01U, static_cast<uint8_t>(buffer[i]));
  TEST_ASSERT_EQUAL_CHAR('\0', buffer[result.length]);
}

void test_metadata_failure_does_not_stop_startup_sequence() {
  HookState state = {false, 0U};
  const StartupHooks hooks = {&state, metadataHook, ppsHook, gnssHook};
  const StartupSequenceResult result = runStartupSequence(hooks);
  TEST_ASSERT_FALSE(result.metadataComplete);
  TEST_ASSERT_TRUE(result.ppsStarted);
  TEST_ASSERT_TRUE(result.gnssStarted);
  TEST_ASSERT_EQUAL_HEX8(0x07U, state.calls);
}

void test_first_serial_begin_never_calls_end() {
  TEST_ASSERT_FALSE(shouldEndSerialBeforeBegin(false));
  TEST_ASSERT_TRUE(shouldEndSerialBeforeBegin(true));
}

void test_unconnected_gnss_exhausts_finite_baud_candidates() {
  uint8_t candidate = 0U;
  TEST_ASSERT_TRUE(advanceBaudCandidate(candidate, 3U));
  TEST_ASSERT_TRUE(advanceBaudCandidate(candidate, 3U));
  TEST_ASSERT_FALSE(advanceBaudCandidate(candidate, 3U));
  TEST_ASSERT_EQUAL_UINT8(3U, candidate);
}

void test_full_fixed_log_queue_preserves_adjacent_memory() {
  struct GuardedQueue {
    uint8_t before;
    FixedLogQueue<2U, 16U> queue;
    uint8_t after;
    GuardedQueue() : before(0xA5U), queue(), after(0x5AU) {}
  } guarded;
  char *first = guarded.queue.reserve();
  TEST_ASSERT_NOT_NULL(first);
  memcpy(first, "first", 5U);
  TEST_ASSERT_TRUE(guarded.queue.commit(5U));
  char *second = guarded.queue.reserve();
  TEST_ASSERT_NOT_NULL(second);
  memcpy(second, "second", 6U);
  TEST_ASSERT_TRUE(guarded.queue.commit(6U));
  TEST_ASSERT_NULL(guarded.queue.reserve());
  TEST_ASSERT_EQUAL_UINT32(2U, guarded.queue.count());
  TEST_ASSERT_EQUAL_HEX8(0xA5U, guarded.before);
  TEST_ASSERT_EQUAL_HEX8(0x5AU, guarded.after);
}

void run_startup_logging_tests() {
  RUN_TEST(test_safe_format_short_result);
  RUN_TEST(test_safe_format_exact_capacity_minus_one);
  RUN_TEST(test_safe_format_truncation_is_clamped);
  RUN_TEST(test_safe_format_negative_result_is_error);
  RUN_TEST(test_safe_length_never_exceeds_capacity_minus_one);
  RUN_TEST(test_metadata_contains_complete_disabled_value);
  RUN_TEST(test_metadata_small_buffer_cannot_read_past_end);
  RUN_TEST(test_truncated_metadata_contains_no_uninitialized_control_byte);
  RUN_TEST(test_metadata_failure_does_not_stop_startup_sequence);
  RUN_TEST(test_first_serial_begin_never_calls_end);
  RUN_TEST(test_unconnected_gnss_exhausts_finite_baud_candidates);
  RUN_TEST(test_full_fixed_log_queue_preserves_adjacent_memory);
}

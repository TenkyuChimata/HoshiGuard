#include "test_support.h"
#include "raw_capture.h"
#include "safe_format.h"
#include "startup_metadata.h"
#include "startup_sequence.h"
#include "log_arbiter.h"
#include "fixtures/real/real_capture_arrivals.h"

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
}

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

void test_log_buffer_boundary_rejected_without_overwrite() {
  QzssL1sFrame frame = {};
  memcpy(frame.raw, kDcxSample, sizeof(frame.raw));
  char output[80];
  memset(output, 0x5A, sizeof(output));
  TEST_ASSERT_EQUAL_UINT32(0U, formatL1sRecord(frame, output, 32U));
  TEST_ASSERT_EQUAL_HEX8(0x5A, static_cast<uint8_t>(output[79]));
}

void test_oversized_raw_record_never_overflows() {
  uint8_t payload[UBX_MAX_PAYLOAD];
  memset(payload, 0xA5, sizeof(payload));
  UbxPacketView view = {0xFF, 0xEE, sizeof(payload), payload};
  char output[RAW_CAPTURE_MAX_RECORD + 1U];
  output[RAW_CAPTURE_MAX_RECORD] = 'Q';
  const size_t length = formatUbxRawRecord(0xFFFFFFFFFFFFFFFFULL, view,
                                           output, RAW_CAPTURE_MAX_RECORD);
  TEST_ASSERT_GREATER_THAN_UINT32(0U, length);
  TEST_ASSERT_LESS_THAN_UINT32(RAW_CAPTURE_MAX_RECORD, length);
  TEST_ASSERT_EQUAL_CHAR('Q', output[RAW_CAPTURE_MAX_RECORD]);
}

void test_raw_timestamp_and_length_do_not_shift_arguments() {
  const uint8_t payload[] = {1U, 2U, 3U};
  const UbxPacketView packet = {0x0AU, 0x04U, 3U, payload};
  char output[160];
  TEST_ASSERT_NOT_EQUAL(0U, formatUbxRawRecord(
      1171398ULL, packet, output, sizeof(output)));
  TEST_ASSERT_NOT_NULL(strstr(output, "timestamp_us=1171398 len=11 data="));
  TEST_ASSERT_NULL(strstr(output, "timestamp_us=lu"));

  {

  const uint8_t payload[] = {0x06, 0x8A};
  UbxPacketView view = {0x05, 0x01, sizeof(payload), payload};
  char line[128];
  const size_t length = formatUbxRawRecord(12345678U, view, line, sizeof(line));
  TEST_ASSERT_GREATER_THAN_UINT32(0U, length);
  TEST_ASSERT_NOT_NULL(strstr(line, "UBX_RAW timestamp_us=12345678 len=10 data=B56205010200"));
  TEST_ASSERT_EQUAL_CHAR('\n', line[length - 1U]);

  }
}

void test_uint64_decimal_supports_full_range() {
  char output[21];
  TEST_ASSERT_EQUAL_UINT32(20U, formatUint64Decimal(
      0xFFFFFFFFFFFFFFFFULL, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("18446744073709551615", output);
}

void test_log_record_remains_active_across_partial_writes() {
  LogRecordQueue<3U, 32U> queue;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::Queued),
      static_cast<int>(queue.enqueue(LogPriority::Capture, "RAW-ABCDEFG\n", 12U)));
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_MEMORY("RAW-", queue.frontData(), 4U);
  queue.advance(4U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::Queued),
      static_cast<int>(queue.enqueue(LogPriority::Config, "CFG\n", 4U)));
  TEST_ASSERT_EQUAL_MEMORY("ABCDEFG\n", queue.frontData(), 8U);
  queue.advance(8U);
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_MEMORY("CFG\n", queue.frontData(), 4U);

  {

  LogRecordQueue<2U, 16U> queue;
  queue.enqueue(LogPriority::Capture, "RAW\n", 4U);
  queue.enqueue(LogPriority::Config, "CFG\n", 4U);
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_CHAR('\n', queue.frontData()[queue.frontRemaining() - 1U]);
  queue.advance(queue.frontRemaining());
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_CHAR('\n', queue.frontData()[queue.frontRemaining() - 1U]);

  }
}

void test_log_queue_full_prefers_critical_over_verbose() {
  LogRecordQueue<2U, 16U> queue;
  queue.enqueue(LogPriority::Capture, "CAP\n", 4U);
  queue.enqueue(LogPriority::Verbose, "VERBOSE\n", 8U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::ReplacedLowerPriority),
      static_cast<int>(queue.enqueue(LogPriority::Critical, "ERR\n", 4U)));
  TEST_ASSERT_EQUAL_UINT32(2U, queue.count());
}

void test_log_queue_rejects_oversize_without_overwrite() {
  LogRecordQueue<1U, 8U> queue;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::Dropped),
      static_cast<int>(queue.enqueue(LogPriority::Capture, "123456789", 9U)));
  TEST_ASSERT_EQUAL_UINT32(0U, queue.count());
}

void test_compact_l1s_record_contains_replayable_complete_ubx() {
  const RealFixture &source = realFixture("qzss_l1s_mt44_pabc6");
  const UbxPacketView packet = {source.data[2], source.data[3],
      static_cast<uint16_t>(source.data[4] | (source.data[5] << 8)),
      source.data + 6U};
  SfrbxHeader header = {};
  TEST_ASSERT_TRUE(parseSfrbxHeader(packet.payload, packet.length, header));
  QzssL1sFrame frame = {};
  TEST_ASSERT_TRUE(extractQzssL1s(packet.payload, packet.length, frame));
  char line[RAW_CAPTURE_MAX_RECORD];
  TEST_ASSERT_NOT_EQUAL(0U, formatQzssL1sUbxRecord(
      12345678ULL, 99U, header, frame, packet, line, sizeof(line)));
  TEST_ASSERT_NOT_NULL(strstr(line, "numWords=9"));
  TEST_ASSERT_NOT_NULL(strstr(line, "crc=ok"));
  uint8_t recovered[64]; size_t length = 0U; uint64_t timestamp = 0U;
  TEST_ASSERT_TRUE(parseUbxRawRecord(line, recovered, sizeof(recovered),
                                     length, timestamp));
  TEST_ASSERT_EQUAL_UINT64(12345678ULL, timestamp);
  TEST_ASSERT_EQUAL_UINT32(source.length, length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(source.data, recovered, source.length);
}

void test_l1s_reserved_slots_reject_verbose_before_l1s() {
  LogRecordQueue<6U, 32U, 3U> queue;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::Queued),
      static_cast<int>(queue.enqueue(LogPriority::Verbose, "V1\n", 3U)));
  queue.enqueue(LogPriority::Verbose, "V2\n", 3U);
  queue.enqueue(LogPriority::Verbose, "V3\n", 3U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::Dropped),
      static_cast<int>(queue.enqueue(LogPriority::Verbose, "V4\n", 3U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LogEnqueueResult::Queued),
      static_cast<int>(queue.enqueue(LogPriority::QzssL1s, "L1S\n", 4U)));
}

void test_partial_l1s_output_remains_atomic_under_queue_pressure() {
  LogRecordQueue<4U, 32U, 2U> queue;
  queue.enqueue(LogPriority::QzssL1s, "QZSS_L1S_RECORD\n", 16U);
  TEST_ASSERT_TRUE(queue.activateFront());
  queue.advance(5U);
  queue.enqueue(LogPriority::Critical, "ERROR\n", 6U);
  TEST_ASSERT_EQUAL_MEMORY("L1S_RECORD\n", queue.frontData(), 11U);
  TEST_ASSERT_TRUE(queue.advance(11U));
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_MEMORY("ERROR\n", queue.frontData(), 6U);
}

void test_congested_queue_drains_to_empty() {
  LogRecordQueue<6U, 32U, 3U> queue;
  for (size_t i = 0U; i < 3U; ++i)
    queue.enqueue(LogPriority::QzssL1s, "L1S\n", 4U);
  while (queue.activateFront()) queue.advance(queue.frontRemaining());
  TEST_ASSERT_EQUAL_UINT32(0U, queue.count());
  TEST_ASSERT_EQUAL_UINT32(0U, queue.pendingBytes());
}

void test_real_arrival_pressure_has_zero_l1s_drops_at_115200() {
  LogRecordQueue<6U, 256U, 3U> queue;
  char l1s[220]; memset(l1s, 'A', sizeof(l1s)); l1s[219] = '\n';
  char verbose[180]; memset(verbose, 'V', sizeof(verbose)); verbose[179] = '\n';
  uint64_t previous = kRealL1sArrivalTimestampUs[0];
  uint64_t remainder = 0U;
  uint32_t l1sDrops = 0U, verboseDrops = 0U;
  for (size_t i = 0U; i < kRealL1sArrivalCount; ++i) {
    const uint64_t delta = kRealL1sArrivalTimestampUs[i] - previous;
    uint64_t budgetNumerator = delta * 11520ULL + remainder;
    size_t budget = static_cast<size_t>(budgetNumerator / 1000000ULL);
    remainder = budgetNumerator % 1000000ULL;
    while (budget != 0U && queue.activateFront()) {
      const size_t amount = queue.frontRemaining() < budget
                                ? queue.frontRemaining() : budget;
      queue.advance(amount); budget -= amount;
    }
    if (queue.enqueue(LogPriority::QzssL1s, l1s, sizeof(l1s)) ==
        LogEnqueueResult::Dropped) ++l1sDrops;
    if (queue.enqueue(LogPriority::Verbose, verbose, sizeof(verbose)) ==
        LogEnqueueResult::Dropped) ++verboseDrops;
    if ((i & 1U) == 0U && queue.enqueue(LogPriority::Verbose, verbose,
                                        sizeof(verbose)) == LogEnqueueResult::Dropped)
      ++verboseDrops;
    previous = kRealL1sArrivalTimestampUs[i];
  }
  TEST_ASSERT_EQUAL_UINT32(0U, l1sDrops);
  TEST_ASSERT_GREATER_THAN_UINT32(0U, verboseDrops);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(6U, queue.maxCount());
}

void test_privacy_metadata_warns_when_nav_pvt_raw_enabled() {
  char output[128];
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Complete),
      static_cast<int>(formatPrivacyMetadata(false, true, output,
                                              sizeof(output)).status));
  TEST_ASSERT_NOT_NULL(strstr(output, "privacy_mode=disabled"));
  TEST_ASSERT_NOT_NULL(strstr(output, "warning=location_data_present"));
}

void test_privacy_metadata_is_safe_by_default_shape() {
  char output[128];
  const SafeFormatResult result = formatPrivacyMetadata(
      true, false, output, sizeof(output));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafeFormatStatus::Complete),
                        static_cast<int>(result.status));
  TEST_ASSERT_NOT_NULL(strstr(output, "privacy_mode=enabled"));
  TEST_ASSERT_NOT_NULL(strstr(output, "nav_pvt_raw=disabled"));
  TEST_ASSERT_NOT_NULL(strstr(output, "warning=none"));
}

void run_logging_tests() {
  RUN_TEST(test_safe_format_short_result);
  RUN_TEST(test_safe_format_exact_capacity_minus_one);
  RUN_TEST(test_safe_format_truncation_is_clamped);
  RUN_TEST(test_safe_format_negative_result_is_error);
  RUN_TEST(test_safe_length_never_exceeds_capacity_minus_one);
  RUN_TEST(test_metadata_contains_complete_disabled_value);
  RUN_TEST(test_metadata_small_buffer_cannot_read_past_end);
  RUN_TEST(test_truncated_metadata_contains_no_uninitialized_control_byte);
  RUN_TEST(test_metadata_failure_does_not_stop_startup_sequence);
  RUN_TEST(test_full_fixed_log_queue_preserves_adjacent_memory);
  RUN_TEST(test_log_buffer_boundary_rejected_without_overwrite);
  RUN_TEST(test_oversized_raw_record_never_overflows);
  RUN_TEST(test_raw_timestamp_and_length_do_not_shift_arguments);
  RUN_TEST(test_uint64_decimal_supports_full_range);
  RUN_TEST(test_log_record_remains_active_across_partial_writes);
  RUN_TEST(test_log_queue_full_prefers_critical_over_verbose);
  RUN_TEST(test_log_queue_rejects_oversize_without_overwrite);
  RUN_TEST(test_compact_l1s_record_contains_replayable_complete_ubx);
  RUN_TEST(test_l1s_reserved_slots_reject_verbose_before_l1s);
  RUN_TEST(test_partial_l1s_output_remains_atomic_under_queue_pressure);
  RUN_TEST(test_congested_queue_drains_to_empty);
  RUN_TEST(test_real_arrival_pressure_has_zero_l1s_drops_at_115200);
  RUN_TEST(test_privacy_metadata_warns_when_nav_pvt_raw_enabled);
  RUN_TEST(test_privacy_metadata_is_safe_by_default_shape);
}

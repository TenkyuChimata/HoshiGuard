#include <unity.h>

#include <string.h>

#include "baud_switch.h"
#include "log_arbiter.h"
#include "raw_capture.h"
#include "safe_format.h"

void test_baud_is_not_a_normal_config_key() {
  TEST_ASSERT_FALSE(normalConfigContainsBaudKey());
}

void test_baud_waits_for_all_normal_config_keys() {
  TEST_ASSERT_FALSE(baudSwitchMayStart(9U, 10U));
  TEST_ASSERT_TRUE(baudSwitchMayStart(10U, 10U));
}

void test_disabled_baud_switch_does_not_request_change() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::Disabled),
      static_cast<int>(initialBaudSwitchResult(false, 38400U, 115200U)));
}

void test_equal_baud_is_not_needed() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::NotNeeded),
      static_cast<int>(initialBaudSwitchResult(true, 38400U, 38400U)));
}

void test_baud_valset_packet_is_little_endian_and_checksum_valid() {
  uint8_t packet[24] = {};
  const size_t length = buildBaudValsetPacket(115200U, 1U, packet,
                                               sizeof(packet));
  const uint8_t expectedPrefix[] = {0xB5, 0x62, 0x06, 0x8A, 0x0C, 0x00,
                                    0x00, 0x01, 0x00, 0x00,
                                    0x01, 0x00, 0x52, 0x40,
                                    0x00, 0xC2, 0x01, 0x00};
  TEST_ASSERT_EQUAL_UINT32(20U, length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedPrefix, packet, sizeof(expectedPrefix));
  uint8_t a = 0U, b = 0U;
  UbxParser::checksum(packet + 2U, length - 4U, a, b);
  TEST_ASSERT_EQUAL_HEX8(a, packet[length - 2U]);
  TEST_ASSERT_EQUAL_HEX8(b, packet[length - 1U]);
}

void test_valset_ack_alone_never_verifies_baud() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::Pending),
      static_cast<int>(evaluateBaudSwitchEvidence(
          true, 38400U, 115200U, true, false, false)));
}

void test_target_mon_ver_verifies_baud() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::Verified),
      static_cast<int>(evaluateBaudSwitchEvidence(
          true, 38400U, 115200U, false, true, false)));
}

void test_original_mon_ver_keeps_original_without_failure() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BaudSwitchResult::KeptOriginal),
      static_cast<int>(evaluateBaudSwitchEvidence(
          true, 38400U, 115200U, true, false, true)));
}

void test_recovery_sequence_is_finite_and_ordered() {
  BaudRecoverySequence sequence;
  sequence.begin(38400U, 115200U);
  const uint32_t expected[] = {115200U, 38400U, 9600U, 38400U, 115200U};
  for (size_t i = 0U; i < 5U; ++i) {
    uint32_t value = 0U;
    TEST_ASSERT_TRUE(sequence.next(value));
    TEST_ASSERT_EQUAL_UINT32(expected[i], value);
  }
  uint32_t value = 0U;
  TEST_ASSERT_FALSE(sequence.next(value));
  TEST_ASSERT_TRUE(sequence.exhausted());
}

void test_raw_timestamp_and_length_do_not_shift_arguments() {
  const uint8_t payload[] = {1U, 2U, 3U};
  const UbxPacketView packet = {0x0AU, 0x04U, 3U, payload};
  char output[160];
  TEST_ASSERT_NOT_EQUAL(0U, formatUbxRawRecord(
      1171398ULL, packet, output, sizeof(output)));
  TEST_ASSERT_NOT_NULL(strstr(output, "timestamp_us=1171398 len=11 data="));
  TEST_ASSERT_NULL(strstr(output, "timestamp_us=lu"));
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
}

void test_log_records_only_change_at_newline_boundaries() {
  LogRecordQueue<2U, 16U> queue;
  queue.enqueue(LogPriority::Capture, "RAW\n", 4U);
  queue.enqueue(LogPriority::Config, "CFG\n", 4U);
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_CHAR('\n', queue.frontData()[queue.frontRemaining() - 1U]);
  queue.advance(queue.frontRemaining());
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_CHAR('\n', queue.frontData()[queue.frontRemaining() - 1U]);
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

void run_v023_tests() {
  RUN_TEST(test_baud_is_not_a_normal_config_key);
  RUN_TEST(test_baud_waits_for_all_normal_config_keys);
  RUN_TEST(test_disabled_baud_switch_does_not_request_change);
  RUN_TEST(test_equal_baud_is_not_needed);
  RUN_TEST(test_baud_valset_packet_is_little_endian_and_checksum_valid);
  RUN_TEST(test_valset_ack_alone_never_verifies_baud);
  RUN_TEST(test_target_mon_ver_verifies_baud);
  RUN_TEST(test_original_mon_ver_keeps_original_without_failure);
  RUN_TEST(test_recovery_sequence_is_finite_and_ordered);
  RUN_TEST(test_raw_timestamp_and_length_do_not_shift_arguments);
  RUN_TEST(test_uint64_decimal_supports_full_range);
  RUN_TEST(test_log_record_remains_active_across_partial_writes);
  RUN_TEST(test_log_records_only_change_at_newline_boundaries);
  RUN_TEST(test_log_queue_full_prefers_critical_over_verbose);
  RUN_TEST(test_log_queue_rejects_oversize_without_overwrite);
}

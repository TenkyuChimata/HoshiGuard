#include <unity.h>

#include <string.h>

#include "bridge_protocol.h"
#include "fixtures/real_fixture_registry.h"
#include "fixtures/real/real_capture_arrivals.h"

namespace {
const RealFixture &bridgeFixture(const char *name) {
  for (size_t i = 0U; i < kRealFixtureCount; ++i)
    if (strcmp(kRealFixtures[i].name, name) == 0) return kRealFixtures[i];
  return kRealFixtures[0];
}

bridge::Header header(bridge::FrameType type = bridge::FrameType::L1sFrame,
                      uint16_t length = 0U, uint32_t sequence = 1U) {
  bridge::Header value = {type, length, 0U, sequence, 123456789ULL};
  return value;
}
}

void test_bridge_crc32_iso_hdlc_check_vector() {
  static const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL,
                          bridge::crc32IsoHdlc(check, sizeof(check)));
}

void test_bridge_cobs_empty_and_zero_bytes() {
  uint8_t encoded[16], decoded[16]; size_t encodedLength = 0U, decodedLength = 0U;
  TEST_ASSERT_TRUE(bridge::cobsEncode(0, 0U, encoded, sizeof(encoded), encodedLength));
  TEST_ASSERT_EQUAL_UINT32(1U, encodedLength);
  TEST_ASSERT_EQUAL_HEX8(1U, encoded[0]);
  TEST_ASSERT_TRUE(bridge::cobsDecode(encoded, encodedLength, decoded,
                                      sizeof(decoded), decodedLength));
  TEST_ASSERT_EQUAL_UINT32(0U, decodedLength);
  const uint8_t input[] = {0U, 1U, 0U, 2U, 0U};
  TEST_ASSERT_TRUE(bridge::cobsEncode(input, sizeof(input), encoded,
                                      sizeof(encoded), encodedLength));
  TEST_ASSERT_TRUE(bridge::cobsDecode(encoded, encodedLength, decoded,
                                      sizeof(decoded), decodedLength));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(input, decoded, sizeof(input));
}

void test_bridge_cobs_invalid_and_truncated_are_rejected() {
  uint8_t decoded[8]; size_t length = 0U;
  const uint8_t zero[] = {0U};
  const uint8_t beyond[] = {4U, 1U};
  TEST_ASSERT_FALSE(bridge::cobsDecode(zero, sizeof(zero), decoded,
                                       sizeof(decoded), length));
  TEST_ASSERT_FALSE(bridge::cobsDecode(beyond, sizeof(beyond), decoded,
                                       sizeof(decoded), length));
}

void test_bridge_logical_header_and_version_round_trip() {
  const uint8_t payload[] = {1U, 0U, 2U, 3U};
  uint8_t logical[64]; size_t length = 0U;
  TEST_ASSERT_TRUE(bridge::encodeLogicalFrame(
      header(bridge::FrameType::Hello, sizeof(payload), 7U), payload,
      logical, sizeof(logical), length));
  bridge::DecodedFrame decoded = {};
  TEST_ASSERT_TRUE(bridge::decodeLogicalFrame(logical, length, decoded));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bridge::FrameType::Hello),
                          static_cast<uint8_t>(decoded.header.frameType));
  TEST_ASSERT_EQUAL_UINT32(7U, decoded.header.sequence);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, decoded.payload, sizeof(payload));
  logical[2] = 2U;
  TEST_ASSERT_FALSE(bridge::decodeLogicalFrame(logical, length, decoded));
}

void test_bridge_crc_failure_drops_only_current_frame() {
  uint8_t logical[64]; size_t length = 0U;
  const uint8_t payload[] = {7U};
  bridge::encodeLogicalFrame(header(bridge::FrameType::Error, 1U), payload,
                             logical, sizeof(logical), length);
  logical[bridge::kHeaderLength] ^= 1U;
  bridge::DecodedFrame decoded = {};
  TEST_ASSERT_FALSE(bridge::decodeLogicalFrame(logical, length, decoded));
  TEST_ASSERT_TRUE(bridge::encodeLogicalFrame(
      header(bridge::FrameType::Heartbeat, 1U, 2U), payload,
      logical, sizeof(logical), length));
  TEST_ASSERT_TRUE(bridge::decodeLogicalFrame(logical, length, decoded));
}

void test_bridge_maximum_frame_round_trip() {
  static uint8_t payload[bridge::kMaximumLogicalFrame - bridge::kHeaderLength -
                         bridge::kCrcLength];
  static uint8_t wire[bridge::kMaximumEncodedFrame];
  static uint8_t logical[bridge::kMaximumLogicalFrame];
  for (size_t i = 0U; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(i);
  size_t wireLength = 0U;
  TEST_ASSERT_TRUE(bridge::encodeWireFrame(
      header(bridge::FrameType::Status, sizeof(payload)), payload,
      wire, sizeof(wire), wireLength));
  TEST_ASSERT_EQUAL_HEX8(0U, wire[wireLength - 1U]);
  size_t logicalLength = 0U;
  TEST_ASSERT_TRUE(bridge::cobsDecode(wire, wireLength - 1U, logical,
                                      sizeof(logical), logicalLength));
  bridge::DecodedFrame decoded = {};
  TEST_ASSERT_TRUE(bridge::decodeLogicalFrame(logical, logicalLength, decoded));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, decoded.payload, sizeof(payload));
}

void test_bridge_real_full_ubx_is_retained_in_l1s_payload() {
  const RealFixture &real = bridgeFixture("qzss_l1s_mt44_pab53");
  uint8_t payload[20U + 64U] = {};
  bridge::putU16Le(payload + 12U, static_cast<uint16_t>(real.length));
  memcpy(payload + 20U, real.data, real.length);
  uint8_t logical[160]; size_t length = 0U;
  TEST_ASSERT_TRUE(bridge::encodeLogicalFrame(
      header(bridge::FrameType::L1sFrame,
             static_cast<uint16_t>(20U + real.length)), payload,
      logical, sizeof(logical), length));
  bridge::DecodedFrame decoded = {};
  TEST_ASSERT_TRUE(bridge::decodeLogicalFrame(logical, length, decoded));
  TEST_ASSERT_EQUAL_UINT16(real.length, bridge::getU16Le(decoded.payload + 12U));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(real.data, decoded.payload + 20U, real.length);
}

void test_bridge_sequence_rollover_is_preserved() {
  uint8_t logical[48]; size_t length = 0U; bridge::DecodedFrame decoded = {};
  TEST_ASSERT_TRUE(bridge::encodeLogicalFrame(
      header(bridge::FrameType::Heartbeat, 0U, 0xFFFFFFFFUL), 0,
      logical, sizeof(logical), length));
  TEST_ASSERT_TRUE(bridge::decodeLogicalFrame(logical, length, decoded));
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFUL, decoded.header.sequence);
  TEST_ASSERT_TRUE(bridge::encodeLogicalFrame(
      header(bridge::FrameType::Heartbeat, 0U, 0U), 0,
      logical, sizeof(logical), length));
  TEST_ASSERT_TRUE(bridge::decodeLogicalFrame(logical, length, decoded));
  TEST_ASSERT_EQUAL_UINT32(0U, decoded.header.sequence);
}

void test_bridge_queue_reserves_l1s_slots() {
  bridge::RecordQueue<6U, 32U, 3U> queue;
  const uint8_t status[] = {1U, 0U};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(bridge::EnqueueResult::Queued),
      static_cast<int>(queue.enqueue(bridge::Priority::Status, status, sizeof(status))));
  queue.enqueue(bridge::Priority::Status, status, sizeof(status));
  queue.enqueue(bridge::Priority::Status, status, sizeof(status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(bridge::EnqueueResult::Dropped),
      static_cast<int>(queue.enqueue(bridge::Priority::Status, status, sizeof(status))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(bridge::EnqueueResult::Queued),
      static_cast<int>(queue.enqueue(bridge::Priority::L1s, status, sizeof(status))));
}

void test_bridge_active_record_is_atomic_under_partial_output() {
  bridge::RecordQueue<4U, 32U, 2U> queue;
  const uint8_t l1s[] = {1U, 2U, 3U, 0U};
  const uint8_t critical[] = {9U, 8U, 0U};
  queue.enqueue(bridge::Priority::L1s, l1s, sizeof(l1s));
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_FALSE(queue.advance(2U));
  queue.enqueue(bridge::Priority::Critical, critical, sizeof(critical));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(l1s + 2U, queue.frontData(), 2U);
  TEST_ASSERT_TRUE(queue.advance(2U));
  TEST_ASSERT_TRUE(queue.activateFront());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(critical, queue.frontData(), sizeof(critical));
}

void test_bridge_priority_evicts_status_not_l1s() {
  bridge::RecordQueue<3U, 16U, 1U> queue;
  const uint8_t record[] = {1U, 0U};
  queue.enqueue(bridge::Priority::L1s, record, sizeof(record));
  queue.enqueue(bridge::Priority::Status, record, sizeof(record));
  queue.enqueue(bridge::Priority::Status, record, sizeof(record));
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(bridge::EnqueueResult::Dropped),
      static_cast<int>(queue.enqueue(bridge::Priority::Critical, record, sizeof(record))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(bridge::Priority::Status),
                        static_cast<int>(queue.lastEvictedPriority()));
}

void test_bridge_one_hour_real_rate_simulation_has_zero_l1s_drop() {
  bridge::RecordQueue<7U, 128U, 4U> queue;
  uint8_t l1s[104]; memset(l1s, 0xA5, sizeof(l1s)); l1s[103] = 0U;
  uint32_t dropped = 0U;
  uint64_t previous = kRealL1sArrivalTimestampUs[0];
  for (uint32_t hour = 0U; hour < 12U; ++hour) {
    for (size_t i = 0U; i < kRealL1sArrivalCount; ++i) {
      const uint64_t delta = i == 0U ? 1000000ULL :
          kRealL1sArrivalTimestampUs[i] - previous;
      size_t budget = static_cast<size_t>((delta * 11520ULL) / 1000000ULL);
      while (budget != 0U && queue.activateFront()) {
        const size_t amount = queue.frontRemaining() < budget ?
                              queue.frontRemaining() : budget;
        queue.advance(amount); budget -= amount;
      }
      if (queue.enqueue(bridge::Priority::L1s, l1s, sizeof(l1s)) ==
          bridge::EnqueueResult::Dropped) ++dropped;
      if ((i % 20U) == 0U)
        (void)queue.enqueue(bridge::Priority::Status, l1s, sizeof(l1s));
      previous = kRealL1sArrivalTimestampUs[i];
    }
    previous = kRealL1sArrivalTimestampUs[0];
  }
  TEST_ASSERT_EQUAL_UINT32(0U, dropped);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(7U, queue.highWatermark());
}

void test_bridge_115200_sustained_budget_is_sufficient() {
  const uint64_t duration =
      kRealL1sArrivalTimestampUs[kRealL1sArrivalCount - 1U] -
      kRealL1sArrivalTimestampUs[0];
  const uint64_t average =
      (kRealL1sArrivalCount * 104ULL * 1000000ULL) / duration;
  TEST_ASSERT_LESS_THAN_UINT64(11520ULL, average);
}

void run_v025_tests() {
  RUN_TEST(test_bridge_crc32_iso_hdlc_check_vector);
  RUN_TEST(test_bridge_cobs_empty_and_zero_bytes);
  RUN_TEST(test_bridge_cobs_invalid_and_truncated_are_rejected);
  RUN_TEST(test_bridge_logical_header_and_version_round_trip);
  RUN_TEST(test_bridge_crc_failure_drops_only_current_frame);
  RUN_TEST(test_bridge_maximum_frame_round_trip);
  RUN_TEST(test_bridge_real_full_ubx_is_retained_in_l1s_payload);
  RUN_TEST(test_bridge_sequence_rollover_is_preserved);
  RUN_TEST(test_bridge_queue_reserves_l1s_slots);
  RUN_TEST(test_bridge_active_record_is_atomic_under_partial_output);
  RUN_TEST(test_bridge_priority_evicts_status_not_l1s);
  RUN_TEST(test_bridge_one_hour_real_rate_simulation_has_zero_l1s_drop);
  RUN_TEST(test_bridge_115200_sustained_budget_is_sufficient);
}

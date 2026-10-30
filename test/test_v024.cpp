#include <unity.h>

#include <string.h>

#include "fixtures/real_fixture_registry.h"
#include "fixtures/real/real_capture_arrivals.h"
#include "gnss_uart_diag.h"
#include "log_arbiter.h"
#include "raw_capture.h"
#include "startup_metadata.h"
#include "time_sync.h"
#include "ubx_dispatcher.h"

namespace {
const RealFixture &fixture(const char *name) {
  for (size_t i = 0U; i < kRealFixtureCount; ++i)
    if (strcmp(kRealFixtures[i].name, name) == 0) return kRealFixtures[i];
  return kRealFixtures[0];
}

void assertRealL1s(const char *name, uint8_t pab, uint8_t mt) {
  const RealFixture &value = fixture(name);
  UbxParser parser;
  UbxFrameDispatcher dispatcher;
  UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result,
                 1234567ULL, 9U);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, result.sfrbxPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, result.l1sFrames);
  TEST_ASSERT_TRUE(result.lastFrame.validPab);
  TEST_ASSERT_TRUE(result.lastFrame.validCrc);
  TEST_ASSERT_EQUAL_UINT8(9U, result.lastFrame.numWords);
  TEST_ASSERT_TRUE(result.lastFrame.hasExtraWord);
  TEST_ASSERT_EQUAL_HEX8(pab, result.lastFrame.pab);
  TEST_ASSERT_EQUAL_UINT8(mt, result.lastFrame.messageType);
  TEST_ASSERT_EQUAL_HEX32(qzssL1sReceivedCrc(result.lastFrame.raw),
                          qzssL1sCrc24(result.lastFrame.raw));
}

void checksumPacket(uint8_t *packet, size_t length) {
  uint8_t a = 0U, b = 0U;
  UbxParser::checksum(packet + 2U, length - 4U, a, b);
  packet[length - 2U] = a;
  packet[length - 1U] = b;
}

UbxTimTp limitedTiming(uint32_t towMs) {
  UbxTimTp value = {};
  value.towMs = towMs;
  value.week = 2400U;
  value.timeBaseUtc = true;
  value.utcAvailable = true;
  value.tpLocked = true;
  value.qErrValid = false;
  return value;
}
}  // namespace

void test_real_mt43_pab53_end_to_end() { assertRealL1s("qzss_l1s_mt43_pab53", 0x53U, 43U); }
void test_real_mt43_pab9a_end_to_end() { assertRealL1s("qzss_l1s_mt43_pab9a", 0x9AU, 43U); }
void test_real_mt43_pabc6_end_to_end() { assertRealL1s("qzss_l1s_mt43_pabc6", 0xC6U, 43U); }
void test_real_mt44_pab53_end_to_end() { assertRealL1s("qzss_l1s_mt44_pab53", 0x53U, 44U); }
void test_real_mt44_pab9a_end_to_end() { assertRealL1s("qzss_l1s_mt44_pab9a", 0x9AU, 44U); }
void test_real_mt44_pabc6_end_to_end() { assertRealL1s("qzss_l1s_mt44_pabc6", 0xC6U, 44U); }
void test_real_mt47_end_to_end() { assertRealL1s("qzss_l1s_mt47", fixture("qzss_l1s_mt47").pab, 47U); }
void test_real_mt48_end_to_end() { assertRealL1s("qzss_l1s_mt48", fixture("qzss_l1s_mt48").pab, 48U); }
void test_real_mt49_end_to_end() { assertRealL1s("qzss_l1s_mt49", fixture("qzss_l1s_mt49").pab, 49U); }
void test_real_mt50_end_to_end() { assertRealL1s("qzss_l1s_mt50", fixture("qzss_l1s_mt50").pab, 50U); }

void test_real_qzss_l1ca_does_not_enter_l1s() {
  const RealFixture &value = fixture("qzss_l1ca_negative");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().qzssL1caSfrbx);
}

void test_synthetic_non_qzss_does_not_enter_l1s() {
  const RealFixture &value = fixture("non_qzss_negative");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(0U, dispatcher.stats().qzssSfrbx);
}

void test_synthetic_checksum_corruption_is_rejected_by_ubx_parser() {
  const RealFixture &value = fixture("ubx_checksum_corrupted");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(0U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(1U, parser.stats().checksumErrors);
}

void test_synthetic_sfrbx_length_mismatch_is_safely_rejected() {
  const RealFixture &value = fixture("sfrbx_length_mismatch");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(1U, result.validPackets);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().invalidSfrbxLength);
}

void test_numwords_support_is_explicit_for_eight_and_nine_only() {
  TEST_ASSERT_TRUE(isSupportedQzssL1sWordCount(8U));
  TEST_ASSERT_TRUE(isSupportedQzssL1sWordCount(9U));
  TEST_ASSERT_FALSE(isSupportedQzssL1sWordCount(7U));
  TEST_ASSERT_FALSE(isSupportedQzssL1sWordCount(10U));
}

void test_real_nine_word_payload_length_is_exact() {
  const RealFixture &value = fixture("qzss_l1s_mt43_pab53");
  const uint16_t payloadLength = static_cast<uint16_t>(
      value.data[4] | static_cast<uint16_t>(value.data[5] << 8));
  TEST_ASSERT_EQUAL_UINT16(44U, payloadLength);
  TEST_ASSERT_EQUAL_UINT32(payloadLength + 8U, value.length);
  TEST_ASSERT_EQUAL_UINT8(9U, value.data[10]);
}

void test_legacy_eight_word_path_has_no_extra_word() {
  const RealFixture &source = fixture("qzss_l1s_mt43_pab53");
  uint8_t payload[40]; memcpy(payload, source.data + 6U, sizeof(payload));
  payload[4] = 8U;
  QzssL1sFrame frame = {};
  TEST_ASSERT_TRUE(extractQzssL1s(payload, sizeof(payload), frame));
  TEST_ASSERT_EQUAL_UINT8(8U, frame.numWords);
  TEST_ASSERT_FALSE(frame.hasExtraWord);
}

void test_seven_word_qzss_l1s_is_rejected_as_word_count() {
  uint8_t payload[36] = {};
  payload[0] = 5U; payload[2] = 1U; payload[4] = 7U; payload[6] = 2U;
  uint8_t packet[48];
  const size_t length = UbxParser::buildPacket(0x02U, 0x13U, payload,
      sizeof(payload), packet, sizeof(packet));
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(packet, length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().invalidWordCount);
}

void test_six_padding_bits_do_not_change_crc_or_received_crc() {
  const RealFixture &source = fixture("qzss_l1s_mt44_pab53");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(source.data, source.length, parser, dispatcher, result);
  uint8_t changed[QZSS_L1S_RAW_BYTES];
  memcpy(changed, result.lastFrame.raw, sizeof(changed));
  changed[31] ^= 0x3FU;
  TEST_ASSERT_EQUAL_HEX32(qzssL1sCrc24(result.lastFrame.raw),
                          qzssL1sCrc24(changed));
  TEST_ASSERT_EQUAL_HEX32(qzssL1sReceivedCrc(result.lastFrame.raw),
                          qzssL1sReceivedCrc(changed));
}

void test_real_ninth_word_is_retained_but_not_in_250_bits() {
  const RealFixture &value = fixture("qzss_l1s_mt43_pab53");
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(value.data, value.length, parser, dispatcher, result);
  TEST_ASSERT_TRUE(result.lastFrame.hasExtraWord);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(value.data + 46U,
                                result.lastFrame.extraWordUbx, 4U);
  BitReader reader(result.lastFrame.raw, QZSS_L1S_VALID_BITS);
  uint32_t bit = 0U;
  TEST_ASSERT_FALSE(reader.read(250U, 1U, bit));
}

void test_ten_word_qzss_sig1_is_rejected() {
  const RealFixture &source = fixture("qzss_l1ca_negative");
  uint8_t packet[64];
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(packet), source.length);
  memcpy(packet, source.data, source.length);
  packet[8] = 1U;  // 実受信の 10 word payload を保持したまま sigId=1 とする。
  checksumPacket(packet, source.length);
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(packet, source.length, parser, dispatcher, result);
  TEST_ASSERT_EQUAL_UINT32(0U, result.l1sFrames);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().invalidWordCount);
}

void test_same_real_content_from_different_sv_is_deduplicated() {
  const RealFixture &source = fixture("qzss_l1s_mt44_pab53");
  uint8_t packet[64]; memcpy(packet, source.data, source.length);
  UbxParser parser; UbxFrameDispatcher dispatcher; UbxInjectionResult result = {};
  injectUbxBytes(source.data, source.length, parser, dispatcher, result, 100U);
  packet[7] = static_cast<uint8_t>(packet[7] == 7U ? 2U : 7U);
  checksumPacket(packet, source.length);
  injectUbxBytes(packet, source.length, parser, dispatcher, result, 200U);
  TEST_ASSERT_EQUAL_UINT32(1U, dispatcher.stats().duplicateFrames);
}

void test_compact_l1s_record_contains_replayable_complete_ubx() {
  const RealFixture &source = fixture("qzss_l1s_mt44_pabc6");
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

void test_l1s_115200_average_bandwidth_budget() {
  const uint64_t duration = kRealL1sArrivalTimestampUs[kRealL1sArrivalCount - 1U] -
                            kRealL1sArrivalTimestampUs[0];
  const uint64_t bytesPerSecond =
      (kRealL1sArrivalCount * 220ULL * 1000000ULL) / duration;
  TEST_ASSERT_LESS_THAN_UINT64(11520ULL, bytesPerSecond);
}

void test_pps_first_edge_is_not_abnormal() {
  PpsIntervalTracker tracker; tracker.observe(1U, 100U);
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.stats().abnormalInterval);
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.stats().lost);
}

void test_two_second_pps_startup_interval_increments_history() {
  PpsIntervalTracker tracker;
  tracker.observe(1U, 100U); tracker.observe(2U, 2000100U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().abnormalInterval);
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.stats().consecutiveGood);
}

void test_pps_history_recovers_current_stable_state() {
  PpsIntervalTracker tracker;
  tracker.observe(1U, 100U); tracker.observe(2U, 2000100U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  for (uint32_t sequence = 3U; sequence <= 5U; ++sequence)
    tracker.observe(sequence, 2000100U + (sequence - 2U) * 1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::Stable),
      static_cast<int>(evaluatePpsActivity(tracker.stats(), 5000100U, 5100100U)));
}

void test_pps_recovery_never_clears_historical_totals() {
  PpsIntervalTracker tracker;
  tracker.observe(1U, 100U); tracker.observe(2U, 2000100U);
  for (uint32_t i = 3U; i <= 12U; ++i)
    tracker.observe(i, 2000100U + (i - 2U) * 1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().abnormalInterval);
  TEST_ASSERT_EQUAL_UINT32(10U, tracker.stats().consecutiveGood);
}

void test_pps_health_uses_wrap_safe_age() {
  PpsIntervalTracker tracker;
  const uint32_t first = 0xFFF00000UL;
  tracker.observe(1U, first);
  tracker.observe(2U, first + static_cast<uint32_t>(1000000U));
  tracker.observe(3U, first + static_cast<uint32_t>(2000000U));
  tracker.observe(4U, first + static_cast<uint32_t>(3000000U));
  const uint32_t last = first + static_cast<uint32_t>(3000000U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::Stable),
      static_cast<int>(evaluatePpsActivity(tracker.stats(), last,
                                            last + 100000UL)));
}

void test_new_pps_anomaly_immediately_downgrades() {
  PpsIntervalTracker tracker;
  for (uint32_t i = 1U; i <= 4U; ++i) tracker.observe(i, i * 1000000U);
  tracker.observe(5U, 5500000U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::Unstable),
      static_cast<int>(evaluatePpsActivity(tracker.stats(), 5500000U, 5600000U)));
}

void test_pps_timeout_is_transient_after_stability() {
  PpsIntervalTracker tracker;
  for (uint32_t i = 1U; i <= 4U; ++i) tracker.observe(i, i * 1000000U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::TransientOrMissing),
      static_cast<int>(evaluatePpsActivity(tracker.stats(), 4000000U, 6000001U)));
}

void test_real_capture_pps_sequence_is_stable() {
  PpsIntervalTracker tracker;
  for (size_t i = 0U; i < kRealPpsTimestampCount; ++i)
    tracker.observe(static_cast<uint32_t>(i + 1U), kRealPpsTimestampUs[i]);
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.stats().abnormalInterval);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::Stable),
      static_cast<int>(evaluatePpsActivity(
          tracker.stats(), kRealPpsTimestampUs[kRealPpsTimestampCount - 1U],
          kRealPpsTimestampUs[kRealPpsTimestampCount - 1U] + 1000U)));
}

void test_locked_utc_without_qerr_is_limited_not_confirmed() {
  PpsUtcAssociator associator; associator.setTimeGridKnown(true);
  for (uint32_t i = 1U; i <= 4U; ++i) {
    associator.observeTimTp(limitedTiming(i * 1000U), i * 1000000U - 100000U);
    associator.observePps(i, i * 1000000U);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::LockedUtcLimited),
                        static_cast<int>(associator.quality()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeConfirmReason::QErrInvalid),
                        static_cast<int>(associator.confirmReason()));
  TEST_ASSERT_EQUAL_STRING("qerr_unavailable",
                           timeConfirmReasonName(associator.confirmReason()));
  TEST_ASSERT_EQUAL_UINT32(4U, associator.stats().timTpLimited);
  TEST_ASSERT_EQUAL_UINT32(0U, associator.stats().confirmedCount);
}

void test_limited_time_downgrades_when_pps_is_lost() {
  PpsUtcAssociator associator; associator.setTimeGridKnown(true);
  for (uint32_t i = 1U; i <= 4U; ++i) {
    associator.observeTimTp(limitedTiming(i * 1000U), i * 1000000U - 100000U);
    associator.observePps(i, i * 1000000U);
  }
  associator.observeTimTp(limitedTiming(5000U), 5900000U);
  associator.observePps(6U, 6000000U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::Degraded),
                        static_cast<int>(associator.quality()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeConfirmReason::PpsLost),
                        static_cast<int>(associator.confirmReason()));
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

void run_v024_tests() {
  RUN_TEST(test_real_mt43_pab53_end_to_end);
  RUN_TEST(test_real_mt43_pab9a_end_to_end);
  RUN_TEST(test_real_mt43_pabc6_end_to_end);
  RUN_TEST(test_real_mt44_pab53_end_to_end);
  RUN_TEST(test_real_mt44_pab9a_end_to_end);
  RUN_TEST(test_real_mt44_pabc6_end_to_end);
  RUN_TEST(test_real_mt47_end_to_end);
  RUN_TEST(test_real_mt48_end_to_end);
  RUN_TEST(test_real_mt49_end_to_end);
  RUN_TEST(test_real_mt50_end_to_end);
  RUN_TEST(test_real_qzss_l1ca_does_not_enter_l1s);
  RUN_TEST(test_synthetic_non_qzss_does_not_enter_l1s);
  RUN_TEST(test_synthetic_checksum_corruption_is_rejected_by_ubx_parser);
  RUN_TEST(test_synthetic_sfrbx_length_mismatch_is_safely_rejected);
  RUN_TEST(test_numwords_support_is_explicit_for_eight_and_nine_only);
  RUN_TEST(test_real_nine_word_payload_length_is_exact);
  RUN_TEST(test_legacy_eight_word_path_has_no_extra_word);
  RUN_TEST(test_seven_word_qzss_l1s_is_rejected_as_word_count);
  RUN_TEST(test_six_padding_bits_do_not_change_crc_or_received_crc);
  RUN_TEST(test_real_ninth_word_is_retained_but_not_in_250_bits);
  RUN_TEST(test_ten_word_qzss_sig1_is_rejected);
  RUN_TEST(test_same_real_content_from_different_sv_is_deduplicated);
  RUN_TEST(test_compact_l1s_record_contains_replayable_complete_ubx);
  RUN_TEST(test_l1s_reserved_slots_reject_verbose_before_l1s);
  RUN_TEST(test_partial_l1s_output_remains_atomic_under_queue_pressure);
  RUN_TEST(test_congested_queue_drains_to_empty);
  RUN_TEST(test_real_arrival_pressure_has_zero_l1s_drops_at_115200);
  RUN_TEST(test_l1s_115200_average_bandwidth_budget);
  RUN_TEST(test_pps_first_edge_is_not_abnormal);
  RUN_TEST(test_two_second_pps_startup_interval_increments_history);
  RUN_TEST(test_pps_history_recovers_current_stable_state);
  RUN_TEST(test_pps_recovery_never_clears_historical_totals);
  RUN_TEST(test_pps_health_uses_wrap_safe_age);
  RUN_TEST(test_new_pps_anomaly_immediately_downgrades);
  RUN_TEST(test_pps_timeout_is_transient_after_stability);
  RUN_TEST(test_real_capture_pps_sequence_is_stable);
  RUN_TEST(test_locked_utc_without_qerr_is_limited_not_confirmed);
  RUN_TEST(test_limited_time_downgrades_when_pps_is_lost);
  RUN_TEST(test_privacy_metadata_warns_when_nav_pvt_raw_enabled);
  RUN_TEST(test_privacy_metadata_is_safe_by_default_shape);
}

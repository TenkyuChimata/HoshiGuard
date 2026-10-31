#include "test_support.h"
#include "time_sync.h"
#include "gnss_uart_diag.h"
#include "fixtures/real/real_capture_arrivals.h"

namespace {
void writeLe16(uint8_t *p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

UbxTimTp makeTiming(uint32_t towMs) {
  UbxTimTp value = {};
  value.towMs = towMs;
  value.week = 2400U;
  value.flags = 0x03U;
  value.refInfo = 0x40U;
  value.timeBaseUtc = true;
  value.utcAvailable = true;
  value.qErrValid = true;
  value.tpLocked = true;
  return value;
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
}

void test_single_pps_is_observed_once() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::ObservedOnce),
      static_cast<int>(evaluatePpsActivity(1U, 1000000U, 1100000U, 0U, 0U)));

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::Stable),
      static_cast<int>(evaluatePpsActivity(3U, 3000000U, 3100000U, 0U, 0U)));

  }

  {

  TEST_ASSERT_EQUAL_INT(static_cast<int>(PpsActivity::TransientOrMissing),
      static_cast<int>(evaluatePpsActivity(1U, 100000U, 2100000U, 0U, 0U)));

  }
}

void test_pps_micros_wrap_is_safe() {
  PpsIntervalTracker tracker;
  const uint32_t first = 0xFFF00000UL;
  tracker.observe(1U, first);
  tracker.observe(2U, first + 1000000UL);
  TEST_ASSERT_EQUAL_UINT32(1000000U, tracker.stats().lastIntervalMicros);
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.stats().abnormalInterval);
}

void test_pps_loss_and_abnormal_period() {
  PpsIntervalTracker tracker;
  tracker.observe(1U, 100U);
  tracker.observe(3U, 2100100U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().abnormalInterval);
}

void test_nav_pvt_pps_association_is_tentative() {
  uint8_t payload[92] = {};
  writeLe32(payload, 123000U);
  payload[4] = 0xEA; payload[5] = 0x07;  // 2026
  payload[6] = 7; payload[7] = 14;
  payload[8] = 12; payload[9] = 34; payload[10] = 56;
  payload[11] = 0x07;
  PpsUtcAssociator associator;
  associator.observePps(42U, 0xFFFFFF00UL);
  NavPvtRecord record = {};
  TEST_ASSERT_TRUE(associator.observeNavPvt(payload, sizeof(payload),
                                            0x00010000UL, record));
  TEST_ASSERT_TRUE(record.utcValid);
  TEST_ASSERT_TRUE(record.ppsAssociated);
  TEST_ASSERT_EQUAL_UINT32(42U, record.nearestPpsSequence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::UtcPpsTentative),
                        static_cast<int>(record.quality));
}

void test_tim_tp_valid_packet_parsing() {
  uint8_t payload[16] = {};
  writeLe32(payload, 345678U);
  writeLe32(payload + 4U, 0x80000000UL);
  writeLe32(payload + 8U, static_cast<uint32_t>(-125));
  writeLe16(payload + 12U, 2400U);
  payload[14] = 0x03U;
  payload[15] = 0x40U;
  UbxTimTp result = {};
  TEST_ASSERT_TRUE(parseUbxTimTp(payload, sizeof(payload), result));
  TEST_ASSERT_EQUAL_UINT32(345678U, result.towMs);
  TEST_ASSERT_EQUAL_INT32(-125, result.qErrPs);
  TEST_ASSERT_EQUAL_UINT16(2400U, result.week);
}

void test_tim_tp_wrong_length_rejected() {
  uint8_t payload[16] = {};
  UbxTimTp result = {};
  TEST_ASSERT_FALSE(parseUbxTimTp(payload, 15U, result));
  TEST_ASSERT_FALSE(parseUbxTimTp(0, 16U, result));
}

void test_tim_tp_flags_parsing() {
  uint8_t payload[16] = {};
  payload[14] = 0x1FU;  // UTC、利用可能、RAIM=3、qErr 無効、locked。
  payload[15] = 0x47U;
  UbxTimTp result = {};
  TEST_ASSERT_TRUE(parseUbxTimTp(payload, sizeof(payload), result));
  TEST_ASSERT_TRUE(result.timeBaseUtc);
  TEST_ASSERT_TRUE(result.utcAvailable);
  TEST_ASSERT_EQUAL_UINT8(3U, result.raim);
  TEST_ASSERT_FALSE(result.qErrValid);
  TEST_ASSERT_TRUE(result.tpLocked);
  TEST_ASSERT_EQUAL_UINT8(7U, result.timeRefGnss);
  TEST_ASSERT_EQUAL_UINT8(4U, result.utcStandard);
}

void test_tim_tp_matches_next_pps() {
  PpsUtcAssociator associator;
  associator.setTimeGridKnown(true);
  associator.observeTimTp(makeTiming(1000U), 900000U);
  associator.observePps(1U, 1000000U);
  TEST_ASSERT_TRUE(associator.lastTimTp().matched);
  TEST_ASSERT_EQUAL_UINT32(1U, associator.lastTimTp().matchedPpsSequence);
  TEST_ASSERT_EQUAL_UINT32(1U, associator.stats().timTpMatched);
}

void test_tim_tp_ambiguous_match_never_confirms() {
  PpsUtcAssociator associator;
  associator.setTimeGridKnown(true);
  associator.observeTimTp(makeTiming(1000U), 800000U);
  associator.observeTimTp(makeTiming(1000U), 900000U);
  associator.observePps(1U, 1000000U);
  TEST_ASSERT_TRUE(associator.lastTimTp().ambiguous);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(TimeQuality::UtcPpsConfirmed),
                        static_cast<int>(associator.quality()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeConfirmReason::AmbiguousMatch),
                        static_cast<int>(associator.confirmReason()));
}

void test_tim_tp_requires_three_continuous_matches() {
  PpsUtcAssociator associator;
  associator.setTimeGridKnown(true);
  for (uint32_t i = 1U; i <= 3U; ++i) {
    associator.observeTimTp(makeTiming(i * 1000U), i * 1000000U - 100000U);
    associator.observePps(i, i * 1000000U);
    if (i < 3U)
      TEST_ASSERT_NOT_EQUAL(static_cast<int>(TimeQuality::UtcPpsConfirmed),
                            static_cast<int>(associator.quality()));
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::UtcPpsConfirmed),
                        static_cast<int>(associator.quality()));
  TEST_ASSERT_EQUAL_UINT8(3U, associator.consecutiveTimTpMatches());
}

void test_pps_loss_downgrades_confirmed_time() {
  PpsUtcAssociator associator;
  associator.setTimeGridKnown(true);
  for (uint32_t i = 1U; i <= 3U; ++i) {
    associator.observeTimTp(makeTiming(i * 1000U), i * 1000000U - 100000U);
    associator.observePps(i, i * 1000000U);
  }
  associator.observeTimTp(makeTiming(4000U), 4900000U);
  associator.observePps(5U, 5000000U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::Degraded),
                        static_cast<int>(associator.quality()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeConfirmReason::PpsLost),
                        static_cast<int>(associator.confirmReason()));
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

  {

  PpsIntervalTracker tracker;
  tracker.observe(1U, 100U); tracker.observe(2U, 2000100U);
  for (uint32_t i = 3U; i <= 12U; ++i)
    tracker.observe(i, 2000100U + (i - 2U) * 1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().lost);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.stats().abnormalInterval);
  TEST_ASSERT_EQUAL_UINT32(10U, tracker.stats().consecutiveGood);

  }
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

void run_time_tests() {
  RUN_TEST(test_single_pps_is_observed_once);
  RUN_TEST(test_pps_micros_wrap_is_safe);
  RUN_TEST(test_pps_loss_and_abnormal_period);
  RUN_TEST(test_nav_pvt_pps_association_is_tentative);
  RUN_TEST(test_tim_tp_valid_packet_parsing);
  RUN_TEST(test_tim_tp_wrong_length_rejected);
  RUN_TEST(test_tim_tp_flags_parsing);
  RUN_TEST(test_tim_tp_matches_next_pps);
  RUN_TEST(test_tim_tp_ambiguous_match_never_confirms);
  RUN_TEST(test_tim_tp_requires_three_continuous_matches);
  RUN_TEST(test_pps_loss_downgrades_confirmed_time);
  RUN_TEST(test_pps_first_edge_is_not_abnormal);
  RUN_TEST(test_two_second_pps_startup_interval_increments_history);
  RUN_TEST(test_pps_history_recovers_current_stable_state);
  RUN_TEST(test_pps_health_uses_wrap_safe_age);
  RUN_TEST(test_new_pps_anomaly_immediately_downgrades);
  RUN_TEST(test_pps_timeout_is_transient_after_stability);
  RUN_TEST(test_real_capture_pps_sequence_is_stable);
  RUN_TEST(test_locked_utc_without_qerr_is_limited_not_confirmed);
  RUN_TEST(test_limited_time_downgrades_when_pps_is_lost);
}

#include <unity.h>

#include <string.h>

#include "fixtures/real_fixture_registry.h"
#include "gnss_self_test.h"
#include "raw_capture.h"
#include "tim_tp.h"
#include "time_sync.h"

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

ConfigEvidence verifiedEvidence(uint32_t key, uint32_t value,
                                ConfigRequirement requirement) {
  ConfigEvidence evidence = {};
  evidence.key = key;
  evidence.targetValue = value;
  evidence.currentValue = value;
  evidence.requirement = requirement;
  evidence.result = ConfigResult::Verified;
  evidence.hasCurrentValue = true;
  return evidence;
}
}  // namespace

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

void test_self_test_pass_requires_verified_evidence() {
  ConfigEvidence qzss = verifiedEvidence(CFG_SIGNAL_QZSS_ENA, 1U,
                                          ConfigRequirement::Required);
  ConfigEvidence l1s = verifiedEvidence(CFG_SIGNAL_QZSS_L1S_ENA, 1U,
                                         ConfigRequirement::Required);
  ConfigEvidence ubx = verifiedEvidence(CFG_UART1OUTPROT_UBX, 1U,
                                         ConfigRequirement::Required);
  ConfigEvidence sfrbx = verifiedEvidence(CFG_MSGOUT_RXM_SFRBX_UART1, 1U,
                                           ConfigRequirement::Required);
  ConfigEvidence nav = verifiedEvidence(CFG_MSGOUT_NAV_PVT_UART1, 1U,
                                         ConfigRequirement::Optional);
  ConfigEvidence tp = verifiedEvidence(CFG_TP_TP1_ENA, 1U,
                                        ConfigRequirement::Optional);
  ConfigEvidence grid = verifiedEvidence(CFG_TP_TIMEGRID_TP1, 0U,
                                          ConfigRequirement::Optional);
  GnssSelfTestEvidence evidence = {};
  evidence.uartBytesObserved = evidence.validUbxObserved = true;
  evidence.monVerObserved = evidence.sfrbxObserved = true;
  evidence.navPvtObserved = evidence.ppsObserved = true;
  evidence.timePulseAllVerified = true;
  evidence.qzss = &qzss; evidence.qzssL1s = &l1s; evidence.ubxOutput = &ubx;
  evidence.sfrbxOutput = &sfrbx; evidence.navPvtOutput = &nav;
  evidence.timePulseEnable = &tp; evidence.timePulseGrid = &grid;
  const GnssSelfTestReport report = evaluateGnssSelfTest(evidence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SelfTestStatus::Pass),
                        static_cast<int>(report.overall));
}

void test_valset_ack_without_matching_valget_is_not_verified() {
  ConfigEvidence evidence = {};
  evidence.targetValue = 1U;
  evidence.currentValue = 0U;
  evidence.hasCurrentValue = true;
  evidence.valsetAcknowledged = true;
  evidence.result = ConfigResult::Changed;
  TEST_ASSERT_FALSE(configEvidenceVerified(evidence));
  evidence.result = ConfigResult::Verified;
  TEST_ASSERT_FALSE(configEvidenceVerified(evidence));
}

void test_real_fixture_registry_is_populated_from_field_capture() {
  TEST_ASSERT_EQUAL_UINT32(14U, kRealFixtureCount);
}

void run_v021_tests() {
  RUN_TEST(test_tim_tp_valid_packet_parsing);
  RUN_TEST(test_tim_tp_wrong_length_rejected);
  RUN_TEST(test_tim_tp_flags_parsing);
  RUN_TEST(test_tim_tp_matches_next_pps);
  RUN_TEST(test_tim_tp_ambiguous_match_never_confirms);
  RUN_TEST(test_tim_tp_requires_three_continuous_matches);
  RUN_TEST(test_pps_loss_downgrades_confirmed_time);
  RUN_TEST(test_self_test_pass_requires_verified_evidence);
  RUN_TEST(test_valset_ack_without_matching_valget_is_not_verified);
  RUN_TEST(test_real_fixture_registry_is_populated_from_field_capture);
}

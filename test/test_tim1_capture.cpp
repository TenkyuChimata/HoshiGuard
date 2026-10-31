#include <unity.h>

#include "tim1_capture.h"

namespace {
struct Registers {
  uint32_t sr;
  uint16_t ccr;
  uint32_t injectAfterRead;
  bool pending;
  unsigned discards;
  uint32_t status() const { return sr; }
  uint16_t capture() {
    sr &= ~pps_hardware::kCc1If;
    sr |= injectAfterRead;
    return ccr;
  }
  bool edgePending() const { return pending; }
  void discard() { sr &= ~(pps_hardware::kCc1If | pps_hardware::kCc1Of); ++discards; }
};

void test_tim1_ccr_and_update_order_are_independent() {
  // UIF の有無、CCR の周回直前・直後、何回も未処理の overflow を同じ入口で扱う。
  const uint16_t counts[] = {0U, 1U, 32767U, 65535U};
  for (uint16_t count : counts) for (uint32_t uif = 0U; uif <= 1U; ++uif) {
    Registers io = {pps_hardware::kCc1If | uif, count, 0U, false, 0U};
    const uint32_t sample = pps_hardware::readCapture(io);
    TEST_ASSERT_EQUAL_HEX32(pps_hardware::kValid | count, sample);
    TEST_ASSERT_EQUAL_UINT32(uif, io.sr);
    TEST_ASSERT_EQUAL_UINT32(0U, io.discards);
  }
  Registers empty = {1U, 456U, 0U, false, 0U};
  TEST_ASSERT_EQUAL_UINT32(0U, pps_hardware::readCapture(empty));
  TEST_ASSERT_EQUAL_UINT32(1U, empty.sr);
}

void test_tim1_overcapture_and_ccr_read_races_fail_closed() {
  const uint32_t before[] = {pps_hardware::kCc1If | pps_hardware::kCc1Of,
                             pps_hardware::kCc1If, pps_hardware::kCc1If,
                             pps_hardware::kCc1If, pps_hardware::kCc1Of};
  const uint32_t after[] = {0U, pps_hardware::kCc1If, pps_hardware::kCc1Of, 0U, 0U};
  for (unsigned i = 0U; i < 5U; ++i) {
    Registers io = {before[i] | 1U, 123U, after[i], i == 3U, 0U};
    const uint32_t sample = pps_hardware::readCapture(io);
    TEST_ASSERT_EQUAL_UINT32(0U, sample & pps_hardware::kValid);
    TEST_ASSERT_NOT_EQUAL(0U, sample & (pps_hardware::kReadRace | pps_hardware::kOvercapture));
    TEST_ASSERT_EQUAL_UINT32(1U, io.discards);
    TEST_ASSERT_EQUAL_UINT32(1U, io.sr);
  }
}

void test_tim1_modulo_evidence_never_invents_upper_counts() {
  pps_hardware::IntervalEvidence tracker;
  uint16_t counter = 65530U;
  uint32_t sequence = 1U;
  auto out = tracker.observe(sequence, pps_hardware::kValid | counter);
  TEST_ASSERT_TRUE(out.captureValid);
  TEST_ASSERT_FALSE(out.intervalModuloValid);
  // 1 Hz、短間隔、欠落後の長間隔、大量の overflow はすべて下位 16 bit の差だけを返す。
  const uint32_t intervals[] = {1000000U, 1000000U, 5U, 3000000U, 65536U * 1000U + 7U};
  for (uint32_t interval : intervals) {
    counter = static_cast<uint16_t>(counter + interval);
    out = tracker.observe(++sequence, pps_hardware::kValid | counter);
    TEST_ASSERT_TRUE(out.intervalModuloValid);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(interval), out.intervalModuloTicks);
  }
  out = tracker.observe(++sequence, pps_hardware::kOvercapture | counter);
  TEST_ASSERT_FALSE(out.captureValid);
  TEST_ASSERT_FALSE(out.intervalModuloValid);
  out = tracker.observe(++sequence, pps_hardware::kValid | counter);
  TEST_ASSERT_FALSE(out.intervalModuloValid);
  out = tracker.observe(sequence + 2U, pps_hardware::kValid | counter);
  TEST_ASSERT_FALSE(out.intervalModuloValid);
  tracker = pps_hardware::IntervalEvidence();
  out = tracker.observe(1U, pps_hardware::kValid | counter);
  TEST_ASSERT_FALSE(out.intervalModuloValid);
  // 通し番号の周回も unsigned 差で扱う。
  tracker.observe(0xFFFFFFFFUL, pps_hardware::kValid | counter);
  out = tracker.observe(0U, pps_hardware::kValid | counter);
  TEST_ASSERT_TRUE(out.intervalModuloValid);
}
}

void run_tim1_capture_tests() {
  RUN_TEST(test_tim1_ccr_and_update_order_are_independent);
  RUN_TEST(test_tim1_overcapture_and_ccr_read_races_fail_closed);
  RUN_TEST(test_tim1_modulo_evidence_never_invents_upper_counts);
}

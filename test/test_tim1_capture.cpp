#include <unity.h>

#include "tim1_capture.h"

namespace {
void test_tim1_rcc_dividers_use_sysclk_and_timer_doubling() {
  const uint32_t ahbDivisors[] = {1,1,1,1,1,1,1,1,2,4,8,16,64,128,256,512};
  const uint32_t apbDivisors[] = {1,1,1,1,2,4,8,16};
  const uint32_t clocks[] = {8000000U,48000000U,72000000U};
  for (uint32_t clock : clocks) for (unsigned ahb=0U; ahb<16U; ++ahb)
    for (unsigned apb=0U; apb<8U; ++apb) {
      const uint32_t expected=clock/ahbDivisors[ahb]/apbDivisors[apb]*(apb<4U ? 1U : 2U);
      TEST_ASSERT_EQUAL_UINT32(expected,pps_hardware::tim1InputClock(clock,(ahb<<4U)|(apb<<11U)));
    }
  TEST_ASSERT_EQUAL_UINT32(72000000U,pps_hardware::kCounterHz);
  TEST_ASSERT_EQUAL_UINT32(65536U,pps_hardware::kCounterPeriod);
}

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

void test_tim1_exti_before_capture_and_delayed_single_latch() {
  // EXTI が先着した場合、まだラッチされていない CCR を証跡にしない。
  Registers early = {0U,123U,0U,false,0U};
  TEST_ASSERT_EQUAL_UINT32(0U,pps_hardware::readCapture(early));
  // 複数周回分の IRQ 遅延後も、次の捕捉がなければ CCR 下位値は保持される。
  early.sr=pps_hardware::kCc1If|1U;
  TEST_ASSERT_EQUAL_UINT32(pps_hardware::kValid|123U,pps_hardware::readCapture(early));
  // 次の捕捉まで遅延した場合は UIF に関係なく overcapture として破棄する。
  early.sr=pps_hardware::kCc1If|pps_hardware::kCc1Of|1U;
  TEST_ASSERT_EQUAL_UINT32(0U,pps_hardware::readCapture(early)&pps_hardware::kValid);
  TEST_ASSERT_EQUAL_UINT32(1U,early.discards);
}

void test_tim1_modulo_evidence_never_invents_upper_counts() {
  pps_hardware::IntervalEvidence tracker;
  uint16_t counter = 65530U;
  uint32_t sequence = 1U;
  auto out = tracker.observe(sequence, pps_hardware::kValid | counter);
  TEST_ASSERT_TRUE(out.captureValid);
  TEST_ASSERT_FALSE(out.intervalModuloValid);
  // 1 Hz、短間隔、欠落後の長間隔、大量の overflow はすべて下位 16 bit の差だけを返す。
  const uint32_t intervals[] = {72000000U, 72000000U, 5U, 216000000U, 65536U * 1000U + 7U};
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

void test_tim1_72mhz_phases_drift_and_wide_count_aliases() {
  const uint16_t phases[] = {0U,1U,32767U,65530U,65535U};
  // 1 秒、複数秒、単一/複数周回、±100 ppm、32/64 bit 境界も上位値を推定しない。
  const uint64_t intervals[] = {72000000ULL,144000000ULL,720000000ULL,
      65536ULL,65536ULL*3000ULL,1ULL,2ULL,71992800ULL,72007200ULL,
      0xFFFFFFFFULL,0x100000000ULL,0x100000001ULL,0xFFFFFFFFFFFFFFFFULL};
  for (uint16_t phase : phases) for (uint64_t ticks : intervals) {
    pps_hardware::IntervalEvidence tracker;
    tracker.observe(0xFFFFFFFFU,pps_hardware::kValid|phase);
    const uint16_t next=static_cast<uint16_t>(phase+static_cast<uint16_t>(ticks));
    const auto out=tracker.observe(0U,pps_hardware::kValid|next);
    TEST_ASSERT_TRUE(out.captureValid && out.intervalModuloValid);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(ticks),out.intervalModuloTicks);
  }
  // 一秒と一秒+65536 tick は完全に同じ証跡となる。完全間隔への変換は禁止する。
  TEST_ASSERT_EQUAL_UINT16(41472U,static_cast<uint16_t>(pps_hardware::kCounterHz));
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(72000000U),static_cast<uint16_t>(72065536U));
}

void test_tim1_unavailable_and_conflicting_flags_break_continuity() {
  const uint32_t bad[] = {0U,pps_hardware::kUnavailable,pps_hardware::kValid|pps_hardware::kUnavailable,
                         pps_hardware::kValid|pps_hardware::kOvercapture,
                         pps_hardware::kValid|pps_hardware::kReadRace};
  for (uint32_t flags : bad) {
    pps_hardware::IntervalEvidence tracker;
    tracker.observe(1U,pps_hardware::kValid|100U);
    auto out=tracker.observe(2U,flags|200U);
    TEST_ASSERT_FALSE(out.captureValid || out.intervalModuloValid);
    TEST_ASSERT_EQUAL((flags&pps_hardware::kUnavailable)!=0U,out.unavailable);
    out=tracker.observe(3U,pps_hardware::kValid|300U);
    TEST_ASSERT_TRUE(out.captureValid);
    TEST_ASSERT_FALSE(out.intervalModuloValid);
    out=tracker.observe(4U,pps_hardware::kValid|400U);
    TEST_ASSERT_TRUE(out.intervalModuloValid);
  }
}
}

void run_tim1_capture_tests() {
  RUN_TEST(test_tim1_rcc_dividers_use_sysclk_and_timer_doubling);
  RUN_TEST(test_tim1_ccr_and_update_order_are_independent);
  RUN_TEST(test_tim1_overcapture_and_ccr_read_races_fail_closed);
  RUN_TEST(test_tim1_exti_before_capture_and_delayed_single_latch);
  RUN_TEST(test_tim1_modulo_evidence_never_invents_upper_counts);
  RUN_TEST(test_tim1_72mhz_phases_drift_and_wide_count_aliases);
  RUN_TEST(test_tim1_unavailable_and_conflicting_flags_break_continuity);
}

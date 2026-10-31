#include "pps_capture.h"

PpsCapture *PpsCapture::instance_ = 0;

PpsCapture::PpsCapture()
    : isrMicros_(0), isrSequence_(0), isrFlag_(false)
#if USE_TIM1_INPUT_CAPTURE
      , isrHardwareSample_(0U)
#endif
      {}

void PpsCapture::begin() {
  instance_ = this;
  pinMode(PPS_PIN, INPUT);
#if USE_TIM1_INPUT_CAPTURE
  // 再初期化中のエッジを古い保留値・間隔履歴と混ぜない。通し番号は維持する。
  detachInterrupt(digitalPinToInterrupt(PPS_PIN));
  isrFlag_ = false;
  intervalTracker_ = PpsIntervalTracker();
  (void)pps_hardware::beginTim1();
  isrHardwareSample_ = 0U;
  hardwareIntervals_ = pps_hardware::IntervalEvidence();
#endif
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), isrThunk, RISING);
}

void PpsCapture::isrThunk() {
  if (instance_ != 0) instance_->onEdge();
}

void PpsCapture::onEdge() {
  isrMicros_ = micros();
#if USE_TIM1_INPUT_CAPTURE
  // CCR1 はエッジで既にラッチ済み。micros() の値・通し番号の意味は変えない。
  isrHardwareSample_ = pps_hardware::sampleTim1();
#endif
  ++isrSequence_;
  isrFlag_ = true;
}

bool PpsCapture::update(PpsSnapshot &out) {
  noInterrupts();
  const bool pending = isrFlag_;
  const uint32_t edge = isrMicros_;
  const uint32_t sequence = isrSequence_;
#if USE_TIM1_INPUT_CAPTURE
  const uint32_t hardwareSample = isrHardwareSample_;
#endif
  isrFlag_ = false;
  interrupts();
  if (!pending) return false;

  intervalTracker_.observe(sequence, edge);
  const PpsIntervalStats &stats = intervalTracker_.stats();
  out.edgeMicros = edge;
  out.sequence = sequence;
  out.intervalMicros = stats.lastIntervalMicros;
  out.lostCount = stats.lost;
  out.anomalyCount = stats.abnormalInterval;
  out.consecutiveGood = stats.consecutiveGood;
  out.recentBad = stats.recentBad;
  out.newEdge = true;
#if USE_TIM1_INPUT_CAPTURE
  out.hardware = hardwareIntervals_.observe(sequence, hardwareSample);
#endif
  return true;
}

void PpsCapture::snapshot(PpsSnapshot &out) const {
  noInterrupts();
  out.edgeMicros = isrMicros_;
  out.sequence = isrSequence_;
#if USE_TIM1_INPUT_CAPTURE
  const uint32_t hardwareSample = isrHardwareSample_;
#endif
  interrupts();
  const PpsIntervalStats &stats = intervalTracker_.stats();
  out.intervalMicros = stats.lastIntervalMicros;
  out.lostCount = stats.lost;
  out.anomalyCount = stats.abnormalInterval;
  out.consecutiveGood = stats.consecutiveGood;
  out.recentBad = stats.recentBad;
  out.newEdge = false;
#if USE_TIM1_INPUT_CAPTURE
  // snapshot は消費しないので、間隔証跡を新しく作らない。
  pps_hardware::IntervalEvidence single;
  out.hardware = single.observe(out.sequence, hardwareSample);
#endif
}

#if HOSHIGUARD_GNSS_RESET
void PpsCapture::discardHistory() {
  // ISR の通し番号は維持し、復旧前の保留エッジと間隔履歴だけを破棄する。
  noInterrupts();
  isrFlag_ = false;
#if USE_TIM1_INPUT_CAPTURE
  isrHardwareSample_ = 0U;
  pps_hardware::discardTim1();
#endif
  interrupts();
  intervalTracker_ = PpsIntervalTracker();
#if USE_TIM1_INPUT_CAPTURE
  hardwareIntervals_ = pps_hardware::IntervalEvidence();
#endif
}
#endif

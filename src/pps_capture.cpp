#include "pps_capture.h"

PpsCapture *PpsCapture::instance_ = 0;

PpsCapture::PpsCapture()
    : isrMicros_(0), isrSequence_(0), isrFlag_(false) {}

void PpsCapture::begin() {
  instance_ = this;
  pinMode(PPS_PIN, INPUT);
  // USE_TIM1_INPUT_CAPTURE is reserved for a board-specific higher precision
  // backend. Both settings intentionally retain the verified EXTI baseline.
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), isrThunk, RISING);
}

void PpsCapture::isrThunk() {
  if (instance_ != 0) instance_->onEdge();
}

void PpsCapture::onEdge() {
  isrMicros_ = micros();
  ++isrSequence_;
  isrFlag_ = true;
}

bool PpsCapture::update(PpsSnapshot &out) {
  noInterrupts();
  const bool pending = isrFlag_;
  const uint32_t edge = isrMicros_;
  const uint32_t sequence = isrSequence_;
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
  return true;
}

void PpsCapture::snapshot(PpsSnapshot &out) const {
  noInterrupts();
  out.edgeMicros = isrMicros_;
  out.sequence = isrSequence_;
  interrupts();
  const PpsIntervalStats &stats = intervalTracker_.stats();
  out.intervalMicros = stats.lastIntervalMicros;
  out.lostCount = stats.lost;
  out.anomalyCount = stats.abnormalInterval;
  out.consecutiveGood = stats.consecutiveGood;
  out.recentBad = stats.recentBad;
  out.newEdge = false;
}

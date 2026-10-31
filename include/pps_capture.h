#pragma once

#include <Arduino.h>

#include "config.h"
#include "time_sync.h"
#if USE_TIM1_INPUT_CAPTURE
#include "tim1_capture.h"
#endif

struct PpsSnapshot {
  uint32_t edgeMicros;
  uint32_t sequence;
  uint32_t intervalMicros;
  uint32_t lostCount;
  uint32_t anomalyCount;
  uint32_t consecutiveGood;
  uint32_t recentBad;
  bool newEdge;
#if USE_TIM1_INPUT_CAPTURE
  pps_hardware::Evidence hardware;
#endif
};

class PpsCapture {
 public:
  PpsCapture();
  void begin();
  bool update(PpsSnapshot &snapshot);
  void snapshot(PpsSnapshot &snapshot) const;
  const PpsIntervalStats &healthStats() const { return intervalTracker_.stats(); }
#if USE_TIM1_INPUT_CAPTURE
  bool hardwareActive() const { return hardwareActive_; }
#endif
#if HOSHIGUARD_GNSS_RESET
  void discardHistory();
#endif

 private:
  static void isrThunk();
  void onEdge();
  static PpsCapture *instance_;

  volatile uint32_t isrMicros_;
  volatile uint32_t isrSequence_;
  volatile bool isrFlag_;
  PpsIntervalTracker intervalTracker_;
#if USE_TIM1_INPUT_CAPTURE
  volatile uint32_t isrHardwareSample_;
  bool hardwareActive_;
  pps_hardware::IntervalEvidence hardwareIntervals_;
#endif
};

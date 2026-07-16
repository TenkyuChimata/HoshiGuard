#pragma once

#include <Arduino.h>

#include "config.h"
#include "time_sync.h"

struct PpsSnapshot {
  uint32_t edgeMicros;
  uint32_t sequence;
  uint32_t intervalMicros;
  uint32_t lostCount;
  uint32_t anomalyCount;
  uint32_t consecutiveGood;
  uint32_t recentBad;
  bool newEdge;
};

class PpsCapture {
 public:
  PpsCapture();
  void begin();
  bool update(PpsSnapshot &snapshot);
  void snapshot(PpsSnapshot &snapshot) const;
  const PpsIntervalStats &healthStats() const { return intervalTracker_.stats(); }

 private:
  static void isrThunk();
  void onEdge();
  static PpsCapture *instance_;

  volatile uint32_t isrMicros_;
  volatile uint32_t isrSequence_;
  volatile bool isrFlag_;
  PpsIntervalTracker intervalTracker_;
};

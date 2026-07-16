#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tim_tp.h"

enum class TimeQuality : uint8_t {
  Invalid,
  GnssUtcValid,
  PpsObserved,
  UtcPpsTentative,
  LockedUtcLimited,
  UtcPpsConfirmed,
  Degraded
};

enum class TimeConfirmReason : uint8_t {
  None,
  NoPps,
  NoTimTp,
  TimTpInvalid,
  TimeGridUnknown,
  AmbiguousMatch,
  NotEnoughConsecutiveMatches,
  PpsLost,
  PpsAbnormal,
  TimTpNotLocked,
  UtcUnavailable,
  TimeBaseNotUtc,
  QErrInvalid,
  TowDiscontinuity
};

const char *timeConfirmReasonName(TimeConfirmReason reason);

const char *timeQualityName(TimeQuality quality);

struct PpsEvent {
  uint32_t sequence;
  uint32_t microsValue;
  bool utcAssigned;
  int64_t utcSecond;
};

struct PpsIntervalStats {
  uint32_t count;
  uint32_t lastIntervalMicros;
  uint32_t lost;
  uint32_t abnormalInterval;
  uint32_t consecutiveGood;
  uint32_t recentBad;
};

class PpsIntervalTracker {
 public:
  PpsIntervalTracker();
  void observe(uint32_t sequence, uint32_t edgeMicros);
  const PpsIntervalStats &stats() const { return stats_; }

 private:
  PpsIntervalStats stats_;
  uint32_t lastSequence_;
  uint32_t lastMicros_;
};

struct NavPvtRecord {
  uint32_t iTowMs;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t validFlags;
  int32_t nano;
  uint32_t receivedMicros;
  uint32_t nearestPpsSequence;
  uint32_t nearestPpsMicros;
  bool utcValid;
  bool ppsAssociated;
  TimeQuality quality;
};

struct TimeSyncStats {
  uint32_t navPvtCount;
  uint32_t validUtcCount;
  uint32_t utcPpsAssociated;
  uint32_t utcPpsUnassociated;
  uint32_t timTpCount;
  uint32_t timTpMatched;
  uint32_t timTpAmbiguous;
  uint32_t timTpInvalid;
  uint32_t timTpLimited;
  uint32_t confirmedCount;
};

struct TimTpObservation {
  UbxTimTp message;
  uint32_t receivedMicros;
  uint32_t previousPpsSequence;
  uint32_t matchedPpsSequence;
  uint32_t matchedPpsMicros;
  bool matched;
  bool ambiguous;
};

class PpsUtcAssociator {
 public:
  PpsUtcAssociator();
  void observePps(uint32_t sequence, uint32_t edgeMicros);
  void observeTimTp(const UbxTimTp &message, uint32_t receivedMicros);
  bool observeNavPvt(const uint8_t *payload, uint16_t length,
                     uint32_t receivedMicros, NavPvtRecord &record);
  const PpsEvent *newestPps() const;
  const TimeSyncStats &stats() const { return stats_; }
  TimeQuality quality() const { return quality_; }
  TimeConfirmReason confirmReason() const { return confirmReason_; }
  void setTimeGridKnown(bool known) { timeGridKnown_ = known; }
  bool timeGridKnown() const { return timeGridKnown_; }
  uint8_t consecutiveTimTpMatches() const { return consecutiveMatches_; }
  const TimTpObservation &lastTimTp() const { return lastTimTp_; }
  uint32_t timTpMatchGeneration() const { return matchGeneration_; }

 private:
  static const size_t PPS_CAPACITY = 4U;
  PpsEvent events_[PPS_CAPACITY];
  size_t next_;
  size_t count_;
  TimeSyncStats stats_;
  TimeQuality quality_;
  TimeConfirmReason confirmReason_;
  TimTpObservation pendingTimTp_;
  TimTpObservation lastTimTp_;
  bool pendingTimTpValid_;
  bool pendingAmbiguous_;
  bool timeGridKnown_;
  uint8_t consecutiveMatches_;
  uint32_t matchGeneration_;
  uint32_t lastPpsSequence_;
  uint32_t lastPpsMicros_;
  uint16_t lastMatchedWeek_;
  uint32_t lastMatchedTowMs_;
  bool haveLastMatchedTow_;
};

bool utcToUnixSecond(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second,
                     int64_t &value);

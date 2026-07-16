#include "time_sync.h"

#include <string.h>

namespace {
uint16_t readLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool isLeap(uint16_t year) {
  return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}
}  // namespace

const char *timeQualityName(TimeQuality quality) {
  switch (quality) {
    case TimeQuality::Invalid: return "invalid";
    case TimeQuality::GnssUtcValid: return "utc_valid";
    case TimeQuality::PpsObserved: return "pps_only";
    case TimeQuality::UtcPpsTentative: return "tentative";
    case TimeQuality::LockedUtcLimited: return "locked_utc_limited";
    case TimeQuality::UtcPpsConfirmed: return "confirmed";
    case TimeQuality::Degraded: return "degraded";
  }
  return "invalid";
}

const char *timeConfirmReasonName(TimeConfirmReason reason) {
  switch (reason) {
    case TimeConfirmReason::None: return "none";
    case TimeConfirmReason::NoPps: return "no_pps";
    case TimeConfirmReason::NoTimTp: return "no_tim_tp";
    case TimeConfirmReason::TimTpInvalid: return "tim_tp_invalid";
    case TimeConfirmReason::TimeGridUnknown: return "time_grid_unknown";
    case TimeConfirmReason::AmbiguousMatch: return "ambiguous_match";
    case TimeConfirmReason::NotEnoughConsecutiveMatches: return "not_stable";
    case TimeConfirmReason::PpsLost: return "pps_lost";
    case TimeConfirmReason::PpsAbnormal: return "pps_abnormal";
    case TimeConfirmReason::TimTpNotLocked: return "tim_tp_not_locked";
    case TimeConfirmReason::UtcUnavailable: return "utc_unavailable";
    case TimeConfirmReason::TimeBaseNotUtc: return "time_base_not_utc";
    case TimeConfirmReason::QErrInvalid: return "qerr_unavailable";
    case TimeConfirmReason::TowDiscontinuity: return "tow_discontinuity";
  }
  return "unknown";
}

PpsIntervalTracker::PpsIntervalTracker()
    : lastSequence_(0U), lastMicros_(0U) {
  memset(&stats_, 0, sizeof(stats_));
}

void PpsIntervalTracker::observe(uint32_t sequence, uint32_t edgeMicros) {
  if (sequence == 0U) return;
  ++stats_.count;
  if (lastSequence_ != 0U) {
    const uint32_t interval = edgeMicros - lastMicros_;  // wrap-safe
    stats_.lastIntervalMicros = interval;
    const uint32_t sequenceGap = sequence - lastSequence_;
    bool bad = false;
    if (sequenceGap > 1U) {
      stats_.lost += sequenceGap - 1U;
      bad = true;
    }
    if (interval < 900000U || interval > 1100000U) {
      ++stats_.abnormalInterval;
      bad = true;
    }
    if (sequenceGap == 1U && interval > 1500000U) {
      const uint32_t periods = (interval + 500000U) / 1000000U;
      if (periods > 1U) stats_.lost += periods - 1U;
    }
    if (bad) {
      stats_.consecutiveGood = 0U;
      if (stats_.recentBad != 0xFFFFFFFFUL) ++stats_.recentBad;
    } else {
      if (stats_.consecutiveGood != 0xFFFFFFFFUL)
        ++stats_.consecutiveGood;
      if (stats_.consecutiveGood >= 3U) stats_.recentBad = 0U;
    }
  }
  lastSequence_ = sequence;
  lastMicros_ = edgeMicros;
}

PpsUtcAssociator::PpsUtcAssociator()
    : next_(0U), count_(0U), quality_(TimeQuality::Invalid),
      confirmReason_(TimeConfirmReason::NoPps), pendingTimTpValid_(false),
      pendingAmbiguous_(false), timeGridKnown_(false),
      consecutiveMatches_(0U), matchGeneration_(0U), lastPpsSequence_(0U),
      lastPpsMicros_(0U), lastMatchedWeek_(0U), lastMatchedTowMs_(0U),
      haveLastMatchedTow_(false) {
  memset(events_, 0, sizeof(events_));
  memset(&stats_, 0, sizeof(stats_));
  memset(&pendingTimTp_, 0, sizeof(pendingTimTp_));
  memset(&lastTimTp_, 0, sizeof(lastTimTp_));
}

void PpsUtcAssociator::observePps(uint32_t sequence, uint32_t edgeMicros) {
  PpsEvent &event = events_[next_];
  event.sequence = sequence;
  event.microsValue = edgeMicros;
  event.utcAssigned = false;
  event.utcSecond = 0;
  next_ = (next_ + 1U) % PPS_CAPACITY;
  if (count_ < PPS_CAPACITY) ++count_;

  bool ppsHealthy = true;
  if (lastPpsSequence_ != 0U) {
    const uint32_t gap = sequence - lastPpsSequence_;
    const uint32_t interval = edgeMicros - lastPpsMicros_;
    if (gap != 1U) {
      confirmReason_ = TimeConfirmReason::PpsLost;
      ppsHealthy = false;
    } else if (interval < 900000U || interval > 1100000U) {
      confirmReason_ = TimeConfirmReason::PpsAbnormal;
      ppsHealthy = false;
    }
  }
  lastPpsSequence_ = sequence;
  lastPpsMicros_ = edgeMicros;

  if (!pendingTimTpValid_) {
    consecutiveMatches_ = 0U;
    quality_ = TimeQuality::PpsObserved;
    confirmReason_ = TimeConfirmReason::NoTimTp;
    return;
  }

  TimTpObservation observation = pendingTimTp_;
  pendingTimTpValid_ = false;
  observation.matchedPpsSequence = sequence;
  observation.matchedPpsMicros = edgeMicros;
  observation.ambiguous = pendingAmbiguous_;
  pendingAmbiguous_ = false;
  const uint32_t leadUs = edgeMicros - observation.receivedMicros;
  observation.matched = leadUs <= 1100000U;
  lastTimTp_ = observation;
  ++matchGeneration_;
  if (!observation.matched) {
    consecutiveMatches_ = 0U;
    quality_ = TimeQuality::Degraded;
    confirmReason_ = TimeConfirmReason::AmbiguousMatch;
    ++stats_.timTpAmbiguous;
    return;
  }
  ++stats_.timTpMatched;

  TimeConfirmReason failure = TimeConfirmReason::None;
  if (observation.ambiguous) failure = TimeConfirmReason::AmbiguousMatch;
  else if (!ppsHealthy) failure = confirmReason_;
  else if (!timeGridKnown_) failure = TimeConfirmReason::TimeGridUnknown;
  else if (!observation.message.tpLocked)
    failure = TimeConfirmReason::TimTpNotLocked;
  else if (!observation.message.timeBaseUtc)
    failure = TimeConfirmReason::TimeBaseNotUtc;
  else if (!observation.message.utcAvailable)
    failure = TimeConfirmReason::UtcUnavailable;
  const bool qErrUnavailable = !observation.message.qErrValid;

  if (failure == TimeConfirmReason::None && haveLastMatchedTow_) {
    const bool sameWeek = observation.message.week == lastMatchedWeek_ &&
        observation.message.towMs == lastMatchedTowMs_ + 1000U;
    const bool weekRollover =
        observation.message.week == static_cast<uint16_t>(lastMatchedWeek_ + 1U) &&
        lastMatchedTowMs_ == 604799000UL && observation.message.towMs == 0U;
    if (!sameWeek && !weekRollover)
      failure = TimeConfirmReason::TowDiscontinuity;
  }

  lastMatchedWeek_ = observation.message.week;
  lastMatchedTowMs_ = observation.message.towMs;
  haveLastMatchedTow_ = true;
  if (failure != TimeConfirmReason::None) {
    consecutiveMatches_ = 0U;
    quality_ = TimeQuality::Degraded;
    confirmReason_ = failure;
    if (failure == TimeConfirmReason::AmbiguousMatch)
      ++stats_.timTpAmbiguous;
    return;
  }

  if (qErrUnavailable) {
    consecutiveMatches_ = 0U;
    quality_ = TimeQuality::LockedUtcLimited;
    confirmReason_ = TimeConfirmReason::QErrInvalid;
    ++stats_.timTpLimited;
    return;
  }

  if (consecutiveMatches_ < 255U) ++consecutiveMatches_;
  if (consecutiveMatches_ >= 3U) {
    quality_ = TimeQuality::UtcPpsConfirmed;
    confirmReason_ = TimeConfirmReason::None;
    ++stats_.confirmedCount;
  } else {
    quality_ = TimeQuality::PpsObserved;
    confirmReason_ = TimeConfirmReason::NotEnoughConsecutiveMatches;
  }
}

void PpsUtcAssociator::observeTimTp(const UbxTimTp &message,
                                    uint32_t receivedMicros) {
  ++stats_.timTpCount;
  if (pendingTimTpValid_) pendingAmbiguous_ = true;
  memset(&pendingTimTp_, 0, sizeof(pendingTimTp_));
  pendingTimTp_.message = message;
  pendingTimTp_.receivedMicros = receivedMicros;
  pendingTimTp_.previousPpsSequence = lastPpsSequence_;
  pendingTimTpValid_ = true;
  if (!message.tpLocked ||
      (message.timeBaseUtc && !message.utcAvailable)) {
    ++stats_.timTpInvalid;
  }
}

const PpsEvent *PpsUtcAssociator::newestPps() const {
  if (count_ == 0U) return 0;
  const size_t index = (next_ + PPS_CAPACITY - 1U) % PPS_CAPACITY;
  return &events_[index];
}

bool utcToUnixSecond(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second,
                     int64_t &value) {
  static const uint8_t daysPerMonth[] =
      {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  if (year < 1970U || year > 2099U || month < 1U || month > 12U ||
      day < 1U || hour > 23U || minute > 59U || second > 60U) return false;
  uint8_t maximum = daysPerMonth[month - 1U];
  if (month == 2U && isLeap(year)) maximum = 29U;
  if (day > maximum) return false;
  int64_t days = 0;
  for (uint16_t y = 1970U; y < year; ++y) days += isLeap(y) ? 366 : 365;
  for (uint8_t m = 1U; m < month; ++m) {
    days += daysPerMonth[m - 1U];
    if (m == 2U && isLeap(year)) ++days;
  }
  days += day - 1U;
  value = days * 86400LL + hour * 3600LL + minute * 60LL + second;
  return true;
}

bool PpsUtcAssociator::observeNavPvt(const uint8_t *payload, uint16_t length,
                                     uint32_t receivedMicros,
                                     NavPvtRecord &record) {
  memset(&record, 0, sizeof(record));
  if (payload == 0 || length != 92U) return false;
  ++stats_.navPvtCount;
  record.iTowMs = readLe32(payload);
  record.year = readLe16(payload + 4U);
  record.month = payload[6];
  record.day = payload[7];
  record.hour = payload[8];
  record.minute = payload[9];
  record.second = payload[10];
  record.validFlags = payload[11];
  record.nano = static_cast<int32_t>(readLe32(payload + 16U));
  record.receivedMicros = receivedMicros;
  int64_t utcSecond = 0;
  record.utcValid = (record.validFlags & 0x07U) == 0x07U &&
      utcToUnixSecond(record.year, record.month, record.day, record.hour,
                      record.minute, record.second, utcSecond);
  if (!record.utcValid) {
    record.quality = count_ == 0U ? TimeQuality::Invalid
                                 : TimeQuality::PpsObserved;
    quality_ = record.quality;
    ++stats_.utcPpsUnassociated;
    return true;
  }
  ++stats_.validUtcCount;
  record.quality = TimeQuality::GnssUtcValid;

  // NAV-PVT reports the navigation epoch, not the physical pulse. At the
  // configured 1 Hz rate an integer-second iTOW normally arrives after the
  // corresponding rising edge. Serial latency is not a proof of identity, so
  // this nearest preceding edge is deliberately only tentative. UBX-TIM-TP or
  // another receiver timing message is required before using Confirmed.
  if (count_ != 0U && record.iTowMs % 1000U == 0U) {
    const size_t index = (next_ + PPS_CAPACITY - 1U) % PPS_CAPACITY;
    PpsEvent &event = events_[index];
    const uint32_t age = receivedMicros - event.microsValue;  // wrap-safe
    if (age <= 750000U) {
      event.utcAssigned = true;
      event.utcSecond = utcSecond;
      record.nearestPpsSequence = event.sequence;
      record.nearestPpsMicros = event.microsValue;
      record.ppsAssociated = true;
      record.quality = TimeQuality::UtcPpsTentative;
    }
  }
  if (record.ppsAssociated) ++stats_.utcPpsAssociated;
  else ++stats_.utcPpsUnassociated;
  if (quality_ == TimeQuality::UtcPpsConfirmed && record.ppsAssociated)
    record.quality = TimeQuality::UtcPpsConfirmed;
  else if (quality_ == TimeQuality::LockedUtcLimited && record.ppsAssociated)
    record.quality = TimeQuality::LockedUtcLimited;
  else
    quality_ = record.quality;
  return true;
}

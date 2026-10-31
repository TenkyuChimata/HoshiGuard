#include "status_display.h"

#include <stdio.h>
#include <string.h>

namespace oled {
namespace {
void countText(uint32_t value, uint32_t limit, char (&text)[12]) {
  if (value > limit)
    (void)snprintf(text, sizeof(text), "%lu+", static_cast<unsigned long>(limit));
  else
    (void)snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
}
const char *timeLabel(TimeQuality quality) {
  switch (quality) {
    case TimeQuality::GnssUtcValid: return "UTC";
    case TimeQuality::PpsObserved: return "PPS";
    case TimeQuality::UtcPpsTentative: return "TENT";
    case TimeQuality::LockedUtcLimited: return "LIMIT";
    case TimeQuality::UtcPpsConfirmed: return "CONF";
    case TimeQuality::Degraded: return "WARN";
    default: return "NONE";
  }
}
}

void Age::touch(uint32_t nowMs) {
  seen_ = true;
  lastMs_ = nowMs;
  elapsedMs_ = 0U;
}

void Age::advance(uint32_t nowMs) {
  if (!seen_) return;
  const uint32_t delta = nowMs - lastMs_;
  lastMs_ = nowMs;
  elapsedMs_ = delta > UINT32_MAX - elapsedMs_ ? UINT32_MAX : elapsedMs_ + delta;
}

StatusDisplay::StatusDisplay()
    : validL1s_(0U), lastRenderMs_(0U), lastMt_(0U), rendered_(false) {}

void StatusDisplay::noteL1s(uint32_t nowMs, uint8_t messageType, bool valid) {
  if (!valid) return;
  l1s_.touch(nowMs);
  lastMt_ = messageType;
  if (validL1s_ != UINT32_MAX) ++validL1s_;
}

bool StatusDisplay::due(uint32_t nowMs) const {
  return !rendered_ || nowMs - lastRenderMs_ >= 500U;
}

void StatusDisplay::render(uint32_t nowMs, const StatusInput &input,
                            Screen &screen, const char *version) {
  lastRenderMs_ = nowMs;
  rendered_ = true;
  ubx_.advance(nowMs); pps_.advance(nowMs); nav_.advance(nowMs);
  timTp_.advance(nowMs); l1s_.advance(nowMs);
  memset(&screen, 0, sizeof(screen));
  screen.inverted = 1U;
  char line[64];
  (void)snprintf(line, sizeof(line), "HoshiGuard %.10s", version == 0 ? "?" : version);
  setRow(screen, 0U, line);
  const bool receiving = ubx_.fresh(3000U);
  const bool ppsFresh = pps_.fresh(1800U);
  const bool ppsPresent = ppsFresh && !input.ppsAbnormal;
  const bool ppsHealthy = ppsPresent && input.ppsStable;
  const bool live = l1s_.fresh(30000U);
  const char *gnss = "INIT";
  if (input.gnss == GnssState::Ready) gnss = "READY";
  else if (input.gnss == GnssState::Degraded) gnss = "DEG";
  else if (input.gnss == GnssState::Failed) gnss = "FAIL";
  (void)snprintf(line, sizeof(line), "M10S:%s UBX:%s", gnss,
                 receiving ? "OK" : (ubx_.seen() ? "LOST" : "WAIT"));
  setRow(screen, 1U, line);
  const char *quality = timeLabel(input.timeQuality);
  if (input.timeQuality != TimeQuality::Invalid) {
    if (!receiving || (input.timeQuality != TimeQuality::PpsObserved &&
                        !nav_.fresh(3000U)) ||
        (input.timeQuality != TimeQuality::GnssUtcValid && !ppsPresent) ||
        ((input.timeQuality == TimeQuality::UtcPpsConfirmed ||
          input.timeQuality == TimeQuality::LockedUtcLimited) &&
          (!ppsHealthy || !timTp_.fresh(3000U)))) quality = "STALE";
  }
  (void)snprintf(line, sizeof(line), "PPS:%s TIME:%s",
                 ppsFresh ? (input.ppsAbnormal ? "BAD" : (input.ppsStable ? "OK" : "SEEN")) :
                            (pps_.seen() ? "LOST" : "WAIT"), quality);
  setRow(screen, 2U, line);
  (void)snprintf(line, sizeof(line), "L1S:%s N:%lu", live ? "LIVE" : "WAIT",
                 static_cast<unsigned long>(validL1s_));
  setRow(screen, 3U, line);
  if (!l1s_.seen()) {
    setRow(screen, 4U, "AGE:-- MT:--");
  } else {
    char age[12];
    const uint32_t seconds = l1s_.elapsedMs() / 1000U;
    if (seconds < 10000U)
      (void)snprintf(age, sizeof(age), "%lus", static_cast<unsigned long>(seconds));
    else if (seconds < 86400U)
      (void)snprintf(age, sizeof(age), "%luh", static_cast<unsigned long>(seconds / 3600U));
    else
      (void)snprintf(age, sizeof(age), "%lud%s", static_cast<unsigned long>(seconds / 86400U),
                     l1s_.elapsedMs() == UINT32_MAX ? "+" : "");
    (void)snprintf(line, sizeof(line), "AGE:%s MT%u%s", age,
                   static_cast<unsigned>(lastMt_),
                   lastMt_ == 43U ? " DCR" : (lastMt_ == 44U ? " DCX" : ""));
    setRow(screen, 4U, line);
  }
  char first[12], second[12];
  if (input.bridgeBinary) {
    countText(input.bridgeSent, 99999U, first);
    countText(input.bridgeDropped, 9999U, second);
    (void)snprintf(line, sizeof(line), "BR TX:%s D:%s", first, second);
    setRow(screen, 5U, line);
  } else {
    setRow(screen, 5U, "BR:OFF TEXT OUTPUT");
  }
  countText(input.ubxErrors, 9999U, first);
  countText(input.l1sErrors, 9999U, second);
  (void)snprintf(line, sizeof(line), "BAD U:%s L:%s", first, second);
  setRow(screen, 6U, line);
  const char *notice = live ? "RX ACTIVE" : "WAIT L1S";
  bool warning = true;
  if (input.gnss == GnssState::Failed) notice = "! M10S INIT FAILED";
  else if (input.gnss == GnssState::Initializing) { notice = "CONFIGURING M10S"; warning = false; }
  else if (!receiving) notice = ubx_.seen() ? "! UBX RX LOST" : "! NO UBX RX";
  else if (input.bridgeBinary && input.bridgeL1sDropped != 0U) notice = "! BR L1S DROP";
  else if (input.bridgeBinary && input.bridgeDropped != 0U) notice = "! BR DROP HISTORY";
  else if (input.initError != 0U) {
    (void)snprintf(line, sizeof(line), "! CONFIG E%u", static_cast<unsigned>(input.initError));
    notice = line;
  } else if (pps_.seen() && !ppsPresent) notice = "! PPS CHECK";
  else warning = false;
  setRow(screen, 7U, notice);
  if (warning) screen.inverted = static_cast<uint8_t>(screen.inverted | 0x80U);
}
}

#pragma once

#include "ssd1306.h"
#include "time_sync.h"

namespace oled {
enum class GnssState : uint8_t { Initializing, Ready, Degraded, Failed };

struct StatusInput {
  GnssState gnss;
  uint8_t initError;
  TimeQuality timeQuality;
  bool ppsAbnormal;
  bool ppsStable;
  bool bridgeBinary;
  uint32_t bridgeSent;
  uint32_t bridgeDropped;
  uint32_t bridgeL1sDropped;
  uint32_t ubxErrors;
  uint32_t l1sErrors;
};

// 経過時間を飽和加算する。長期無受信でも millis() の次の周回で LIVE に戻さない。
class Age {
 public:
  Age() : lastMs_(0U), elapsedMs_(0U), seen_(false) {}
  void touch(uint32_t nowMs);
  void advance(uint32_t nowMs);
  bool seen() const { return seen_; }
  bool fresh(uint32_t limitMs) const { return seen_ && elapsedMs_ < limitMs; }
  uint32_t elapsedMs() const { return elapsedMs_; }
 private:
  uint32_t lastMs_;
  uint32_t elapsedMs_;
  bool seen_;
};

class StatusDisplay {
 public:
  StatusDisplay();
  void noteUbx(uint32_t nowMs) { ubx_.touch(nowMs); }
  void notePps(uint32_t nowMs) { pps_.touch(nowMs); }
  void noteNav(uint32_t nowMs) { nav_.touch(nowMs); }
  void noteTimTp(uint32_t nowMs) { timTp_.touch(nowMs); }
  void noteL1s(uint32_t nowMs, uint8_t messageType, bool valid);
  bool due(uint32_t nowMs) const;
  void render(uint32_t nowMs, const StatusInput &input, Screen &screen,
              const char *version);

 private:
  Age ubx_;
  Age pps_;
  Age nav_;
  Age timTp_;
  Age l1s_;
  uint32_t validL1s_;
  uint32_t lastRenderMs_;
  uint8_t lastMt_;
  bool rendered_;
};
}

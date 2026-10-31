#pragma once

#include <stdint.h>
#include "hardware_options.h"

namespace auxiliary {
#if HOSHIGUARD_STATUS_LED
enum class LedState : uint8_t { Starting, Running, Warning, Fatal };
class StatusLed {
 public:
  StatusLed();
  void noteUbx();
  bool tick(uint32_t nowMs, bool initializing, bool failed, bool configError);
  LedState state() const { return state_; }
 private:
  uint32_t lastMs_;
  uint32_t ubxAgeMs_;
  uint16_t phaseMs_;
  LedState state_;
  bool seenUbx_;
  bool freshUbx_;
};
#endif

#if HOSHIGUARD_OLED_SWITCH
// ON の一度だけで表示を抑止する安全側の起動ラッチ。接点の安定待ちをしない。
class OledBootSwitch {
 public:
  OledBootSwitch() : disabled_(false) {}
  void sample(bool high) { disabled_ = disabled_ || high; }
  bool disabled() const { return disabled_; }
 private:
  bool disabled_;
};
#endif

#if HOSHIGUARD_GNSS_RESET
struct RecoveryInput {
  bool initFailed;
  bool validUbxSeen;
  bool ppsSeen;
  bool rxBytes;
};
enum class RecoveryAction : uint8_t { None, RestartSoftware, AssertReset, ReleaseReset };
// 起動時の無通信専用。受信の実績が一度でもあれば、この起動中は介入しない。
class StartupRecovery {
 public:
  static const uint32_t kSoftwareWaitMs = 30000U;
  static const uint32_t kHardwareWaitMs = 120000U;
  static const uint32_t kQuietMs = 30000U;
  static const uint32_t kPulseMs = 10U;
  static const uint32_t kBootWaitMs = 1000U;
  StartupRecovery();
  RecoveryAction tick(uint32_t nowMs, const RecoveryInput &input);
  bool holdingReset() const { return stage_ == Stage::Holding; }
  bool suspendsReception() const { return stage_ == Stage::Holding || stage_ == Stage::Booting; }
  uint8_t hardwareResets() const { return hardwareResets_; }
 private:
  enum class Stage : uint8_t { Software, Hardware, Holding, Booting, Done };
  uint32_t lastMs_;
  uint32_t stageAgeMs_;
  uint32_t quietAgeMs_;
  Stage stage_;
  uint8_t hardwareResets_;
};
#endif
}

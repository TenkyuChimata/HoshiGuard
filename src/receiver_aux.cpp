#include "receiver_aux.h"

namespace auxiliary {
#if HOSHIGUARD_STATUS_LED || HOSHIGUARD_GNSS_RESET
namespace {
// 差分を飽和加算し、millis() の周回後に古い状態を新しい状態と誤認しない。
uint32_t addAge(uint32_t age, uint32_t delta) {
  return delta > UINT32_MAX - age ? UINT32_MAX : age + delta;
}
}
#endif

#if HOSHIGUARD_STATUS_LED
StatusLed::StatusLed()
    : lastMs_(0U), ubxAgeMs_(0U), phaseMs_(0U), state_(LedState::Starting),
      seenUbx_(false), freshUbx_(false) {}
void StatusLed::noteUbx() {
  seenUbx_ = true;
  freshUbx_ = true;
}
bool StatusLed::tick(uint32_t nowMs, bool initializing, bool failed, bool configError) {
  const uint32_t delta = nowMs - lastMs_;
  lastMs_ = nowMs;
  ubxAgeMs_ = freshUbx_ ? 0U : addAge(ubxAgeMs_, delta);
  freshUbx_ = false;
  LedState next = LedState::Running;
  if (failed) next = LedState::Fatal;
  else if (initializing) next = LedState::Starting;
  else if (configError || !seenUbx_ || ubxAgeMs_ >= 10000U) next = LedState::Warning;
  if (next != state_) { state_ = next; phaseMs_ = 0U; }
  else phaseMs_ = static_cast<uint16_t>((phaseMs_ + delta % 2000U) % 2000U);
  switch (state_) {
    case LedState::Starting: return phaseMs_ % 500U < 250U;
    case LedState::Running: return phaseMs_ < 50U;
    case LedState::Warning: return phaseMs_ < 100U || (phaseMs_ >= 200U && phaseMs_ < 300U);
    case LedState::Fatal: return phaseMs_ % 1000U < 500U && phaseMs_ % 200U < 100U;
  }
  return false;
}
#endif

#if HOSHIGUARD_GNSS_RESET
StartupRecovery::StartupRecovery()
    : lastMs_(0U), stageAgeMs_(0U), quietAgeMs_(0U), stage_(Stage::Software), hardwareResets_(0U) {}
RecoveryAction StartupRecovery::tick(uint32_t nowMs, const RecoveryInput &input) {
  const uint32_t delta = nowMs - lastMs_;
  lastMs_ = nowMs;
  stageAgeMs_ = addAge(stageAgeMs_, delta);
  quietAgeMs_ = input.rxBytes ? 0U : addAge(quietAgeMs_, delta);
  // アサート後の解除は、途中に入力が来ても必ず行う。
  if (stage_ == Stage::Holding) {
    if (stageAgeMs_ < kPulseMs) return RecoveryAction::None;
    stage_ = Stage::Booting;
    stageAgeMs_ = 0U;
    return RecoveryAction::ReleaseReset;
  }
  if (stage_ == Stage::Booting) {
    if (stageAgeMs_ < kBootWaitMs) return RecoveryAction::None;
    stage_ = Stage::Done;
    return RecoveryAction::RestartSoftware;
  }
  if (input.validUbxSeen || input.ppsSeen) stage_ = Stage::Done;
  if (stage_ == Stage::Software && input.initFailed && stageAgeMs_ >= kSoftwareWaitMs) {
    stage_ = Stage::Hardware;
    stageAgeMs_ = 0U;
    return RecoveryAction::RestartSoftware;
  }
  if (stage_ == Stage::Hardware && input.initFailed && stageAgeMs_ >= kHardwareWaitMs &&
      quietAgeMs_ >= kQuietMs) {
    stage_ = Stage::Holding;
    stageAgeMs_ = 0U;
    ++hardwareResets_;
    return RecoveryAction::AssertReset;
  }
  return RecoveryAction::None;
}
#endif
}

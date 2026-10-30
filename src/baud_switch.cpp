#include "baud_switch.h"

const char *baudSwitchStateName(BaudSwitchState state) {
  switch (state) {
    case BaudSwitchState::Idle: return "Idle";
    case BaudSwitchState::SendValset: return "SendValset";
    case BaudSwitchState::WaitOldBaudAck: return "WaitOldBaudAck";
    case BaudSwitchState::QuietBeforeHostSwitch: return "QuietBeforeHostSwitch";
    case BaudSwitchState::SwitchHostUart: return "SwitchHostUart";
    case BaudSwitchState::QuietAfterHostSwitch: return "QuietAfterHostSwitch";
    case BaudSwitchState::ResetParsers: return "ResetParsers";
    case BaudSwitchState::PollMonVerNewBaud: return "PollMonVerNewBaud";
    case BaudSwitchState::WaitMonVerNewBaud: return "WaitMonVerNewBaud";
    case BaudSwitchState::RecoverProbeTargetBaud: return "RecoverProbeTargetBaud";
    case BaudSwitchState::RecoverProbeOriginalBaud: return "RecoverProbeOriginalBaud";
    case BaudSwitchState::RecoverProbeCandidate: return "RecoverProbeCandidate";
    case BaudSwitchState::Verified: return "Verified";
    case BaudSwitchState::KeptOriginal: return "KeptOriginal";
    case BaudSwitchState::Failed: return "Failed";
  }
  return "Unknown";
}

const char *baudSwitchResultName(BaudSwitchResult result) {
  switch (result) {
    case BaudSwitchResult::Pending: return "PENDING";
    case BaudSwitchResult::Disabled: return "DISABLED";
    case BaudSwitchResult::NotNeeded: return "NOT_NEEDED";
    case BaudSwitchResult::Verified: return "VERIFIED";
    case BaudSwitchResult::KeptOriginal: return "KEPT_ORIGINAL";
    case BaudSwitchResult::RecoveredFallback: return "RECOVERED_FALLBACK";
    case BaudSwitchResult::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

bool normalConfigContainsBaudKey() {
  size_t count = 0U;
  const ConfigKeySpec *items = gnssConfigItems(count);
  for (size_t i = 0U; i < count; ++i)
    if (items[i].key == CFG_UART1_BAUDRATE) return true;
  return false;
}

bool baudSwitchMayStart(size_t completedNormalKeys, size_t normalKeyCount) {
  return completedNormalKeys == normalKeyCount;
}

BaudSwitchResult initialBaudSwitchResult(bool enabled, uint32_t currentBaud,
                                         uint32_t targetBaud) {
  if (!enabled) return BaudSwitchResult::Disabled;
  if (currentBaud == targetBaud) return BaudSwitchResult::NotNeeded;
  return BaudSwitchResult::Pending;
}

BaudSwitchResult evaluateBaudSwitchEvidence(bool enabled,
                                             uint32_t originalBaud,
                                             uint32_t targetBaud,
                                             bool valsetAck,
                                             bool targetMonVer,
                                             bool originalMonVer) {
  (void)valsetAck;  // ACK は処理の証拠であり、新 baud での通信を証明しない。
  if (!enabled) return BaudSwitchResult::Disabled;
  if (originalBaud == targetBaud) return BaudSwitchResult::NotNeeded;
  if (targetMonVer) return BaudSwitchResult::Verified;
  if (originalMonVer) return BaudSwitchResult::KeptOriginal;
  return BaudSwitchResult::Pending;
}

size_t buildBaudValsetPacket(uint32_t baud, uint8_t layers, uint8_t *output,
                             size_t capacity) {
  uint8_t payload[12];
  const size_t payloadLength = buildCfgValsetPayload(
      CFG_UART1_BAUDRATE, baud, 4U, layers, payload, sizeof(payload));
  if (payloadLength == 0U) return 0U;
  return UbxParser::buildPacket(0x06U, 0x8AU, payload,
                                static_cast<uint16_t>(payloadLength), output,
                                capacity);
}

BaudRecoverySequence::BaudRecoverySequence() : candidates_{0, 0, 0, 0, 0}, index_(5U) {}

void BaudRecoverySequence::begin(uint32_t originalBaud, uint32_t targetBaud) {
  candidates_[0] = targetBaud;
  candidates_[1] = originalBaud;
  candidates_[2] = 9600UL;
  candidates_[3] = 38400UL;
  candidates_[4] = 115200UL;
  index_ = 0U;
}

bool BaudRecoverySequence::next(uint32_t &baud) {
  if (index_ >= 5U) return false;
  baud = candidates_[index_++];
  return true;
}

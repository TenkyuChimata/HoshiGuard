#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gnss_config.h"
#include "ubx_parser.h"

enum class BaudSwitchState : uint8_t {
  Idle,
  SendValset,
  WaitOldBaudAck,
  QuietBeforeHostSwitch,
  SwitchHostUart,
  QuietAfterHostSwitch,
  ResetParsers,
  PollMonVerNewBaud,
  WaitMonVerNewBaud,
  RecoverProbeTargetBaud,
  RecoverProbeOriginalBaud,
  RecoverProbeCandidate,
  Verified,
  KeptOriginal,
  Failed
};

enum class BaudSwitchResult : uint8_t {
  Pending,
  Disabled,
  NotNeeded,
  Verified,
  KeptOriginal,
  RecoveredFallback,
  Failed
};

const char *baudSwitchStateName(BaudSwitchState state);
const char *baudSwitchResultName(BaudSwitchResult result);

bool normalConfigContainsBaudKey();
bool baudSwitchMayStart(size_t completedNormalKeys, size_t normalKeyCount);
BaudSwitchResult initialBaudSwitchResult(bool enabled, uint32_t currentBaud,
                                         uint32_t targetBaud);
BaudSwitchResult evaluateBaudSwitchEvidence(bool enabled,
                                             uint32_t originalBaud,
                                             uint32_t targetBaud,
                                             bool valsetAck,
                                             bool targetMonVer,
                                             bool originalMonVer);
size_t buildBaudValsetPacket(uint32_t baud, uint8_t layers, uint8_t *output,
                             size_t capacity);

// 新 baud 検証失敗後の復旧順序。最初に目標値、次に元の値を再試行し、最後の 3 回は回数を制限した自動検出を行う。重複する baud も新しい試行として扱う。
class BaudRecoverySequence {
 public:
  BaudRecoverySequence();
  void begin(uint32_t originalBaud, uint32_t targetBaud);
  bool next(uint32_t &baud);
  size_t attempts() const { return index_; }
  bool exhausted() const { return index_ >= 5U; }

 private:
  uint32_t candidates_[5];
  size_t index_;
};

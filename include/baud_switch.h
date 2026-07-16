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

// Finite recovery order required after new-baud verification fails. The first
// two probes deliberately retry target then original; the final three are the
// bounded auto-detection fallback. Duplicates are retained as fresh probes.
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

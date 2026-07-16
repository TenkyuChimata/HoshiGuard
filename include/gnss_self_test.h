#pragma once

#include <stdint.h>

#include "gnss_config.h"
#include "gnss_uart_diag.h"

enum class SelfTestStatus : uint8_t { Unknown, Pass, Fail, Degraded };

const char *selfTestStatusName(SelfTestStatus status);

struct GnssSelfTestEvidence {
  bool uartBytesObserved;
  bool txAttempted;
  bool validUbxObserved;
  bool monVerObserved;
  bool sfrbxObserved;
  bool navPvtObserved;
  bool ppsObserved;
  PpsActivity ppsActivity;
  bool optionalConfigFailure;
  bool timePulseAllVerified;
  bool timePulseAnyFailure;
  const ConfigEvidence *qzss;
  const ConfigEvidence *qzssL1s;
  const ConfigEvidence *ubxOutput;
  const ConfigEvidence *sfrbxOutput;
  const ConfigEvidence *navPvtOutput;
  const ConfigEvidence *timePulseEnable;
  const ConfigEvidence *timePulseGrid;
};

struct GnssSelfTestReport {
  SelfTestStatus rxActivity;
  SelfTestStatus txAttempted;
  BidirectionalUbxStatus bidirectionalUbx;
  SelfTestStatus uartCommunication;
  SelfTestStatus monVer;
  SelfTestStatus qzssEnabled;
  SelfTestStatus qzssL1sEnabled;
  SelfTestStatus ubxOutput;
  SelfTestStatus sfrbxOutput;
  SelfTestStatus navPvtOutput;
  SelfTestStatus timePulseConfig;
  SelfTestStatus ppsObserved;
  PpsActivity ppsActivity;
  SelfTestStatus overall;
};

GnssSelfTestReport evaluateGnssSelfTest(const GnssSelfTestEvidence &evidence);

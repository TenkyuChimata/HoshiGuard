#include "gnss_self_test.h"

namespace {
SelfTestStatus verified(const ConfigEvidence *evidence) {
  if (evidence == 0) return SelfTestStatus::Unknown;
  if (configEvidenceVerified(*evidence)) return SelfTestStatus::Pass;
  if (evidence->result == ConfigResult::Unsupported ||
      evidence->result == ConfigResult::Timeout ||
      evidence->result == ConfigResult::Failed)
    return SelfTestStatus::Fail;
  return SelfTestStatus::Unknown;
}
}  // namespace

const char *selfTestStatusName(SelfTestStatus status) {
  switch (status) {
    case SelfTestStatus::Unknown: return "UNKNOWN";
    case SelfTestStatus::Pass: return "PASS";
    case SelfTestStatus::Fail: return "FAIL";
    case SelfTestStatus::Degraded: return "DEGRADED";
  }
  return "UNKNOWN";
}

GnssSelfTestReport evaluateGnssSelfTest(const GnssSelfTestEvidence &e) {
  GnssSelfTestReport report = {};
  report.rxActivity = e.uartBytesObserved ? SelfTestStatus::Pass
                                           : SelfTestStatus::Fail;
  const bool txAttempted = e.txAttempted || e.monVerObserved;
  report.txAttempted = txAttempted ? SelfTestStatus::Pass
                                   : SelfTestStatus::Unknown;
  const GnssLinkEvidence link = {e.uartBytesObserved, txAttempted,
                                 e.monVerObserved, e.validUbxObserved};
  report.bidirectionalUbx = evaluateBidirectionalUbx(link);
  report.uartCommunication =
      report.bidirectionalUbx == BidirectionalUbxStatus::Pass
          ? SelfTestStatus::Pass
          : (report.bidirectionalUbx == BidirectionalUbxStatus::Fail
                 ? SelfTestStatus::Fail : SelfTestStatus::Unknown);
  report.monVer = e.monVerObserved ? SelfTestStatus::Pass
                                   : SelfTestStatus::Fail;
  report.qzssEnabled = verified(e.qzss);
  report.qzssL1sEnabled = verified(e.qzssL1s);
  report.ubxOutput = e.validUbxObserved ? SelfTestStatus::Pass
                                       : verified(e.ubxOutput);
  report.sfrbxOutput = e.sfrbxObserved ? SelfTestStatus::Pass
                                      : verified(e.sfrbxOutput);
  report.navPvtOutput = e.navPvtObserved ? SelfTestStatus::Pass
                                        : verified(e.navPvtOutput);
  if (e.timePulseAllVerified)
    report.timePulseConfig = SelfTestStatus::Pass;
  else if (e.timePulseAnyFailure)
    report.timePulseConfig = SelfTestStatus::Degraded;
  else
    report.timePulseConfig = SelfTestStatus::Unknown;
  report.ppsActivity =
      (e.ppsActivity == PpsActivity::NoActivity && e.ppsObserved)
          ? PpsActivity::Stable : e.ppsActivity;
  if (report.ppsActivity == PpsActivity::Stable)
    report.ppsObserved = SelfTestStatus::Pass;
  else if (report.ppsActivity == PpsActivity::TransientOrMissing ||
           report.ppsActivity == PpsActivity::Unstable)
    report.ppsObserved = SelfTestStatus::Degraded;
  else
    report.ppsObserved = e.ppsObserved ? SelfTestStatus::Unknown
                                      : SelfTestStatus::Unknown;

  const bool requiredFailure =
      report.uartCommunication == SelfTestStatus::Fail ||
      report.monVer == SelfTestStatus::Fail ||
      report.qzssEnabled == SelfTestStatus::Fail ||
      report.qzssL1sEnabled == SelfTestStatus::Fail ||
      report.ubxOutput == SelfTestStatus::Fail ||
      report.sfrbxOutput == SelfTestStatus::Fail;
  if (requiredFailure) report.overall = SelfTestStatus::Fail;
  else if (e.optionalConfigFailure ||
           report.timePulseConfig != SelfTestStatus::Pass ||
           report.ppsObserved != SelfTestStatus::Pass ||
           report.sfrbxOutput != SelfTestStatus::Pass)
    report.overall = SelfTestStatus::Degraded;
  else
    report.overall = SelfTestStatus::Pass;
  return report;
}

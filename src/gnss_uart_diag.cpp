#include "gnss_uart_diag.h"

#include "ubx_parser.h"

const char *bidirectionalUbxStatusName(BidirectionalUbxStatus value) {
  switch (value) {
    case BidirectionalUbxStatus::Unknown: return "UNKNOWN";
    case BidirectionalUbxStatus::Pass: return "PASS";
    case BidirectionalUbxStatus::Fail: return "FAIL";
  }
  return "UNKNOWN";
}

const char *baudEvidenceKindName(BaudEvidenceKind value) {
  switch (value) {
    case BaudEvidenceKind::Unknown: return "UNKNOWN";
    case BaudEvidenceKind::ValidUbx: return "VALID_UBX";
    case BaudEvidenceKind::NmeaOnly: return "NMEA_ONLY";
  }
  return "UNKNOWN";
}

const char *ppsActivityName(PpsActivity value) {
  switch (value) {
    case PpsActivity::NoActivity: return "NO";
    case PpsActivity::ObservedOnce: return "OBSERVED_ONCE";
    case PpsActivity::Stable: return "STABLE";
    case PpsActivity::TransientOrMissing: return "TRANSIENT_OR_MISSING";
    case PpsActivity::Unstable: return "UNSTABLE";
  }
  return "NO";
}

void recordGnssTx(GnssTxStats &stats, size_t requested, size_t written) {
  ++stats.packets;
  stats.bytesRequested += static_cast<uint32_t>(requested);
  stats.bytesWritten += static_cast<uint32_t>(written);
  if (written != requested) ++stats.shortWrites;
}

BidirectionalUbxStatus evaluateBidirectionalUbx(
    const GnssLinkEvidence &evidence) {
  if (evidence.monVerResponse) return BidirectionalUbxStatus::Pass;
  if (!evidence.txAttempted) return BidirectionalUbxStatus::Unknown;
  if (evidence.rxActivity || evidence.validUbxReceived)
    return BidirectionalUbxStatus::Fail;
  return BidirectionalUbxStatus::Unknown;
}

BaudEvidence selectBaudEvidence(bool hasValidUbx, uint32_t ubxBaud,
                                bool hasNmeaOnly, uint32_t nmeaBaud) {
  BaudEvidence result = {BaudEvidenceKind::Unknown, 0U};
  if (hasValidUbx) {
    result.kind = BaudEvidenceKind::ValidUbx;
    result.baud = ubxBaud;
  } else if (hasNmeaOnly) {
    result.kind = BaudEvidenceKind::NmeaOnly;
    result.baud = nmeaBaud;
  }
  return result;
}

PpsActivity evaluatePpsActivity(uint32_t sequence, uint32_t lastEdgeMicros,
                                uint32_t nowMicros, uint32_t lostCount,
                                uint32_t anomalyCount) {
  if (sequence == 0U) return PpsActivity::NoActivity;
  const uint32_t age = nowMicros - lastEdgeMicros;  // 周回しても安全
  if (age > 1800000UL) return PpsActivity::TransientOrMissing;
  if (lostCount != 0U || anomalyCount != 0U) return PpsActivity::Unstable;
  if (sequence < 3U) return PpsActivity::ObservedOnce;
  return PpsActivity::Stable;
}

PpsActivity evaluatePpsActivity(const PpsIntervalStats &stats,
                                uint32_t lastEdgeMicros,
                                uint32_t nowMicros) {
  if (stats.count == 0U) return PpsActivity::NoActivity;
  if (nowMicros - lastEdgeMicros > 1800000UL)
    return PpsActivity::TransientOrMissing;
  if (stats.count == 1U) return PpsActivity::ObservedOnce;
  if (stats.consecutiveGood >= 3U) return PpsActivity::Stable;
  if (stats.recentBad != 0U) return PpsActivity::Unstable;
  return PpsActivity::ObservedOnce;
}

size_t buildMonVerPoll(uint8_t *output, size_t capacity) {
  return UbxParser::buildPacket(0x0AU, 0x04U, 0, 0U, output, capacity);
}

size_t buildGnssLoopbackSequence(uint8_t *output, size_t capacity) {
  static const uint8_t prefix[] = {0x00U, 0x55U, 0xAAU, 0xFFU};
  if (output == 0 || capacity < sizeof(prefix) + 8U) return 0U;
  for (size_t i = 0U; i < sizeof(prefix); ++i) output[i] = prefix[i];
  const size_t poll = buildMonVerPoll(output + sizeof(prefix),
                                      capacity - sizeof(prefix));
  return poll == 8U ? sizeof(prefix) + poll : 0U;
}

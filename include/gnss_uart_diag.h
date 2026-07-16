#pragma once

#include <stddef.h>
#include <stdint.h>

#include "time_sync.h"

enum class BidirectionalUbxStatus : uint8_t { Unknown, Pass, Fail };
enum class BaudEvidenceKind : uint8_t { Unknown, ValidUbx, NmeaOnly };
enum class PpsActivity : uint8_t {
  NoActivity,
  ObservedOnce,
  Stable,
  TransientOrMissing,
  Unstable
};

struct GnssTxStats {
  uint32_t packets;
  uint32_t bytesRequested;
  uint32_t bytesWritten;
  uint32_t shortWrites;
};

struct GnssLinkEvidence {
  bool rxActivity;
  bool txAttempted;
  bool monVerResponse;
  bool validUbxReceived;
};

struct BaudEvidence {
  BaudEvidenceKind kind;
  uint32_t baud;
};

const char *bidirectionalUbxStatusName(BidirectionalUbxStatus value);
const char *baudEvidenceKindName(BaudEvidenceKind value);
const char *ppsActivityName(PpsActivity value);

void recordGnssTx(GnssTxStats &stats, size_t requested, size_t written);
BidirectionalUbxStatus evaluateBidirectionalUbx(
    const GnssLinkEvidence &evidence);
BaudEvidence selectBaudEvidence(bool hasValidUbx, uint32_t ubxBaud,
                                bool hasNmeaOnly, uint32_t nmeaBaud);
PpsActivity evaluatePpsActivity(uint32_t sequence, uint32_t lastEdgeMicros,
                                uint32_t nowMicros, uint32_t lostCount,
                                uint32_t anomalyCount);
PpsActivity evaluatePpsActivity(const PpsIntervalStats &stats,
                                uint32_t lastEdgeMicros,
                                uint32_t nowMicros);

size_t buildMonVerPoll(uint8_t *output, size_t capacity);
size_t buildGnssLoopbackSequence(uint8_t *output, size_t capacity);

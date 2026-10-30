#include "bridge_transport.h"

#if defined(ARDUINO)
#include <string.h>

namespace {
void copyFixed(uint8_t *out, size_t capacity, const char *value) {
  memset(out, 0, capacity);
  if (value == 0 || capacity == 0U) return;
  size_t length = strlen(value);
  if (length >= capacity) length = capacity - 1U;
  memcpy(out, value, length);
}
}

BridgeTransport::BridgeTransport(HardwareSerial &serial)
    : serial_(serial), nextSequence_(0U) {
  memset(&stats_, 0, sizeof(stats_));
}

void BridgeTransport::begin() {}

bool BridgeTransport::enqueue(bridge::FrameType type, bridge::Priority priority,
                              uint64_t timestampUs, uint16_t headerFlags,
                              const uint8_t *payload, uint16_t payloadLength,
                              bool validL1s) {
  ++stats_.framesGenerated;
  if (validL1s) ++stats_.l1sGenerated;
  const bridge::Header header = {type, payloadLength, headerFlags,
                                 nextSequence_++, timestampUs};
  size_t logicalLength = 0U;
  size_t encodedLength = 0U;
  bool encoded = bridge::encodeLogicalFrame(
      header, payload, logicalScratch_, sizeof(logicalScratch_),
      logicalLength);
  if (encoded) {
    encoded = bridge::cobsEncode(
        logicalScratch_, logicalLength, wireScratch_,
        sizeof(wireScratch_) - 1U, encodedLength);
  }
  if (encoded && encodedLength < sizeof(wireScratch_)) {
    wireScratch_[encodedLength++] = 0U;
  } else {
    encoded = false;
  }
  if (!encoded) {
    ++stats_.encodeErrors;
    ++stats_.framesDropped;
    if (priority == bridge::Priority::Critical) ++stats_.criticalDropped;
    if (validL1s) ++stats_.l1sDropped;
    return false;
  }
  const bridge::EnqueueResult result =
      queue_.enqueue(priority, wireScratch_, encodedLength);
  if (result == bridge::EnqueueResult::Dropped) {
    ++stats_.framesDropped;
    if (priority == bridge::Priority::Critical) ++stats_.criticalDropped;
    if (validL1s) ++stats_.l1sDropped;
    return false;
  }
  if (result == bridge::EnqueueResult::ReplacedLower) {
    ++stats_.framesDropped;
    if (queue_.lastEvictedPriority() == bridge::Priority::Critical)
      ++stats_.criticalDropped;
    // 低優先レコードで L1S を置換しない。後続 L1S は STATUS/TIME を置換できるため、ここでは L1S の欠落を加算しない。
  }
  ++stats_.framesQueued;
  if (validL1s) ++stats_.l1sQueued;
  if (queue_.highWatermark() > stats_.queueHighWatermark)
    stats_.queueHighWatermark =
        static_cast<uint32_t>(queue_.highWatermark());
  return true;
}

bool BridgeTransport::enqueueHello(uint64_t timestampUs,
                                   uint32_t currentBaud) {
  uint8_t payload[64];
  memset(payload, 0, sizeof(payload));
  payload[0] = bridge::kProtocolMajor;
  payload[1] = bridge::kProtocolMinor;
  payload[2] = HOSHIGUARD_VERSION_MAJOR;
  payload[3] = HOSHIGUARD_VERSION_MINOR;
  payload[4] = HOSHIGUARD_VERSION_PATCH;
  payload[5] = CAPTURE_PRIVACY_MODE != 0 ? 1U : 0U;
  payload[6] = 1U;  // リトルエンディアンの論理配置
  payload[7] = 1U;  // CRC 識別子 1 = CRC-32/ISO-HDLC
  uint32_t features = 0U;
  features |= 1UL << 0;  // 完全な UBX 証跡
  features |= 1UL << 1;  // PPS
  features |= 1UL << 2;  // TIM-TP
  features |= 1UL << 3;  // numWords=9
  features |= 1UL << 4;  // numWords=8 の検証済み旧形式
  bridge::putU32Le(payload + 8U, features);
  bridge::putU32Le(payload + 12U, DEBUG_BAUD);
  bridge::putU32Le(payload + 16U, currentBaud);
  bridge::putU16Le(payload + 20U,
                   static_cast<uint16_t>(bridge::kMaximumUbxLength));
  bridge::putU16Le(payload + 22U,
                   static_cast<uint16_t>(bridge::kMaximumLogicalFrame));
  copyFixed(payload + 24U, 16U, "STM32F103C8");
  copyFixed(payload + 40U, 24U, __DATE__ " " __TIME__);
  return enqueue(bridge::FrameType::Hello, bridge::Priority::Critical,
                 timestampUs, 0U, payload,
                 static_cast<uint16_t>(sizeof(payload)), false);
}

bool BridgeTransport::enqueueL1s(uint64_t timestampUs,
                                 const BridgeL1sMetadata &metadata,
                                 const uint8_t *fullUbx,
                                 uint16_t fullUbxLength) {
  if (fullUbx == 0 || fullUbxLength == 0U ||
      fullUbxLength > bridge::kMaximumUbxLength) return false;
  bridge::putU32Le(l1sPayloadScratch_, metadata.ppsSequence);
  l1sPayloadScratch_[4] = metadata.svId;
  l1sPayloadScratch_[5] = metadata.gnssId;
  l1sPayloadScratch_[6] = metadata.sigId;
  l1sPayloadScratch_[7] = metadata.numWords;
  l1sPayloadScratch_[8] = metadata.pab;
  l1sPayloadScratch_[9] = metadata.messageType;
  bridge::putU16Le(l1sPayloadScratch_ + 10U, metadata.flags);
  bridge::putU16Le(l1sPayloadScratch_ + 12U, fullUbxLength);
  l1sPayloadScratch_[14] = metadata.timeQuality;
  l1sPayloadScratch_[15] = metadata.ppsState;
  l1sPayloadScratch_[16] = HOSHIGUARD_VERSION_MAJOR;
  l1sPayloadScratch_[17] = HOSHIGUARD_VERSION_MINOR;
  l1sPayloadScratch_[18] = HOSHIGUARD_VERSION_PATCH;
  l1sPayloadScratch_[19] = 0U;
  memcpy(l1sPayloadScratch_ + 20U, fullUbx, fullUbxLength);
  const bool valid = (metadata.flags & bridge::L1sFlagUbxChecksumValid) != 0U &&
                     (metadata.flags & bridge::L1sFlagLengthValid) != 0U &&
                     (metadata.flags & bridge::L1sFlagPabValid) != 0U &&
                     (metadata.flags & bridge::L1sFlagCrcValid) != 0U;
  return enqueue(bridge::FrameType::L1sFrame, bridge::Priority::L1s,
                 timestampUs, 0U, l1sPayloadScratch_,
                 static_cast<uint16_t>(20U + fullUbxLength), valid);
}

bool BridgeTransport::enqueueStatus(uint64_t timestampUs,
                                    const BridgeStatusPayload &status) {
  uint8_t payload[88];
  memset(payload, 0, sizeof(payload));
  bridge::putU32Le(payload, status.uptimeMs);
  bridge::putU32Le(payload + 4U, status.currentBaud);
  payload[8] = status.gnssState;
  payload[9] = status.ppsState;
  payload[10] = status.timeQuality;
  payload[11] = status.lastError;
  bridge::putU32Le(payload + 12U, status.ubxReceived);
  bridge::putU32Le(payload + 16U, status.ubxChecksumErrors);
  bridge::putU32Le(payload + 20U, status.sfrbxReceived);
  bridge::putU32Le(payload + 24U, status.qzssL1sReceived);
  bridge::putU32Le(payload + 28U, status.validL1s);
  bridge::putU32Le(payload + 32U, stats_.framesGenerated);
  bridge::putU32Le(payload + 36U, stats_.framesQueued);
  bridge::putU32Le(payload + 40U, stats_.framesSent);
  bridge::putU32Le(payload + 44U, stats_.framesDropped);
  bridge::putU32Le(payload + 48U, stats_.criticalDropped);
  bridge::putU32Le(payload + 52U, stats_.uartOutputBytes);
  bridge::putU32Le(payload + 56U, stats_.queueHighWatermark);
  bridge::putU32Le(payload + 60U, status.ppsEdges);
  bridge::putU32Le(payload + 64U, status.ppsLostTotal);
  bridge::putU32Le(payload + 68U, status.ppsAbnormalTotal);
  bridge::putU32Le(payload + 72U, stats_.l1sGenerated);
  bridge::putU32Le(payload + 76U, stats_.l1sSent);
  bridge::putU32Le(payload + 80U, stats_.l1sDropped);
  bridge::putU32Le(payload + 84U, stats_.encodeErrors);
  return enqueue(bridge::FrameType::Status, bridge::Priority::Status,
                 timestampUs, 0U, payload,
                 static_cast<uint16_t>(sizeof(payload)), false);
}

bool BridgeTransport::enqueueTimeStatus(uint64_t timestampUs,
                                        uint32_t ppsSequence,
                                        uint8_t ppsState, uint8_t quality,
                                        uint8_t reason,
                                        const UbxTimTp &timTp,
                                        uint32_t matchedPpsSequence,
                                        bool matched, bool ambiguous,
                                        bool timeGridKnown) {
  uint8_t payload[32];
  memset(payload, 0, sizeof(payload));
  bridge::putU32Le(payload, ppsSequence);
  payload[4] = ppsState;
  payload[5] = quality;
  payload[6] = reason;
  payload[7] = timTp.flags;
  bridge::putU32Le(payload + 8U, static_cast<uint32_t>(timTp.qErrPs));
  payload[12] = timTp.qErrValid ? 1U : 0U;
  payload[13] = timTp.refInfo;
  bridge::putU16Le(payload + 14U, timTp.week);
  bridge::putU32Le(payload + 16U, timTp.towMs);
  bridge::putU32Le(payload + 20U, timTp.towSubMs);
  bridge::putU32Le(payload + 24U, matchedPpsSequence);
  if (matched) payload[28] |= 1U << 0;
  if (ambiguous) payload[28] |= 1U << 1;
  if (timeGridKnown) payload[28] |= 1U << 2;
  return enqueue(bridge::FrameType::TimeStatus, bridge::Priority::Time,
                 timestampUs, 0U, payload,
                 static_cast<uint16_t>(sizeof(payload)), false);
}

bool BridgeTransport::enqueueError(uint64_t timestampUs, uint16_t errorCode,
                                   uint32_t evidence0,
                                   uint32_t evidence1) {
  uint8_t payload[12];
  bridge::putU16Le(payload, errorCode);
  bridge::putU16Le(payload + 2U, 0U);
  bridge::putU32Le(payload + 4U, evidence0);
  bridge::putU32Le(payload + 8U, evidence1);
  return enqueue(bridge::FrameType::Error, bridge::Priority::Critical,
                 timestampUs, 0U, payload,
                 static_cast<uint16_t>(sizeof(payload)), false);
}

bool BridgeTransport::enqueueHeartbeat(uint64_t timestampUs,
                                       uint32_t uptimeMs) {
  uint8_t payload[4];
  bridge::putU32Le(payload, uptimeMs);
  return enqueue(bridge::FrameType::Heartbeat, bridge::Priority::Status,
                 timestampUs, 0U, payload,
                 static_cast<uint16_t>(sizeof(payload)), false);
}

void BridgeTransport::pump() {
  if (!queue_.activateFront()) return;
  const int available = serial_.availableForWrite();
  if (available <= 0) return;
  const size_t remaining = queue_.frontRemaining();
  const size_t amount = remaining < static_cast<size_t>(available)
                            ? remaining
                            : static_cast<size_t>(available);
  const bridge::Priority priority = queue_.frontPriority();
  const size_t written = serial_.write(queue_.frontData(), amount);
  const size_t accepted = written < amount ? written : amount;
  const bool completed = queue_.advance(accepted);
  stats_.uartOutputBytes += static_cast<uint32_t>(accepted);
  if (completed) {
    ++stats_.framesSent;
    if (priority == bridge::Priority::L1s) ++stats_.l1sSent;
  }
}

#endif

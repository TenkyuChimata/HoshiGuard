#pragma once

#include "config.h"

#if defined(ARDUINO)
#include <Arduino.h>

#include "bridge_protocol.h"
#include "tim_tp.h"

#ifndef BRIDGE_QUEUE_DEPTH
#define BRIDGE_QUEUE_DEPTH 7U
#endif
#ifndef BRIDGE_L1S_RESERVED_SLOTS
#define BRIDGE_L1S_RESERVED_SLOTS 4U
#endif

struct BridgeTransportStats {
  uint32_t framesGenerated;
  uint32_t framesQueued;
  uint32_t framesSent;
  uint32_t framesDropped;
  uint32_t criticalDropped;
  uint32_t l1sGenerated;
  uint32_t l1sQueued;
  uint32_t l1sSent;
  uint32_t l1sDropped;
  uint32_t uartOutputBytes;
  uint32_t queueHighWatermark;
  uint32_t encodeErrors;
};

struct BridgeL1sMetadata {
  uint32_t ppsSequence;
  uint8_t svId;
  uint8_t gnssId;
  uint8_t sigId;
  uint8_t numWords;
  uint8_t pab;
  uint8_t messageType;
  uint16_t flags;
  uint8_t timeQuality;
  uint8_t ppsState;
};

struct BridgeStatusPayload {
  uint32_t uptimeMs;
  uint32_t currentBaud;
  uint8_t gnssState;
  uint8_t ppsState;
  uint8_t timeQuality;
  uint8_t lastError;
  uint32_t ubxReceived;
  uint32_t ubxChecksumErrors;
  uint32_t sfrbxReceived;
  uint32_t qzssL1sReceived;
  uint32_t validL1s;
  uint32_t ppsEdges;
  uint32_t ppsLostTotal;
  uint32_t ppsAbnormalTotal;
};

class BridgeTransport {
 public:
  explicit BridgeTransport(HardwareSerial &serial);
  void begin();
  bool enqueueHello(uint64_t timestampUs, uint32_t currentBaud);
  bool enqueueL1s(uint64_t timestampUs, const BridgeL1sMetadata &metadata,
                  const uint8_t *fullUbx, uint16_t fullUbxLength);
  bool enqueueStatus(uint64_t timestampUs,
                     const BridgeStatusPayload &status);
  bool enqueueTimeStatus(uint64_t timestampUs, uint32_t ppsSequence,
                         uint8_t ppsState, uint8_t quality, uint8_t reason,
                         const UbxTimTp &timTp, uint32_t matchedPpsSequence,
                         bool matched, bool ambiguous, bool timeGridKnown);
  bool enqueueError(uint64_t timestampUs, uint16_t errorCode,
                    uint32_t evidence0, uint32_t evidence1);
  bool enqueueHeartbeat(uint64_t timestampUs, uint32_t uptimeMs);
  void pump();
  const BridgeTransportStats &stats() const { return stats_; }
  uint32_t nextSequence() const { return nextSequence_; }

 private:
  bool enqueue(bridge::FrameType type, bridge::Priority priority,
               uint64_t timestampUs, uint16_t headerFlags,
               const uint8_t *payload, uint16_t payloadLength,
               bool validL1s);
  HardwareSerial &serial_;
  bridge::RecordQueue<BRIDGE_QUEUE_DEPTH, bridge::kMaximumEncodedFrame,
                      BRIDGE_L1S_RESERVED_SLOTS> queue_;
  // The bridge is driven only from the main loop. Keeping the encode workspace
  // in this single transport instance avoids placing three complete frames on
  // the STM32F103 stack for every received L1S packet.
  uint8_t logicalScratch_[bridge::kMaximumLogicalFrame];
  uint8_t wireScratch_[bridge::kMaximumEncodedFrame];
  uint8_t l1sPayloadScratch_[20U + bridge::kMaximumUbxLength];
  uint32_t nextSequence_;
  BridgeTransportStats stats_;
};

#endif

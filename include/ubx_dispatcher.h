#pragma once

#include <stddef.h>
#include <stdint.h>

#include "qzss_l1s.h"
#include "ubx_parser.h"

struct SatelliteFrameStats {
  uint32_t sfrbxTotal;
  uint32_t gpsSfrbx;
  uint32_t qzssSfrbx;
  uint32_t qzssL1caSfrbx;
  uint32_t qzssL1sSfrbx;
  uint32_t invalidSfrbxLength;
  uint32_t invalidWordCount;
  uint32_t validPab;
  uint32_t invalidPab;
  uint32_t validCrc;
  uint32_t invalidCrc;
  uint32_t mt43;
  uint32_t mt44;
  uint32_t unsupportedMt;
  uint32_t duplicateFrames;
  uint32_t sfrbxByGnss[9];
  uint32_t qzssBySv[16];
  uint8_t lastSvId;
};

struct UbxDispatchEvent {
  bool isSfrbx;
  bool hasHeader;
  bool hasL1s;
  SfrbxHeader header;
  QzssL1sFrame frame;
};

class UbxFrameDispatcher {
 public:
  UbxFrameDispatcher();
  bool processPacket(const UbxPacketView &packet, uint64_t receivedMicros,
                     uint32_t ppsSequence, UbxDispatchEvent &event);
  const SatelliteFrameStats &stats() const { return stats_; }

 private:
  SatelliteFrameStats stats_;
  L1sDeduplicator deduplicator_;
};

struct UbxInjectionResult {
  uint32_t validPackets;
  uint32_t sfrbxPackets;
  uint32_t l1sFrames;
  bool hasLastFrame;
  QzssL1sFrame lastFrame;
};

// テスト・再生用の入口。UART 経路と同じ UbxParser と dispatcher を使い、HardwareSerial に依存しない。
void injectUbxBytes(const uint8_t *data, size_t length, UbxParser &parser,
                    UbxFrameDispatcher &dispatcher,
                    UbxInjectionResult &result,
                    uint64_t receivedMicros = 0U,
                    uint32_t ppsSequence = 0U);

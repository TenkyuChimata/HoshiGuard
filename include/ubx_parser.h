#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef UBX_MAX_PAYLOAD
#define UBX_MAX_PAYLOAD 256U
#endif

struct UbxPacketView {
  uint8_t messageClass;
  uint8_t messageId;
  uint16_t length;
  const uint8_t *payload;
};

struct UbxParserStats {
  uint32_t receivedBytes;
  uint32_t validPackets;
  uint32_t checksumErrors;
  uint32_t oversizedPackets;
  uint32_t resyncs;
  uint32_t nmeaBytesSkipped;
};

class UbxParser {
 public:
  UbxParser();

  // Returns true exactly once for each checksum-valid packet. The payload view
  // remains valid until more bytes are fed to this parser.
  bool feed(uint8_t byte, UbxPacketView &packet);
  void reset();
  // Discard a partial packet/NMEA line without changing cumulative counters.
  void resetStreamState();
  const UbxParserStats &stats() const { return stats_; }

  static void checksum(const uint8_t *data, size_t length,
                       uint8_t &ckA, uint8_t &ckB);
  static size_t buildPacket(uint8_t messageClass, uint8_t messageId,
                            const uint8_t *payload, uint16_t payloadLength,
                            uint8_t *output, size_t outputCapacity);

 private:
  enum State : uint8_t {
    SYNC1,
    SYNC2,
    CLASS,
    ID,
    LENGTH_LOW,
    LENGTH_HIGH,
    PAYLOAD,
    CHECKSUM_A,
    CHECKSUM_B
  };

  void checksumByte(uint8_t byte);
  void resyncFrom(uint8_t byte);

  State state_;
  uint8_t messageClass_;
  uint8_t messageId_;
  uint16_t payloadLength_;
  uint16_t payloadIndex_;
  uint8_t ckA_;
  uint8_t ckB_;
  uint8_t receivedCkA_;
  uint8_t payload_[UBX_MAX_PAYLOAD];
  UbxParserStats stats_;
  bool nmeaActive_;
};

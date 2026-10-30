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

  // checksum が正しい packet ごとに一度だけ true を返す。payload の参照は次の byte を入力するまで有効。
  bool feed(uint8_t byte, UbxPacketView &packet);
  void reset();
  // 累積カウンターを変えず、途中の packet/NMEA 行を破棄する。
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

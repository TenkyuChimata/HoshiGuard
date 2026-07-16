#pragma once

#include <stddef.h>
#include <stdint.h>

static const size_t QZSS_L1S_RAW_BYTES = 32U;
static const size_t QZSS_L1S_VALID_BITS = 250U;
static const size_t QZSS_L1S_CRC_DATA_BITS = 226U;
static const uint8_t QZSS_L1S_CONTENT_WORDS = 8U;
static const uint8_t QZSS_L1S_M10_WORDS = 9U;
static const uint8_t QZSS_L1S_LEGACY_WORDS = 8U;

class BitReader {
 public:
  // QZSS bit numbering: bit 0 is the MSB of byte 0.
  BitReader(const uint8_t *data, size_t bitLength)
      : data_(data), bitLength_(bitLength) {}
  bool read(size_t bitOffset, uint8_t bitCount, uint32_t &value) const;

 private:
  const uint8_t *data_;
  size_t bitLength_;
};

struct SfrbxHeader {
  uint8_t gnssId;
  uint8_t svId;
  uint8_t sigId;
  uint8_t freqId;
  uint8_t numWords;
  uint8_t channel;
  uint8_t version;
};

enum class SfrbxParseStatus : uint8_t {
  Ok,
  TooShort,
  InvalidWordCount,
  LengthMismatch
};

struct QzssL1sFrame {
  uint8_t raw[QZSS_L1S_RAW_BYTES];
  uint8_t svId;
  uint8_t pab;
  uint8_t messageType;
  uint8_t numWords;
  // MAX-M10 SPG 5.10 reports a ninth dwrd. The first eight dwrd contain the
  // 250-bit L1S message plus six spare bits; preserve the extra UBX bytes for
  // diagnostics without treating them as L1S fields.
  uint8_t extraWordUbx[4];
  bool hasExtraWord;
  bool validPab;
  bool validCrc;
  bool duplicate;
  uint64_t receivedMicros;
  uint32_t ppsSequence;
};

bool isSupportedQzssL1sWordCount(uint8_t numWords);

bool parseSfrbxHeader(const uint8_t *payload, uint16_t payloadLength,
                      SfrbxHeader &header);
SfrbxParseStatus parseSfrbxHeaderDetailed(const uint8_t *payload,
                                          uint16_t payloadLength,
                                          SfrbxHeader &header);
bool extractQzssL1s(const uint8_t *payload, uint16_t payloadLength,
                    QzssL1sFrame &frame);
bool isValidL1sPreamble(uint8_t pab);
uint32_t qzssL1sCrc24(const uint8_t raw[QZSS_L1S_RAW_BYTES]);
uint32_t qzssL1sReceivedCrc(const uint8_t raw[QZSS_L1S_RAW_BYTES]);
uint32_t qzssL1sHash(const uint8_t raw[QZSS_L1S_RAW_BYTES]);

class L1sDeduplicator {
 public:
  L1sDeduplicator();
  bool checkAndRemember(const uint8_t raw[QZSS_L1S_RAW_BYTES],
                        uint64_t receivedMicros,
                        uint64_t windowMicros);

 private:
  static const size_t CAPACITY = 8U;
  struct Entry {
    uint8_t raw[QZSS_L1S_RAW_BYTES];
    uint32_t hash;
    uint64_t receivedMicros;
    bool used;
  } entries_[CAPACITY];
  size_t next_;
};

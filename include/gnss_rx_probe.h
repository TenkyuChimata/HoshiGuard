#pragma once

#include <stddef.h>
#include <stdint.h>

enum class GnssRxClassification : uint8_t {
  NoData,
  ValidUbx,
  LikelyNmea,
  LikelyText,
  LikelyWrongBaud,
  UnknownBinary
};

enum class GnssRxEvidence : uint8_t {
  None,
  UbxChecksumValid,
  NmeaChecksumValid,
  NmeaFraming,
  MostlyPrintable,
  MostlyNonPrintable,
  IdleByteDominated,
  BinaryUnclassified
};

const char *gnssRxClassificationName(GnssRxClassification value);
const char *gnssRxEvidenceName(GnssRxEvidence value);

struct NmeaProbeStats {
  uint32_t candidateLines;
  uint32_t validChecksum;
  uint32_t invalidChecksum;
  uint32_t incompleteLines;
  uint32_t overlongLines;
  uint32_t recognizedSentenceLines;
  uint8_t sentenceIdCount;
  char sentenceIds[4][6];
};

struct GnssRxProbeStats {
  uint32_t receivedBytes;
  uint32_t printableBytes;
  uint32_t dollarBytes;
  uint32_t crlfPairs;
  uint32_t ubxSyncPairs;
  uint32_t zeroBytes;
  uint32_t ffBytes;
  uint32_t highBitBytes;
  uint32_t validUbxPackets;
  NmeaProbeStats nmea;
};

struct GnssRxProbeResult {
  GnssRxProbeStats stats;
  GnssRxClassification classification;
  GnssRxEvidence evidence;
};

class GnssRxProbe {
 public:
  static const size_t SAMPLE_CAPACITY = 128U;
  static const size_t NMEA_LINE_CAPACITY = 96U;

  GnssRxProbe();
  void reset();
  void feed(uint8_t byte);
  void noteValidUbx();
  GnssRxProbeResult finish();
  GnssRxProbeResult result() const;
  const uint8_t *sample() const { return sample_; }
  size_t sampleLength() const { return sampleLength_; }

 private:
  void beginNmeaLine();
  void finishNmeaLine();
  bool rememberSentenceId();

  GnssRxProbeStats stats_;
  uint8_t sample_[SAMPLE_CAPACITY];
  size_t sampleLength_;
  char nmeaLine_[NMEA_LINE_CAPACITY];
  size_t nmeaLength_;
  bool nmeaActive_;
  bool nmeaOverlong_;
  uint8_t previousByte_;
  bool havePrevious_;
  GnssRxClassification classification_;
  GnssRxEvidence evidence_;
};

// 生の制御文字は出力しない。容量が 0 以外なら必ず終端し、戻り値に終端文字を含めない。
size_t formatGnssRxAscii(const uint8_t *data, size_t length,
                         char *output, size_t capacity);
size_t formatGnssRxHex(const uint8_t *data, size_t length,
                       char *output, size_t capacity);

bool nmeaInferredBaud(const GnssRxProbeResult &result);

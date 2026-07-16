#include "gnss_rx_probe.h"

#include <string.h>

namespace {
bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

uint8_t hexValue(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
  return static_cast<uint8_t>(c - 'a' + 10);
}

bool isTextByte(uint8_t b) {
  return (b >= 0x20U && b <= 0x7EU) || b == '\r' || b == '\n' || b == '\t';
}

GnssRxProbeResult classify(const GnssRxProbeStats &s) {
  GnssRxProbeResult result = {};
  result.stats = s;
  if (s.validUbxPackets != 0U) {
    result.classification = GnssRxClassification::ValidUbx;
    result.evidence = GnssRxEvidence::UbxChecksumValid;
    return result;
  }
  if (s.receivedBytes == 0U) {
    result.classification = GnssRxClassification::NoData;
    result.evidence = GnssRxEvidence::None;
    return result;
  }
  const uint32_t textPercent =
      static_cast<uint32_t>((static_cast<uint64_t>(s.printableBytes) * 100U) /
                            s.receivedBytes);
  const uint32_t highPercent =
      static_cast<uint32_t>((static_cast<uint64_t>(s.highBitBytes) * 100U) /
                            s.receivedBytes);
  const uint32_t idlePercent = static_cast<uint32_t>(
      (static_cast<uint64_t>(s.zeroBytes + s.ffBytes) * 100U) /
      s.receivedBytes);
  if (s.nmea.validChecksum != 0U && s.nmea.recognizedSentenceLines != 0U &&
      s.dollarBytes != 0U &&
      s.crlfPairs != 0U) {
    result.classification = GnssRxClassification::LikelyNmea;
    result.evidence = GnssRxEvidence::NmeaChecksumValid;
  } else if (s.nmea.candidateLines >= 2U &&
             s.nmea.recognizedSentenceLines >= 2U && s.dollarBytes >= 2U &&
             s.crlfPairs >= 2U && textPercent >= 75U) {
    result.classification = GnssRxClassification::LikelyNmea;
    result.evidence = GnssRxEvidence::NmeaFraming;
  } else if (textPercent >= 85U && s.crlfPairs != 0U) {
    result.classification = GnssRxClassification::LikelyText;
    result.evidence = GnssRxEvidence::MostlyPrintable;
  } else if (s.receivedBytes >= 24U && idlePercent >= 30U &&
             s.dollarBytes == 0U && s.crlfPairs == 0U) {
    result.classification = GnssRxClassification::LikelyWrongBaud;
    result.evidence = GnssRxEvidence::IdleByteDominated;
  } else if (s.receivedBytes >= 24U && textPercent < 40U && highPercent >= 35U &&
             s.dollarBytes == 0U && s.crlfPairs == 0U) {
    result.classification = GnssRxClassification::LikelyWrongBaud;
    result.evidence = GnssRxEvidence::MostlyNonPrintable;
  } else {
    result.classification = GnssRxClassification::UnknownBinary;
    result.evidence = GnssRxEvidence::BinaryUnclassified;
  }
  return result;
}
}  // namespace

const char *gnssRxClassificationName(GnssRxClassification value) {
  switch (value) {
    case GnssRxClassification::NoData: return "NoData";
    case GnssRxClassification::ValidUbx: return "ValidUbx";
    case GnssRxClassification::LikelyNmea: return "LikelyNmea";
    case GnssRxClassification::LikelyText: return "LikelyText";
    case GnssRxClassification::LikelyWrongBaud: return "LikelyWrongBaud";
    case GnssRxClassification::UnknownBinary: return "UnknownBinary";
  }
  return "UnknownBinary";
}

const char *gnssRxEvidenceName(GnssRxEvidence value) {
  switch (value) {
    case GnssRxEvidence::None: return "no_data";
    case GnssRxEvidence::UbxChecksumValid: return "valid_ubx_checksum";
    case GnssRxEvidence::NmeaChecksumValid: return "valid_nmea_checksum";
    case GnssRxEvidence::NmeaFraming: return "repeated_nmea_framing";
    case GnssRxEvidence::MostlyPrintable: return "mostly_printable_text";
    case GnssRxEvidence::MostlyNonPrintable: return "mostly_non_printable";
    case GnssRxEvidence::IdleByteDominated: return "many_00_or_ff_bytes";
    case GnssRxEvidence::BinaryUnclassified: return "unclassified_binary";
  }
  return "unclassified_binary";
}

GnssRxProbe::GnssRxProbe() { reset(); }

void GnssRxProbe::reset() {
  memset(&stats_, 0, sizeof(stats_));
  memset(sample_, 0, sizeof(sample_));
  memset(nmeaLine_, 0, sizeof(nmeaLine_));
  sampleLength_ = 0U;
  nmeaLength_ = 0U;
  nmeaActive_ = false;
  nmeaOverlong_ = false;
  previousByte_ = 0U;
  havePrevious_ = false;
  classification_ = GnssRxClassification::NoData;
  evidence_ = GnssRxEvidence::None;
}

void GnssRxProbe::beginNmeaLine() {
  if (nmeaActive_) ++stats_.nmea.incompleteLines;
  nmeaActive_ = true;
  nmeaOverlong_ = false;
  nmeaLength_ = 0U;
  nmeaLine_[nmeaLength_++] = '$';
}

bool GnssRxProbe::rememberSentenceId() {
  if (nmeaLength_ < 6U) return false;
  char id[6];
  memcpy(id, nmeaLine_ + 1U, 5U);
  id[5] = '\0';
  const bool recognized =
      id[0] == 'G' && (id[1] == 'N' || id[1] == 'P' || id[1] == 'Q') &&
      id[2] >= 'A' && id[2] <= 'Z' && id[3] >= 'A' && id[3] <= 'Z' &&
      id[4] >= 'A' && id[4] <= 'Z';
  if (!recognized) return false;
  if (stats_.nmea.sentenceIdCount >= 4U) return true;
  for (uint8_t i = 0U; i < stats_.nmea.sentenceIdCount; ++i)
    if (memcmp(stats_.nmea.sentenceIds[i], id, sizeof(id)) == 0) return true;
  memcpy(stats_.nmea.sentenceIds[stats_.nmea.sentenceIdCount], id, sizeof(id));
  ++stats_.nmea.sentenceIdCount;
  return true;
}

void GnssRxProbe::finishNmeaLine() {
  nmeaActive_ = false;
  if (nmeaOverlong_) {
    ++stats_.nmea.overlongLines;
    return;
  }
  ++stats_.nmea.candidateLines;
  if (rememberSentenceId()) ++stats_.nmea.recognizedSentenceLines;
  if (nmeaLength_ < 7U || nmeaLine_[nmeaLength_ - 2U] != '\r') {
    ++stats_.nmea.invalidChecksum;
    return;
  }
  size_t star = nmeaLength_;
  for (size_t i = 1U; i < nmeaLength_; ++i) {
    if (nmeaLine_[i] == '*') star = i;
  }
  if (star + 5U != nmeaLength_ || !isHex(nmeaLine_[star + 1U]) ||
      !isHex(nmeaLine_[star + 2U])) {
    ++stats_.nmea.invalidChecksum;
    return;
  }
  uint8_t checksum = 0U;
  for (size_t i = 1U; i < star; ++i)
    checksum = static_cast<uint8_t>(checksum ^
                                    static_cast<uint8_t>(nmeaLine_[i]));
  const uint8_t received = static_cast<uint8_t>(
      (hexValue(nmeaLine_[star + 1U]) << 4) | hexValue(nmeaLine_[star + 2U]));
  if (checksum == received) ++stats_.nmea.validChecksum;
  else ++stats_.nmea.invalidChecksum;
}

void GnssRxProbe::feed(uint8_t byte) {
  ++stats_.receivedBytes;
  if (sampleLength_ < sizeof(sample_)) sample_[sampleLength_++] = byte;
  if (isTextByte(byte)) ++stats_.printableBytes;
  if (byte == '$') ++stats_.dollarBytes;
  if (byte == 0U) ++stats_.zeroBytes;
  if (byte == 0xFFU) ++stats_.ffBytes;
  if ((byte & 0x80U) != 0U) ++stats_.highBitBytes;
  if (havePrevious_) {
    if (previousByte_ == '\r' && byte == '\n') ++stats_.crlfPairs;
    if (previousByte_ == 0xB5U && byte == 0x62U) ++stats_.ubxSyncPairs;
  }
  previousByte_ = byte;
  havePrevious_ = true;

  if (byte == '$') {
    beginNmeaLine();
    return;
  }
  if (!nmeaActive_) return;
  if (nmeaLength_ < sizeof(nmeaLine_)) {
    nmeaLine_[nmeaLength_++] = static_cast<char>(byte);
  } else {
    nmeaOverlong_ = true;
  }
  if (byte == '\n') finishNmeaLine();
}

void GnssRxProbe::noteValidUbx() { ++stats_.validUbxPackets; }

GnssRxProbeResult GnssRxProbe::finish() {
  if (nmeaActive_) {
    ++stats_.nmea.incompleteLines;
    nmeaActive_ = false;
  }
  const GnssRxProbeResult classified = classify(stats_);
  classification_ = classified.classification;
  evidence_ = classified.evidence;
  return classified;
}

GnssRxProbeResult GnssRxProbe::result() const {
  GnssRxProbeResult classified = classify(stats_);
  classified.classification = classification_ == GnssRxClassification::NoData
                                  ? classified.classification : classification_;
  classified.evidence = evidence_ == GnssRxEvidence::None
                            ? classified.evidence : evidence_;
  return classified;
}

size_t formatGnssRxAscii(const uint8_t *data, size_t length,
                         char *output, size_t capacity) {
  if (output == 0 || capacity == 0U) return 0U;
  size_t written = 0U;
  for (size_t i = 0U; data != 0 && i < length; ++i) {
    const char *escape = 0;
    if (data[i] == '\r') escape = "\\r";
    else if (data[i] == '\n') escape = "\\n";
    else if (data[i] == '\t') escape = "\\t";
    const size_t needed = escape == 0 ? 1U : 2U;
    if (written + needed >= capacity) break;
    if (escape != 0) {
      output[written++] = escape[0];
      output[written++] = escape[1];
    } else {
      output[written++] = (data[i] >= 0x20U && data[i] <= 0x7EU)
                              ? static_cast<char>(data[i]) : '.';
    }
  }
  output[written] = '\0';
  return written;
}

size_t formatGnssRxHex(const uint8_t *data, size_t length,
                       char *output, size_t capacity) {
  static const char digits[] = "0123456789ABCDEF";
  if (output == 0 || capacity == 0U) return 0U;
  size_t written = 0U;
  for (size_t i = 0U; data != 0 && i < length; ++i) {
    if (written + 2U >= capacity) break;
    output[written++] = digits[data[i] >> 4];
    output[written++] = digits[data[i] & 0x0FU];
  }
  output[written] = '\0';
  return written;
}

bool nmeaInferredBaud(const GnssRxProbeResult &result) {
  return result.classification == GnssRxClassification::LikelyNmea &&
         result.stats.nmea.validChecksum >= 2U;
}

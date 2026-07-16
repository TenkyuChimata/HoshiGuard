#include "raw_capture.h"

#include <stdio.h>
#include <string.h>

#ifndef ARDUINO
#include <stdlib.h>
#endif

namespace {
const char kHex[] = "0123456789ABCDEF";

bool appendHexByte(char *output, size_t capacity, size_t &position,
                   uint8_t value) {
  if (position + 2U >= capacity) return false;
  output[position++] = kHex[value >> 4];
  output[position++] = kHex[value & 0x0FU];
  return true;
}

bool appendBytes(char *output, size_t capacity, size_t &position,
                 const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (!appendHexByte(output, capacity, position, data[i])) return false;
  return true;
}

bool appendText(char *output, size_t capacity, size_t &position,
                const char *text) {
  if (text == 0) return false;
  while (*text != '\0') {
    if (position + 1U >= capacity) return false;
    output[position++] = *text++;
  }
  return true;
}

bool appendUint64Decimal(char *output, size_t capacity, size_t &position,
                         uint64_t value) {
  char reversed[20];
  size_t count = 0U;
  do {
    reversed[count++] = static_cast<char>('0' + value % 10U);
    value /= 10U;
  } while (value != 0U && count < sizeof(reversed));
  if (position + count >= capacity) return false;
  while (count != 0U) output[position++] = reversed[--count];
  return true;
}

void checksumByte(uint8_t value, uint8_t &a, uint8_t &b) {
  a = static_cast<uint8_t>(a + value);
  b = static_cast<uint8_t>(b + a);
}

#ifndef ARDUINO
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
#endif
}  // namespace

size_t encodeHexUpper(const uint8_t *data, size_t length, char *output,
                      size_t capacity) {
  if (output == 0 || (length != 0U && data == 0) ||
      capacity < length * 2U + 1U) return 0U;
  size_t position = 0U;
  if (!appendBytes(output, capacity, position, data, length)) return 0U;
  output[position] = '\0';
  return position;
}

size_t formatUbxRawRecord(uint64_t timestampUs, const UbxPacketView &packet,
                          char *output, size_t capacity) {
  if (output == 0 || capacity == 0U ||
      (packet.length != 0U && packet.payload == 0)) return 0U;
  const size_t packetLength = static_cast<size_t>(packet.length) + 8U;
  size_t position = 0U;
  if (!appendText(output, capacity, position, "UBX_RAW timestamp_us=") ||
      !appendUint64Decimal(output, capacity, position, timestampUs)) return 0U;
  const SafeFormatResult suffix = safeSnprintf(
      output + position, capacity - position, " len=%u data=",
      static_cast<unsigned>(packetLength));
  if (suffix.status != SafeFormatStatus::Complete) return 0U;
  position += suffix.length;
  uint8_t ckA = 0U, ckB = 0U;
  const uint8_t header[] = {
      packet.messageClass, packet.messageId,
      static_cast<uint8_t>(packet.length),
      static_cast<uint8_t>(packet.length >> 8)};
  if (!appendHexByte(output, capacity, position, 0xB5U) ||
      !appendHexByte(output, capacity, position, 0x62U) ||
      !appendBytes(output, capacity, position, header, sizeof(header)))
    return 0U;
  for (size_t i = 0; i < sizeof(header); ++i) checksumByte(header[i], ckA, ckB);
  for (uint16_t i = 0; i < packet.length; ++i) {
    if (!appendHexByte(output, capacity, position, packet.payload[i])) return 0U;
    checksumByte(packet.payload[i], ckA, ckB);
  }
  if (!appendHexByte(output, capacity, position, ckA) ||
      !appendHexByte(output, capacity, position, ckB) ||
      position + 1U >= capacity) return 0U;
  output[position++] = '\n';
  output[position] = '\0';
  return position;
}

size_t formatQzssL1sUbxRecord(uint64_t timestampUs, uint32_t ppsSequence,
                              const SfrbxHeader &header,
                              const QzssL1sFrame &frame,
                              const UbxPacketView &packet,
                              char *output, size_t capacity) {
  if (output == 0 || capacity == 0U || packet.payload == 0 ||
      packet.messageClass != 0x02U || packet.messageId != 0x13U)
    return 0U;
  const size_t packetLength = static_cast<size_t>(packet.length) + 8U;
  size_t position = 0U;
  if (!appendText(output, capacity, position,
                  "QZSS_L1S_UBX timestamp_us=") ||
      !appendUint64Decimal(output, capacity, position, timestampUs)) return 0U;
  const SafeFormatResult prefix = safeSnprintf(
      output + position, capacity - position,
      " pps_seq=%lu svId=%u numWords=%u pab=%02X mt=%u crc=%s len=%u data=",
      static_cast<unsigned long>(ppsSequence), header.svId, header.numWords,
      frame.pab, frame.messageType, frame.validCrc ? "ok" : "bad",
      static_cast<unsigned>(packetLength));
  if (prefix.status != SafeFormatStatus::Complete) return 0U;
  position += prefix.length;
  uint8_t ckA = 0U, ckB = 0U;
  const uint8_t ubxHeader[] = {
      packet.messageClass, packet.messageId,
      static_cast<uint8_t>(packet.length),
      static_cast<uint8_t>(packet.length >> 8)};
  if (!appendHexByte(output, capacity, position, 0xB5U) ||
      !appendHexByte(output, capacity, position, 0x62U) ||
      !appendBytes(output, capacity, position, ubxHeader, sizeof(ubxHeader)))
    return 0U;
  for (size_t i = 0U; i < sizeof(ubxHeader); ++i)
    checksumByte(ubxHeader[i], ckA, ckB);
  for (uint16_t i = 0U; i < packet.length; ++i) {
    if (!appendHexByte(output, capacity, position, packet.payload[i])) return 0U;
    checksumByte(packet.payload[i], ckA, ckB);
  }
  if (!appendHexByte(output, capacity, position, ckA) ||
      !appendHexByte(output, capacity, position, ckB) ||
      position + 1U >= capacity) return 0U;
  output[position++] = '\n';
  output[position] = '\0';
  return position;
}

size_t formatSfrbxRecord(uint64_t timestampUs, const SfrbxHeader &header,
                         const uint8_t *payload, uint16_t payloadLength,
                         char *output, size_t capacity) {
  if (payload == 0 || payloadLength < 8U || output == 0) return 0U;
  size_t position = 0U;
  if (!appendText(output, capacity, position, "SFRBX timestamp_us=") ||
      !appendUint64Decimal(output, capacity, position, timestampUs)) return 0U;
  const SafeFormatResult prefix = safeSnprintf(
      output + position, capacity - position,
      " gnssId=%u svId=%u sigId=%u freqId=%u numWords=%u raw=",
      header.gnssId, header.svId,
      header.sigId, header.freqId, header.numWords);
  if (prefix.status != SafeFormatStatus::Complete) return 0U;
  position += prefix.length;
  if (!appendBytes(output, capacity, position, payload + 8U,
                   payloadLength - 8U) || position + 1U >= capacity) return 0U;
  output[position++] = '\n';
  output[position] = '\0';
  return position;
}

size_t formatL1sRecord(const QzssL1sFrame &frame, char *output,
                       size_t capacity) {
  if (output == 0) return 0U;
  size_t position = 0U;
  if (!appendText(output, capacity, position, "L1S timestamp_us=") ||
      !appendUint64Decimal(output, capacity, position, frame.receivedMicros))
    return 0U;
  const SafeFormatResult prefix = safeSnprintf(
      output + position, capacity - position,
      " pps_seq=%lu svId=%u pab=%02X mt=%u crc=%s raw=",
      static_cast<unsigned long>(frame.ppsSequence), frame.svId, frame.pab,
      frame.messageType, frame.validCrc ? "ok" : "bad");
  if (prefix.status != SafeFormatStatus::Complete) return 0U;
  position += prefix.length;
  if (!appendBytes(output, capacity, position, frame.raw, sizeof(frame.raw)) ||
      position + 1U >= capacity) return 0U;
  output[position++] = '\n';
  output[position] = '\0';
  return position;
}

size_t formatSfrbxHeaderRecord(uint64_t timestampUs,
                               const SfrbxHeader &header,
                               char *output, size_t capacity) {
  if (output == 0 || capacity == 0U) return 0U;
  size_t position = 0U;
  if (!appendText(output, capacity, position, "SFRBX_HDR timestamp_us=") ||
      !appendUint64Decimal(output, capacity, position, timestampUs)) return 0U;
  const SafeFormatResult formatted = safeSnprintf(
      output + position, capacity - position,
      " gnssId=%u svId=%u sigId=%u freqId=%u numWords=%u\n",
      header.gnssId, header.svId,
      header.sigId, header.freqId, header.numWords);
  return formatted.status == SafeFormatStatus::Complete
             ? position + formatted.length : 0U;
}

size_t formatPpsRecord(uint32_t timestampUs, uint32_t sequence,
                       uint32_t intervalUs, uint32_t lost,
                       uint32_t abnormal, uint32_t consecutiveGood,
                       char *output, size_t capacity) {
  if (output == 0 || capacity == 0U) return 0U;
  const SafeFormatResult formatted = safeSnprintf(
      output, capacity,
      "PPS timestamp_us=%lu seq=%lu interval_us=%lu pps_lost_total=%lu "
      "pps_abnormal_total=%lu pps_consecutive_good=%lu\n",
      static_cast<unsigned long>(timestampUs),
      static_cast<unsigned long>(sequence),
      static_cast<unsigned long>(intervalUs),
      static_cast<unsigned long>(lost),
      static_cast<unsigned long>(abnormal),
      static_cast<unsigned long>(consecutiveGood));
  return formatted.status == SafeFormatStatus::Complete ? formatted.length : 0U;
}

size_t formatTimTpRecord(uint32_t timestampUs, uint32_t previousPpsSequence,
                         const UbxTimTp &message, char *output,
                         size_t capacity) {
  if (output == 0 || capacity == 0U) return 0U;
  const SafeFormatResult formatted = safeSnprintf(
      output, capacity,
      "TIMTP timestamp_us=%lu previous_pps_seq=%lu tow_ms=%lu "
      "tow_sub_ms=%lu qerr_ps=%ld week=%u flags=%02X ref_info=%02X "
      "time_base=%s utc_available=%s locked=%s qerr_valid=%s\n",
      static_cast<unsigned long>(timestampUs),
      static_cast<unsigned long>(previousPpsSequence),
      static_cast<unsigned long>(message.towMs),
      static_cast<unsigned long>(message.towSubMs),
      static_cast<long>(message.qErrPs), message.week, message.flags,
      message.refInfo, message.timeBaseUtc ? "UTC" : "GNSS",
      message.utcAvailable ? "yes" : "no",
      message.tpLocked ? "yes" : "no", message.qErrValid ? "yes" : "no");
  return formatted.status == SafeFormatStatus::Complete ? formatted.length : 0U;
}

size_t formatTimTpMatchRecord(uint32_t timestampUs, uint32_t ppsSequence,
                              bool matched, bool ambiguous,
                              const char *quality, const char *reason,
                              char *output, size_t capacity) {
  if (output == 0 || quality == 0 || reason == 0 || capacity == 0U) return 0U;
  const SafeFormatResult formatted = safeSnprintf(
      output, capacity,
      "TIMTP_MATCH timestamp_us=%lu pps_seq=%lu matched=%s ambiguous=%s "
      "quality=%s reason=%s\n",
      static_cast<unsigned long>(timestampUs),
      static_cast<unsigned long>(ppsSequence), matched ? "yes" : "no",
      ambiguous ? "yes" : "no", quality, reason);
  return formatted.status == SafeFormatStatus::Complete ? formatted.length : 0U;
}

#ifndef ARDUINO
bool parseUbxRawRecord(const char *line, uint8_t *packet,
                       size_t packetCapacity, size_t &packetLength,
                       uint64_t &timestampUs) {
  packetLength = 0U;
  timestampUs = 0U;
  if (line == 0 || packet == 0) return false;
  static const char prefix[] = "UBX_RAW timestamp_us=";
  static const char compactPrefix[] = "QZSS_L1S_UBX timestamp_us=";
  const char *timestampText = 0;
  if (strncmp(line, prefix, sizeof(prefix) - 1U) == 0)
    timestampText = line + sizeof(prefix) - 1U;
  else if (strncmp(line, compactPrefix, sizeof(compactPrefix) - 1U) == 0)
    timestampText = line + sizeof(compactPrefix) - 1U;
  else
    return false;
  char *end = 0;
  const unsigned long long timestamp = strtoull(timestampText, &end, 10);
  if (end == timestampText)
    return false;
  const char *lengthField = strstr(end, " len=");
  if (lengthField == 0) return false;
  const unsigned long declared = strtoul(lengthField + 5U, &end, 10);
  if (strncmp(end, " data=", 6U) != 0 || declared > packetCapacity) return false;
  const char *hex = end + 6U;
  const char *finish = hex;
  while (*finish != '\0' && *finish != '\r' && *finish != '\n') ++finish;
  if (static_cast<size_t>(finish - hex) != declared * 2U) return false;
  for (size_t i = 0; i < declared; ++i) {
    const int high = hexNibble(hex[i * 2U]);
    const int low = hexNibble(hex[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    packet[i] = static_cast<uint8_t>((high << 4) | low);
  }
  if (declared < 8U || packet[0] != 0xB5U || packet[1] != 0x62U) return false;
  const uint16_t payloadLength = static_cast<uint16_t>(
      static_cast<uint16_t>(packet[4]) |
      static_cast<uint16_t>(static_cast<uint16_t>(packet[5]) << 8));
  if (declared != static_cast<unsigned long>(payloadLength) + 8UL) return false;
  uint8_t ckA = 0U, ckB = 0U;
  UbxParser::checksum(packet + 2U, payloadLength + 4U, ckA, ckB);
  if (packet[declared - 2U] != ckA || packet[declared - 1U] != ckB)
    return false;
  packetLength = declared;
  timestampUs = static_cast<uint64_t>(timestamp);
  return true;
}
#endif

#ifdef ARDUINO
RawCaptureLogger::RawCaptureLogger(LogArbiter &log) : log_(log) {
  memset(&stats_, 0, sizeof(stats_));
#if RAW_CAPTURE_MODE
  windowBytes_ = 0U;
  windowStartMs_ = 0U;
  lastOutputBytes_ = 0U;
#endif
}

void RawCaptureLogger::begin(uint32_t nowMs) {
#if RAW_CAPTURE_MODE
  windowStartMs_ = nowMs;
#else
  (void)nowMs;
#endif
}

#if RAW_CAPTURE_MODE
void RawCaptureLogger::commitRecord(size_t length, LogPriority priority) {
  if (length == 0U || length >= RAW_CAPTURE_MAX_RECORD) {
    ++stats_.droppedRecords;
    return;
  }
  if (!log_.enqueueRecord(priority, record_, length)) {
    ++stats_.droppedRecords;
    return;
  }
  if (length > stats_.maxRecordLength) stats_.maxRecordLength = length;
}
#endif

void RawCaptureLogger::enqueueUbx(uint64_t timestampUs,
                                  const UbxPacketView &packet) {
#if RAW_CAPTURE_MODE && (RAW_CAPTURE_INCLUDE_ALL_UBX || RAW_CAPTURE_INCLUDE_PRIORITY_UBX)
  bool selected = RAW_CAPTURE_INCLUDE_ALL_UBX != 0;
#if RAW_CAPTURE_INCLUDE_PRIORITY_UBX
  selected = selected || packet.messageClass == 0x05U ||
      packet.messageClass == 0x06U || packet.messageClass == 0x0AU ||
      (packet.messageClass == 0x01U && packet.messageId == 0x07U) ||
      (packet.messageClass == 0x0DU && packet.messageId == 0x01U) ||
      (packet.messageClass == 0x02U && packet.messageId == 0x13U &&
       packet.length >= 8U && packet.payload != 0 && packet.payload[0] == 5U);
#endif
  if (!selected) return;
#if CAPTURE_PRIVACY_MODE || !CAPTURE_INCLUDE_NAV_PVT_RAW
  if (packet.messageClass == 0x01U && packet.messageId == 0x07U) return;
#endif
#if L1S_COMPACT_CAPTURE
  if (packet.messageClass == 0x02U && packet.messageId == 0x13U) return;
#endif
  commitRecord(formatUbxRawRecord(
      timestampUs, packet, record_, sizeof(record_)));
#else
  (void)timestampUs; (void)packet;
#endif
}

void RawCaptureLogger::enqueueQzssL1sUbx(
    uint64_t timestampUs, uint32_t ppsSequence, const SfrbxHeader &header,
    const QzssL1sFrame &frame, const UbxPacketView &packet) {
#if RAW_CAPTURE_MODE && L1S_COMPACT_CAPTURE
  commitRecord(formatQzssL1sUbxRecord(
      timestampUs, ppsSequence, header, frame, packet, record_,
      sizeof(record_)), LogPriority::QzssL1s);
#else
  (void)timestampUs; (void)ppsSequence; (void)header; (void)frame; (void)packet;
#endif
}

void RawCaptureLogger::enqueueSfrbx(uint64_t timestampUs,
                                    const SfrbxHeader &header,
                                    const uint8_t *payload,
                                    uint16_t payloadLength) {
#if RAW_CAPTURE_MODE && RAW_CAPTURE_INCLUDE_SFRBX
  if (RAW_CAPTURE_QZSS_ONLY && header.gnssId != 5U) return;
  commitRecord(formatSfrbxRecord(
      timestampUs, header, payload, payloadLength, record_, sizeof(record_)));
#else
  (void)timestampUs; (void)header; (void)payload; (void)payloadLength;
#endif
}

void RawCaptureLogger::enqueueSfrbxHeader(uint64_t timestampUs,
                                          const SfrbxHeader &header) {
#if RAW_CAPTURE_MODE && RAW_CAPTURE_INCLUDE_SFRBX_HEADERS
  commitRecord(formatSfrbxHeaderRecord(
      timestampUs, header, record_, sizeof(record_)));
#else
  (void)timestampUs; (void)header;
#endif
}

void RawCaptureLogger::enqueueL1s(const QzssL1sFrame &frame) {
#if RAW_CAPTURE_MODE && RAW_CAPTURE_INCLUDE_L1S
  commitRecord(formatL1sRecord(frame, record_, sizeof(record_)));
#else
  (void)frame;
#endif
}

void RawCaptureLogger::enqueuePps(uint32_t timestampUs, uint32_t sequence,
                                  uint32_t intervalUs, uint32_t lost,
                                  uint32_t abnormal,
                                  uint32_t consecutiveGood) {
#if RAW_CAPTURE_MODE && RAW_CAPTURE_INCLUDE_PPS
  commitRecord(formatPpsRecord(
      timestampUs, sequence, intervalUs, lost, abnormal, consecutiveGood, record_,
      sizeof(record_)));
#else
  (void)timestampUs; (void)sequence; (void)intervalUs; (void)lost;
  (void)abnormal; (void)consecutiveGood;
#endif
}

void RawCaptureLogger::enqueueTimTp(uint32_t timestampUs,
                                    uint32_t previousPpsSequence,
                                    const UbxTimTp &message) {
#if RAW_CAPTURE_MODE && RAW_CAPTURE_INCLUDE_TIMTP
  commitRecord(formatTimTpRecord(
      timestampUs, previousPpsSequence, message, record_, sizeof(record_)));
#else
  (void)timestampUs; (void)previousPpsSequence; (void)message;
#endif
}

void RawCaptureLogger::enqueueTimTpMatch(uint32_t timestampUs,
                                         uint32_t ppsSequence, bool matched,
                                         bool ambiguous, const char *quality,
                                         const char *reason) {
#if RAW_CAPTURE_MODE && RAW_CAPTURE_INCLUDE_TIMTP_MATCH
  commitRecord(formatTimTpMatchRecord(
      timestampUs, ppsSequence, matched, ambiguous, quality, reason, record_,
      sizeof(record_)));
#else
  (void)timestampUs; (void)ppsSequence; (void)matched; (void)ambiguous;
  (void)quality; (void)reason;
#endif
}

void RawCaptureLogger::pump(uint32_t nowMs) {
  log_.pump();
#if RAW_CAPTURE_MODE
  if (static_cast<uint32_t>(nowMs - windowStartMs_) >= 1000U) {
    stats_.bytesPerSecond = windowBytes_;
    windowBytes_ = 0U;
    windowStartMs_ = nowMs;
  }
  const LogArbiterStats &logStats = log_.stats();
  const uint32_t delta = logStats.totalOutputBytes - lastOutputBytes_;
  lastOutputBytes_ = logStats.totalOutputBytes;
  stats_.totalOutputBytes += delta;
  windowBytes_ += delta;
  stats_.maxPendingBytes = logStats.maxPendingBytes;
  stats_.logRecordsQueued = logStats.recordsQueued;
  stats_.logRecordsDropped = logStats.recordsDropped;
  stats_.logMaxPending = logStats.maxPendingBytes;
  stats_.logInterleavePrevented = logStats.interleavePrevented;
  stats_.logFormatErrors = logStats.formatErrors;
  stats_.logFormatTruncated = logStats.formatTruncated;
  stats_.l1sRecordsGenerated = logStats.l1sRecordsGenerated;
  stats_.l1sRecordsQueued = logStats.l1sRecordsQueued;
  stats_.l1sRecordsSent = logStats.l1sRecordsSent;
  stats_.l1sRecordsDropped = logStats.l1sRecordsDropped;
  stats_.otherRecordsDropped = logStats.otherDropped;
  stats_.criticalRecordsDropped = logStats.criticalDropped;
  stats_.uartOutputBytes = logStats.totalOutputBytes;
  stats_.queueHighWatermark = logStats.queueHighWatermark;
#else
  (void)nowMs;
#endif
}

bool RawCaptureLogger::busy() const {
  return log_.busy();
}
#endif

#include "qzss_l1s.h"

#include <string.h>

bool BitReader::read(size_t bitOffset, uint8_t bitCount,
                     uint32_t &value) const {
  value = 0;
  if (data_ == 0 || bitCount > 32U || bitOffset > bitLength_ ||
      static_cast<size_t>(bitCount) > bitLength_ - bitOffset) {
    return false;
  }
  for (uint8_t i = 0; i < bitCount; ++i) {
    const size_t bit = bitOffset + i;
    value = (value << 1) |
            ((data_[bit >> 3] >> (7U - (bit & 7U))) & 1U);
  }
  return true;
}

bool parseSfrbxHeader(const uint8_t *payload, uint16_t payloadLength,
                      SfrbxHeader &header) {
  return parseSfrbxHeaderDetailed(payload, payloadLength, header) ==
         SfrbxParseStatus::Ok;
}

SfrbxParseStatus parseSfrbxHeaderDetailed(const uint8_t *payload,
                                          uint16_t payloadLength,
                                          SfrbxHeader &header) {
  if (payload == 0 || payloadLength < 8U) return SfrbxParseStatus::TooShort;
  header.gnssId = payload[0];
  header.svId = payload[1];
  header.sigId = payload[2];
  header.freqId = payload[3];
  header.numWords = payload[4];
  header.channel = payload[5];
  header.version = payload[6];
  if (header.numWords > 16U) return SfrbxParseStatus::InvalidWordCount;
  if (payloadLength != static_cast<uint16_t>(8U + 4U * header.numWords))
    return SfrbxParseStatus::LengthMismatch;
  return SfrbxParseStatus::Ok;
}

bool isValidL1sPreamble(uint8_t pab) {
  return pab == 0x53U || pab == 0x9AU || pab == 0xC6U;
}

bool isSupportedQzssL1sWordCount(uint8_t numWords) {
  // MAX-M10S SPG 5.10 の現場取得で 9 word を確認済み。既存の Switch Science/試験ベクトル用に 8 word も維持するが、任意の 8 以上の個数は受理しない。
  return numWords == QZSS_L1S_M10_WORDS ||
         numWords == QZSS_L1S_LEGACY_WORDS;
}

uint32_t qzssL1sCrc24(const uint8_t raw[QZSS_L1S_RAW_BYTES]) {
  // IS-QZSS-L1S-009 4.1.1.3。x^24 を除く多項式: x^23+x^18+x^17+x^14+x^11+x^10+x^7+x^6+x^5+x^4+x^3+x+1。
  static const uint32_t polynomial = 0x864CFBU;
  uint32_t crc = 0;
  for (size_t i = 0; i < QZSS_L1S_CRC_DATA_BITS; ++i) {
    const uint32_t input =
        (static_cast<uint32_t>(raw[i >> 3]) >> (7U - (i & 7U))) & 1U;
    const uint32_t feedback = ((crc >> 23) & 1U) ^ input;
    crc = (crc << 1) & 0xFFFFFFU;
    if (feedback != 0U) crc ^= polynomial;
  }
  return crc;
}

uint32_t qzssL1sReceivedCrc(const uint8_t raw[QZSS_L1S_RAW_BYTES]) {
  BitReader bits(raw, QZSS_L1S_VALID_BITS);
  uint32_t crc = 0;
  (void)bits.read(QZSS_L1S_CRC_DATA_BITS, 24U, crc);
  return crc;
}

uint32_t qzssL1sHash(const uint8_t raw[QZSS_L1S_RAW_BYTES]) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < 31U; ++i) {
    hash ^= raw[i];
    hash *= 16777619UL;
  }
  hash ^= static_cast<uint8_t>(raw[31] & 0xC0U);
  hash *= 16777619UL;
  return hash;
}

bool extractQzssL1s(const uint8_t *payload, uint16_t payloadLength,
                    QzssL1sFrame &frame) {
  SfrbxHeader header;
  if (!parseSfrbxHeader(payload, payloadLength, header) ||
      header.gnssId != 5U || header.sigId != 1U ||
      !isSupportedQzssL1sWordCount(header.numWords)) {
    return false;
  }

  // UBX は U4 dwrd をリトルエンディアンで格納し、数値の bit 31 が先に送信される。各 word 内の byte 順だけを反転する。
  for (size_t word = 0; word < QZSS_L1S_CONTENT_WORDS; ++word) {
    const size_t source = 8U + word * 4U;
    const size_t dest = word * 4U;
    frame.raw[dest] = payload[source + 3U];
    frame.raw[dest + 1U] = payload[source + 2U];
    frame.raw[dest + 2U] = payload[source + 1U];
    frame.raw[dest + 3U] = payload[source];
  }
  frame.numWords = header.numWords;
  frame.hasExtraWord = header.numWords == QZSS_L1S_M10_WORDS;
  memset(frame.extraWordUbx, 0, sizeof(frame.extraWordUbx));
  if (frame.hasExtraWord)
    memcpy(frame.extraWordUbx, payload + 8U + 4U * QZSS_L1S_CONTENT_WORDS,
           sizeof(frame.extraWordUbx));
  frame.svId = header.svId;
  frame.pab = frame.raw[0];
  BitReader bits(frame.raw, QZSS_L1S_VALID_BITS);
  uint32_t mt = 0;
  if (!bits.read(8U, 6U, mt)) return false;
  frame.messageType = static_cast<uint8_t>(mt);
  frame.validPab = isValidL1sPreamble(frame.pab);
  frame.validCrc = qzssL1sCrc24(frame.raw) == qzssL1sReceivedCrc(frame.raw);
  frame.duplicate = false;
  return true;
}

L1sDeduplicator::L1sDeduplicator() : next_(0) {
  memset(entries_, 0, sizeof(entries_));
}

bool L1sDeduplicator::checkAndRemember(
    const uint8_t raw[QZSS_L1S_RAW_BYTES], uint64_t receivedMicros,
    uint64_t windowMicros) {
  const uint32_t hash = qzssL1sHash(raw);
  for (size_t i = 0; i < CAPACITY; ++i) {
    Entry &entry = entries_[i];
    if (!entry.used || entry.hash != hash ||
        receivedMicros < entry.receivedMicros ||
        receivedMicros - entry.receivedMicros > windowMicros) {
      continue;
    }
    if (memcmp(entry.raw, raw, 31U) == 0 &&
        ((entry.raw[31] ^ raw[31]) & 0xC0U) == 0U) {
      entry.receivedMicros = receivedMicros;
      return true;
    }
  }

  Entry &entry = entries_[next_];
  memcpy(entry.raw, raw, QZSS_L1S_RAW_BYTES);
  entry.raw[31] &= 0xC0U;  // SFRBX の padding 6 bit は通報内容に含めない。
  entry.hash = hash;
  entry.receivedMicros = receivedMicros;
  entry.used = true;
  next_ = (next_ + 1U) % CAPACITY;
  return false;
}
